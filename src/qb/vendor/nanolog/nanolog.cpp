/*

Distributed under the MIT License (MIT)

    Copyright (c) 2016 Karthik Iyengar

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in the
Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished
to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS
OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

*/

#include "nanolog.h"
#include <algorithm>
#include <atomic>
#include <new> // std::align_val_t for the over-aligned ring allocation
#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <limits>
#include <mutex> // qb fork (Huly QB-1207)
#include <qb/system/time.h>
#include <qb/utility/build_macros.h>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#ifndef _WIN32
#include <pthread.h> // qb fork (Huly QB-1246): pthread_atfork
#endif

#ifndef NANOLOG_TEST_HOOK
#define NANOLOG_TEST_HOOK(phase, index) ((void) 0)
#endif

namespace {

// qb fork (Huly QB-1246): how many fork()s separate this process from the one that loaded nanolog, counted in each
// child by a pthread_atfork handler that only increments an integer -- it takes no lock (Huly QB-1193). fork() copies
// the calling thread only: a logger records the generation its writer thread started in, and a child, one generation
// further, never joins, drains or takes back a logger of its parent. Registered by the first call, which comes before
// any logger exists; a generation, unlike a process id, is never reused. Windows has no fork(): always 0. Not counted:
// a child of _Fork() or of a raw clone (no handler runs), and every child if pthread_atfork failed -- the old behaviour.
std::atomic<unsigned long> fork_generation{0};

#ifndef _WIN32
void
count_fork_in_child() noexcept {
    fork_generation.fetch_add(1, std::memory_order_relaxed);
}
#endif

unsigned long
current_fork_generation() noexcept {
#ifndef _WIN32
    static bool const counted = ::pthread_atfork(nullptr, nullptr, &count_fork_in_child) == 0;
    (void) counted;
#endif
    return fork_generation.load(std::memory_order_relaxed);
}

/* Returns microseconds since epoch */
uint64_t
timestamp_now() {
    return static_cast<uint64_t>(qb::unix_micros(qb::wall_now()));
}

/* I want [2016-10-13 00:01:23.528514] */
void
format_timestamp(std::ostream &os, uint64_t timestamp) {
    // The next 3 lines do not work on MSVC!
    // auto duration = std::chrono::microseconds(timestamp);
    // std::chrono::high_resolution_clock::time_point time_point(duration);
    // std::time_t time_t = std::chrono::high_resolution_clock::to_time_t(time_point);
    std::time_t const time_t = static_cast<std::time_t>(timestamp / 1000000); // qb fork (Huly QB-1207), with the next lines
    // qb fork (Huly QB-1207): two writer threads can format at once -- a replaced logger's last drain and the new
    // logger's writer -- so the breakdown is qb's re-entrant one: std::gmtime returns one static object on glibc.
    std::tm gmtime{};
    (void) qb::safe_gmtime(time_t, gmtime);
    char buffer[32];
    strftime(buffer, 32, "%Y-%m-%d %T.", &gmtime); // qb fork (Huly QB-1207): the local breakdown
    char microseconds[7];
    snprintf(microseconds, 7, "%06llu", static_cast<unsigned long long>(timestamp % 1000000));
    os << '[' << buffer << microseconds << ']';
}

std::thread::id
this_thread_id() {
    static thread_local const std::thread::id id = std::this_thread::get_id();
    return id;
}

template <typename T, typename Tuple>
struct TupleIndex;

template <typename T, typename... Types>
struct TupleIndex<T, std::tuple<T, Types...>> {
    static constexpr const std::size_t value = 0;
};

template <typename T, typename U, typename... Types>
struct TupleIndex<T, std::tuple<U, Types...>> {
    static constexpr const std::size_t value = 1 + TupleIndex<T, std::tuple<Types...>>::value;
};

} // anonymous namespace

namespace nanolog {
typedef std::tuple<char, uint32_t, uint64_t, int32_t, int64_t, double, NanoLogLine::string_literal_t, char *> SupportedTypes;

char const *
to_string(LogLevel loglevel) {
    switch (loglevel) {
        case LogLevel::DEBUG:
            return "DEBUG";
        case LogLevel::VERBOSE:
            return "VERB";
        case LogLevel::INFO:
            return "INFO";
        case LogLevel::WARN:
            return "WARN";
        case LogLevel::CRIT:
            return "CRIT";
    }
    return "XXXX";
}

namespace {
template <typename T>
char *
put(char *b, T const &value) {
    std::memcpy(b, &value, sizeof(T));
    return b + sizeof(T);
}
} // namespace

NanoLogLine::NanoLogLine(LogLevel level, char const *file, char const *function, uint32_t line)
    : m_bytes_used(0)
    , m_buffer_size(sizeof(m_stack_buffer)) {
    // The header, straight into the stack buffer -- a new record has no heap buffer, and the header always fits -- in
    // the order stringify(std::ostream &) reads it back (Huly QB-1004: it was six encodes, each selecting the buffer).
    static_assert(sizeof(uint64_t) + sizeof(std::thread::id) + 2 * sizeof(string_literal_t) + sizeof(uint32_t) + sizeof(LogLevel)
                      <= sizeof(m_stack_buffer),
                  "the record header fits the stack buffer");
    char *b      = m_stack_buffer;
    b            = put(b, timestamp_now());
    b            = put(b, this_thread_id());
    b            = put(b, string_literal_t(file));
    b            = put(b, string_literal_t(function));
    b            = put(b, line);
    b            = put(b, level);
    m_bytes_used = static_cast<size_t>(b - m_stack_buffer);
}

void
NanoLogLine::stringify(std::ostream &os) {
    char             *b   = !m_heap_buffer ? m_stack_buffer : m_heap_buffer.get();
    char const *const end = b + m_bytes_used;
    uint64_t          timestamp;
    std::memcpy(&timestamp, b, sizeof(timestamp));
    b += sizeof(uint64_t);
    std::thread::id threadid;
    std::memcpy(&threadid, b, sizeof(threadid));
    b += sizeof(std::thread::id);
    // string_literal_t file = *reinterpret_cast < string_literal_t * >(b);
    b += sizeof(string_literal_t);
    string_literal_t function(nullptr);
    std::memcpy(&function, b, sizeof(function));
    b += sizeof(string_literal_t);
    uint32_t line;
    std::memcpy(&line, b, sizeof(line));
    b += sizeof(uint32_t);
    LogLevel loglevel;
    std::memcpy(&loglevel, b, sizeof(loglevel));
    b += sizeof(LogLevel);

    format_timestamp(os, timestamp);

    os << '[' << to_string(loglevel) << ']' << '[' << threadid << ']'
       << '['
       //<< file.m_s << ':'
       << function.m_s << ':' << line << "] ";

    stringify(os, b, end);

    os << std::endl;

    if (loglevel >= LogLevel::CRIT)
        os.flush();
}

template <typename Arg>
char *
decode(std::ostream &os, char *b, Arg *) {
    Arg arg;
    std::memcpy(&arg, b, sizeof(arg));
    os << arg;
    return b + sizeof(Arg);
}

template <>
char *
decode(std::ostream &os, char *b, NanoLogLine::string_literal_t *) {
    NanoLogLine::string_literal_t s(nullptr);
    std::memcpy(&s, b, sizeof(s));
    os << s.m_s;
    return b + sizeof(NanoLogLine::string_literal_t);
}

// A text field: its 32-bit length, then exactly that many bytes, NUL bytes included (see encode_c_string).
template <>
char *
decode(std::ostream &os, char *b, char **) {
    uint32_t length;
    std::memcpy(&length, b, sizeof(length));
    b += sizeof(length);
    os.write(b, static_cast<std::streamsize>(length));
    return b + length;
}

void
NanoLogLine::stringify(std::ostream &os, char *start, char const *const end) {
    if (start == end)
        return;

    int type_id = static_cast<int>(*start);
    start++;

    switch (type_id) {
        case 0:
            stringify(os, decode(os, start, static_cast<std::tuple_element<0, SupportedTypes>::type *>(nullptr)), end);
            return;
        case 1:
            stringify(os, decode(os, start, static_cast<std::tuple_element<1, SupportedTypes>::type *>(nullptr)), end);
            return;
        case 2:
            stringify(os, decode(os, start, static_cast<std::tuple_element<2, SupportedTypes>::type *>(nullptr)), end);
            return;
        case 3:
            stringify(os, decode(os, start, static_cast<std::tuple_element<3, SupportedTypes>::type *>(nullptr)), end);
            return;
        case 4:
            stringify(os, decode(os, start, static_cast<std::tuple_element<4, SupportedTypes>::type *>(nullptr)), end);
            return;
        case 5:
            stringify(os, decode(os, start, static_cast<std::tuple_element<5, SupportedTypes>::type *>(nullptr)), end);
            return;
        case 6:
            stringify(os, decode(os, start, static_cast<std::tuple_element<6, SupportedTypes>::type *>(nullptr)), end);
            return;
        case 7:
            stringify(os, decode(os, start, static_cast<std::tuple_element<7, SupportedTypes>::type *>(nullptr)), end);
            return;
    }
}

void
NanoLogLine::resize_buffer_if_needed(size_t additional_bytes) {
    size_t const required_size = m_bytes_used + additional_bytes;

    if (required_size <= m_buffer_size)
        return;

    if (!m_heap_buffer) {
        m_buffer_size = std::max(static_cast<size_t>(512), required_size);
        m_heap_buffer.reset(new char[m_buffer_size]);
        memcpy(m_heap_buffer.get(), m_stack_buffer, m_bytes_used);
        return;
    } else {
        m_buffer_size = std::max(static_cast<size_t>(2 * m_buffer_size), required_size);
        std::unique_ptr<char[]> new_heap_buffer(new char[m_buffer_size]);
        memcpy(new_heap_buffer.get(), m_heap_buffer.get(), m_bytes_used);
        m_heap_buffer.swap(new_heap_buffer);
    }
}

// The out-of-line writer of a scalar field: the buffer could not hold it (append_scalar, nanolog.h).
void
NanoLogLine::encode_scalar(void const *value, size_t size, Tag tag) {
    // The header's tags are the indices of SupportedTypes, which stringify() switches on.
    static_assert(TupleIndex<char, SupportedTypes>::value == tag_char, "tag_char");
    static_assert(TupleIndex<uint32_t, SupportedTypes>::value == tag_uint32, "tag_uint32");
    static_assert(TupleIndex<uint64_t, SupportedTypes>::value == tag_uint64, "tag_uint64");
    static_assert(TupleIndex<int32_t, SupportedTypes>::value == tag_int32, "tag_int32");
    static_assert(TupleIndex<int64_t, SupportedTypes>::value == tag_int64, "tag_int64");
    static_assert(TupleIndex<double, SupportedTypes>::value == tag_double, "tag_double");
    static_assert(TupleIndex<string_literal_t, SupportedTypes>::value == tag_literal, "tag_literal");
    static_assert(TupleIndex<char *, SupportedTypes>::value == tag_text, "tag_text");
    resize_buffer_if_needed(1 + size);
    char *const b = cursor();
    b[0]          = static_cast<char>(tag);
    std::memcpy(b + 1, value, size);
    m_bytes_used += 1 + size;
}

// qb fork (Huly QB-367): a text field is its type tag, its length on 32 bits, then its bytes -- all of them, NUL bytes
// included. It was NUL-terminated and decoded up to the first NUL, so a std::string holding one cut the record there
// and the decoder read the rest of the text as type tags. A text past 4 GiB is written as consecutive fields, which
// read back as the same bytes. Out of line: the buffer could not hold the field (append_text, nanolog.h).
void
NanoLogLine::encode_c_string(char const *arg, size_t length) {
    for (;;) {
        auto const field = static_cast<uint32_t>(length < max_text_field ? length : max_text_field);
        if (!field)
            return;
        resize_buffer_if_needed(text_field_overhead + field);
        char *const b = cursor();
        b[0]          = static_cast<char>(tag_text);
        std::memcpy(b + 1, &field, sizeof(field));
        std::memcpy(b + text_field_overhead, arg, field);
        m_bytes_used += text_field_overhead + field;
        if (field == length) [[likely]]
            return;
        arg += field;
        length -= field;
    }
}

struct BufferBase {
    virtual ~BufferBase()                      = default;
    virtual void push(NanoLogLine &&logline)   = 0;
    virtual bool try_pop(NanoLogLine &logline) = 0;
};

struct SpinLock {
    SpinLock(std::atomic_flag &flag)
        : m_flag(flag) {
        while (m_flag.test_and_set(std::memory_order_acquire))
            ;
    }

    ~SpinLock() {
        m_flag.clear(std::memory_order_release);
    }

private:
    std::atomic_flag &m_flag;
};

/* Multi Producer Single Consumer Ring Buffer */
class RingBuffer : public BufferBase {
public:
    struct alignas(64) Item {
        Item()
            : written(0)
            , logline(LogLevel::INFO, nullptr, nullptr, 0) {}

        std::atomic_flag flag{};
        char             written;
        char             padding[256 - sizeof(std::atomic_flag) - sizeof(char) - sizeof(NanoLogLine)];
        NanoLogLine      logline;
    };

    RingBuffer(size_t const size)
        : m_size(size)
        // `Item` is alignas(64) to keep producers off each other's cache lines. std::malloc only
        // guarantees __STDCPP_DEFAULT_NEW_ALIGNMENT__ (16 here), so placement-new'ing an
        // over-aligned type into malloc'd storage is undefined behaviour — it merely happened to
        // work because this allocator returns 64-aligned blocks for large requests (measured:
        // 200/200 at the ring's real size, but 8/400 for small ones, and Windows' allocator is a
        // different implementation entirely). The aligned operator new states the requirement
        // instead of relying on the allocator's luck.
        , m_ring(static_cast<Item *>(::operator new(size * sizeof(Item), std::align_val_t{alignof(Item)})))
        , m_write_index(0)
        , m_read_index(0) {
        for (size_t i = 0; i < m_size; ++i) {
            new (&m_ring[i]) Item();
        }
        static_assert(sizeof(Item) == 256, "Unexpected size != 256");
    }

    ~RingBuffer() {
        for (size_t i = 0; i < m_size; ++i) {
            m_ring[i].~Item();
        }
        ::operator delete(m_ring, std::align_val_t{alignof(Item)}); // UNSIZED: the sized+aligned global needs __cpp_sized_deallocation
    }

    void
    push(NanoLogLine &&logline) override {
        unsigned int write_index = m_write_index.fetch_add(1, std::memory_order_relaxed) % m_size;
        Item        &item        = m_ring[write_index];
        SpinLock     spinlock(item.flag);
        item.logline = std::move(logline);
        item.written = 1;
    }

    bool
    try_pop(NanoLogLine &logline) override {
        Item    &item = m_ring[m_read_index % m_size];
        SpinLock spinlock(item.flag);
        if (item.written == 1) {
            logline      = std::move(item.logline);
            item.written = 0;
            ++m_read_index;
            return true;
        }
        return false;
    }

    RingBuffer(RingBuffer const &)            = delete;
    RingBuffer &operator=(RingBuffer const &) = delete;

private:
    size_t const              m_size;
    Item                     *m_ring;
    std::atomic<unsigned int> m_write_index;
    QB_UNUSED_VAR char        pad[64];
    unsigned int              m_read_index;
};

class Buffer {
public:
    struct Item {
        Item(NanoLogLine &&nanologline)
            : logline(std::move(nanologline)) {}
        char        padding[256 - sizeof(NanoLogLine)];
        NanoLogLine logline;
    };

    static constexpr const size_t size = 32768; // 8MB. Helps reduce memory fragmentation

    Buffer()
        : m_buffer(static_cast<Item *>(std::malloc(size * sizeof(Item)))) {
        for (size_t i = 0; i <= size; ++i) {
            m_write_state[i].store(0, std::memory_order_relaxed);
        }
        static_assert(sizeof(Item) == 256, "Unexpected size != 256");
    }

    ~Buffer() {
        unsigned int write_count = m_write_state[size].load();
        for (size_t i = 0; i < write_count; ++i) {
            m_buffer[i].~Item();
        }
        std::free(m_buffer);
        NANOLOG_TEST_HOOK(destroyed, 0);
    }

    // Returns true if we need to switch to next buffer
    bool
    push(NanoLogLine &&logline, unsigned int const write_index) {
        new (&m_buffer[write_index]) Item(std::move(logline));
        NANOLOG_TEST_HOOK(before_publish, write_index);
        // The ready store is the last access to this Buffer. A consumer may retire the whole
        // Buffer as soon as it acquires the final ready slot, even before this call returns.
        bool const rotate = m_write_state[size].fetch_add(1, std::memory_order_acquire) + 1 == size;
        m_write_state[write_index].store(1, std::memory_order_release);
        NANOLOG_TEST_HOOK(after_publish, write_index);
        return rotate;
    }

    bool
    try_pop(NanoLogLine &logline, unsigned int const read_index) {
        if (m_write_state[read_index].load(std::memory_order_acquire)) {
            Item &item = m_buffer[read_index];
            logline    = std::move(item.logline);
            return true;
        }
        return false;
    }

    Buffer(Buffer const &)            = delete;
    Buffer &operator=(Buffer const &) = delete;

private:
    Item                     *m_buffer;
    std::atomic<unsigned int> m_write_state[size + 1];
};

class QueueBuffer : public BufferBase {
public:
    QueueBuffer(QueueBuffer const &)            = delete;
    QueueBuffer &operator=(QueueBuffer const &) = delete;

    QueueBuffer()
        : m_current_read_buffer{nullptr}
        , m_write_index(0)
        , m_read_index(0) {
        setup_next_write_buffer();
    }

    void
    push(NanoLogLine &&logline) override {
        unsigned int write_index = m_write_index.fetch_add(1, std::memory_order_relaxed);
        if (write_index < Buffer::size) {
            if (m_current_write_buffer.load(std::memory_order_acquire)->push(std::move(logline), write_index)) {
                setup_next_write_buffer();
            }
        } else {
            while (m_write_index.load(std::memory_order_acquire) >= Buffer::size)
                ;
            push(std::move(logline));
        }
    }

    bool
    try_pop(NanoLogLine &logline) override {
        if (m_current_read_buffer == nullptr)
            m_current_read_buffer = get_next_read_buffer();

        Buffer *read_buffer = m_current_read_buffer;

        if (read_buffer == nullptr)
            return false;

        if (read_buffer->try_pop(logline, m_read_index)) {
            m_read_index++;
            if (m_read_index == Buffer::size) {
                m_read_index          = 0;
                m_current_read_buffer = nullptr;
                SpinLock spinlock(m_flag);
                m_buffers.pop();
            }
            return true;
        }

        return false;
    }

private:
    void
    setup_next_write_buffer() {
        std::unique_ptr<Buffer> next_write_buffer(new Buffer());
        m_current_write_buffer.store(next_write_buffer.get(), std::memory_order_release);
        SpinLock spinlock(m_flag);
        m_buffers.push(std::move(next_write_buffer));
        m_write_index.store(0, std::memory_order_relaxed);
    }

    Buffer *
    get_next_read_buffer() {
        SpinLock spinlock(m_flag);
        return m_buffers.empty() ? nullptr : m_buffers.front().get();
    }

private:
    std::queue<std::unique_ptr<Buffer>> m_buffers;
    std::atomic<Buffer *>               m_current_write_buffer;
    Buffer                             *m_current_read_buffer;
    std::atomic<unsigned int>           m_write_index;
    std::atomic_flag                    m_flag{};
    unsigned int                        m_read_index;
};

// qb fork (Huly QB-368): the items of a NonGuaranteedLogger ring of `ring_buffer_size_mb` megabytes, at least one --
// 4096 items of 256 bytes per MB, on 64 bits. The product was taken in 32 bits and wrapped to ZERO at 1048576 MB: an
// empty ring, whose writer thread computed `index % 0`. The ring's indices are 32-bit counters, so a ring of more
// items than they count is refused here: before its slots are allocated, its file opened or its writer thread started.
static std::size_t
ring_buffer_items(uint32_t ring_buffer_size_mb) {
    constexpr std::uint64_t items_per_mb = 1024 * 1024 / sizeof(RingBuffer::Item);
    std::uint64_t const     items        = std::uint64_t{std::max(1u, ring_buffer_size_mb)} * items_per_mb;
    if (items > std::numeric_limits<unsigned int>::max())
        throw std::length_error("nanolog: a NonGuaranteedLogger ring of " + std::to_string(ring_buffer_size_mb)
                                + " MB has more items than its 32-bit indices count (at most "
                                + std::to_string(std::numeric_limits<unsigned int>::max() / items_per_mb) + " MB)");
    return static_cast<std::size_t>(items);
}

class FileWriter {
public:
    FileWriter(std::filesystem::path log_file_path, uint32_t log_file_roll_size_mb)
        // 64-bit multiply + storage: log_file_roll_size_mb * 1024 * 1024 would
        // overflow uint32_t for roll sizes >= 4096 MB, wrapping to a tiny (or
        // zero) byte budget that rolls the file on almost every write.
        : m_log_file_roll_size_bytes(static_cast<uint64_t>(log_file_roll_size_mb) * 1024 * 1024)
        , m_name(std::move(log_file_path)) {
        roll_file();
    }

    // qb fork (Huly QB-327): whether the file being written was opened. The constructor's roll opens the first one.
    bool
    is_open() const noexcept {
        return m_os && m_os->is_open();
    }

    void
    write(NanoLogLine &logline) {
        auto pos = m_os->tellp();
        logline.stringify(*m_os);
        m_bytes_written += m_os->tellp() - pos;
        // m_bytes_written (std::streamoff, signed) is always >= 0 here; cast to
        // match the unsigned roll-size and avoid a signed/unsigned comparison
        // warning (the roll size became uint64_t to fix the >=4GB overflow).
        if (static_cast<std::uint64_t>(m_bytes_written) > m_log_file_roll_size_bytes) {
            roll_file();
        }
    }

private:
    void
    roll_file() {
        if (m_os) {
            m_os->flush();
            m_os->close();
        }

        m_bytes_written = 0;
        m_os.reset(new std::ofstream());
        // TODO Optimize this part. Does it even matter ?
        std::filesystem::path log_file_name = m_name;
        log_file_name += ".";
        log_file_name += std::to_string(++m_file_number);
        log_file_name += ".log";
        m_os->open(log_file_name, std::ofstream::out | std::ofstream::trunc);
    }

private:
    uint32_t                       m_file_number   = 0;
    std::streamoff                 m_bytes_written = 0;
    uint64_t const                 m_log_file_roll_size_bytes;
    std::filesystem::path const    m_name;
    std::unique_ptr<std::ofstream> m_os;
};

class NanoLogger {
public:
    NanoLogger(NonGuaranteedLogger ngl, std::filesystem::path const &log_file_path, uint32_t log_file_roll_size_mb)
        : m_state(State::INIT)
        , m_buffer_base(new RingBuffer(ring_buffer_items(ngl.ring_buffer_size_mb)))
        , m_file_writer(std::make_unique<FileWriter>(log_file_path, std::max(1u, log_file_roll_size_mb))) // qb fork (Huly QB-1207)
        , m_file_opened(m_file_writer->is_open())                  // qb fork (Huly QB-1207): through the pointer
        , m_ring_items(ring_buffer_items(ngl.ring_buffer_size_mb)) // qb fork (Huly QB-1207)
        , m_writer_generation(current_fork_generation())           // qb fork (Huly QB-1246)
        , m_thread(&NanoLogger::pop, this) {
        m_state.store(State::READY, std::memory_order_release);
    }

    NanoLogger(GuaranteedLogger, std::filesystem::path const &log_file_path, uint32_t log_file_roll_size_mb)
        : m_state(State::INIT)
        , m_buffer_base(new QueueBuffer())
        , m_file_writer(std::make_unique<FileWriter>(log_file_path, std::max(1u, log_file_roll_size_mb))) // qb fork (Huly QB-1207)
        , m_file_opened(m_file_writer->is_open())        // qb fork (Huly QB-1207): through the pointer
        , m_ring_items(0)                                // qb fork (Huly QB-1207)
        , m_writer_generation(current_fork_generation()) // qb fork (Huly QB-1246)
        , m_thread(&NanoLogger::pop, this) {
        m_state.store(State::READY, std::memory_order_release);
    }

    // qb fork (Huly QB-1207): whether the first file of this logger's current life -- its constructor's, or restart()'s
    // -- was opened: taken before that writer thread writes, so never torn by a roll.
    bool
    file_opened() const noexcept {
        return m_file_opened;
    }

    ~NanoLogger() {
        // qb fork (Huly QB-1207): a logger initialize() created is never destroyed; one built directly is. qb fork (Huly
        // QB-1246): destroyed in a child of fork(), its members still flush and close the copy of its parent's writer.
        stop();
    }

    void
    add(NanoLogLine &&logline) {
        m_buffer_base->push(std::move(logline));
    }

    // qb fork (Huly QB-1207): the writer thread drains what the queue holds into the file, the file is closed, and the
    // logger stays: a thread that read it before initialize() published the next one may still be inside add(), so its
    // queue must outlive that call. What such a thread leaves in the queue is written when restart() takes it back.
    void
    stop() {
        m_state.store(State::SHUTDOWN);
        if (!owned_here()) {
            // qb fork (Huly QB-1246): a child of fork() stops a logger of its parent -- at its exit, or replacing it.
            // The writer thread is the parent's, absent here: it is forgotten, never joined -- a join of a thread this
            // process does not have is undefined (libc++ on macOS refuses it, and the child died of std::terminate).
            // The queue and the file writer are left as copied -- the parent writes them -- so the child neither
            // drains the parent's records nor flushes the parent's buffered bytes.
            new (&m_thread) std::thread();
            return;
        }
        NANOLOG_TEST_HOOK(stopping, 0); // the exit test releases the writer here: it sees SHUTDOWN, only the drain is left
        if (m_thread.joinable())
            m_thread.join();
        m_file_writer.reset();
    }

    // qb fork (Huly QB-1207): a stopped logger writes to a new file, with a new writer thread; what its queue holds is
    // written there first. The file is opened, then the thread started: a throw from either leaves the logger stopped,
    // its queue as it was (the file, created by then, stays).
    void
    restart(std::filesystem::path const &log_file_path, uint32_t log_file_roll_size_mb) {
        auto       writer = std::make_unique<FileWriter>(log_file_path, std::max(1u, log_file_roll_size_mb));
        bool const opened = writer->is_open();
        m_state.store(State::INIT, std::memory_order_relaxed);
        m_writer_generation = current_fork_generation(); // qb fork (Huly QB-1246): the new writer's generation
        try {
            m_thread = std::thread(&NanoLogger::pop, this); // waits for READY, below
        } catch (...) {
            m_state.store(State::SHUTDOWN, std::memory_order_relaxed);
            throw;
        }
        m_file_writer = std::move(writer);
        m_file_opened = opened;
        m_state.store(State::READY, std::memory_order_release);
    }

    // qb fork (Huly QB-1246): whether this process started the writer thread -- false in a child of fork() for every
    // logger its parent created. Such a logger is never joined, drained or taken back here.
    bool
    owned_here() const noexcept {
        return m_writer_generation == current_fork_generation();
    }

    // qb fork (Huly QB-1207): the queue initialize() can reuse this logger for: 0 is a GuaranteedLogger's, any other value
    // a ring of that many items.
    std::size_t
    ring_items() const noexcept {
        return m_ring_items;
    }

    void
    pop() {
        // Wait for constructor to complete and pull all stores done there to this thread
        // / core.
        while (m_state.load(std::memory_order_acquire) == State::INIT)
            std::this_thread::sleep_for(std::chrono::microseconds(50));

        NanoLogLine logline(LogLevel::INFO, nullptr, nullptr, 0);

        while (m_state.load() == State::READY) {
            if (m_buffer_base->try_pop(logline)) {
                m_file_writer->write(logline);      // qb fork (Huly QB-1207): through the pointer restart() replaces
                NANOLOG_TEST_HOOK(writer_wrote, 0); // qb fork (Huly QB-1207): the exit test holds the writer here
            } else
                std::this_thread::sleep_for(std::chrono::microseconds(50));
        }

        // Pop and log all remaining entries
        NANOLOG_TEST_HOOK(writer_draining, 0); // qb fork (Huly QB-1207): the test holds a replaced logger here
        while (m_buffer_base->try_pop(logline)) {
            m_file_writer->write(logline); // qb fork (Huly QB-1207): through the pointer restart() replaces
        }
    }

private:
    enum class State { INIT, READY, SHUTDOWN };

    std::atomic<State>          m_state;
    std::unique_ptr<BufferBase> m_buffer_base;
    std::unique_ptr<FileWriter> m_file_writer; // qb fork (Huly QB-1207): replaced by restart(), so on the heap
    // qb fork (Huly QB-1207): m_file_opened is no longer const, restart() sets it.
    bool              m_file_opened;       // after m_file_writer, before m_thread: initialisation order matters
    std::size_t const m_ring_items;        // qb fork (Huly QB-1207)
    unsigned long     m_writer_generation; // qb fork (Huly QB-1246): the fork generation of the writer thread
    std::thread       m_thread;

public:
    // qb fork (Huly QB-1207): initialize()'s bookkeeping, touched under its lock only.
    NanoLogger   *next       = nullptr; // the logger initialize() created before this one (see `loggers`)
    std::uint64_t stopped_at = 0;       // when initialize() stopped it, in replacements: the oldest is taken back first
};

// qb fork (Huly QB-1207): initialize() REPLACES the published logger, and a thread may log while it does. It used to
// destroy the old logger -- its writer, its queue -- and only then publish the new one: a thread that had read the old
// pointer, or read it during the destruction, pushed into freed memory, and every thread kept pushing into the old one
// while it drained, so a thread logging faster than the writer kept initialize() from returning. Now the new logger is
// published first and the old one then stopped: it drains what it already holds -- every record up to the first one a
// thread is still writing -- and closes its file before initialize() returns. No logger initialize() created is ever
// freed: a thread can hold one from its read of `atomic_nanologger` to the end of add() for as long as it is preempted,
// and nothing on that path may pay to say so. A stopped logger is taken back by a later initialize() for the same queue
// instead -- the one stopped longest ago first -- so a process holds at most two loggers per queue it asked for (a
// child of fork() holds its parent's too); a record a late thread left in one is written then, into that call's file,
// and lost if no later call takes it back.
std::atomic<NanoLogger *> atomic_nanologger;

namespace {
// qb fork (Huly QB-1207): initialize() against itself, and log_file_opened(). Never destroyed: a static destructor
// that runs after StopAtExit's may still call either.
std::mutex &
initialize_mutex() noexcept {
    alignas(std::mutex) static unsigned char storage[sizeof(std::mutex)];
    static std::mutex *const                 mutex = new (storage) std::mutex();
    return *mutex;
}

NanoLogger   *loggers      = nullptr; // every logger initialize() created, the newest first, linked by `next`
std::uint64_t replacements = 0;       // the clock of NanoLogger::stopped_at

// What log_file_opened() answers on a thread that called initialize(): that call's open, not a racing call's (-1: none).
thread_local int last_open = -1;
// qb fork (Huly QB-1246): the fork generation last_open was answered in -- a child's copy of it is its parent's.
thread_local unsigned long last_open_generation = 0;

// qb fork (Huly QB-1207): at exit the published logger is stopped, so that its file is complete -- what destroying it
// did. It is not freed: a static destructor that runs later and logs pushes into a queue nobody writes, where it used
// to write into freed memory.
// It only tries the lock: when qb-io's statics live in a Windows DLL (qb-io built as one, or linked into one), they are
// destroyed after ExitProcess ended every other thread, and one may have died holding it -- the writer too, so nothing
// is drained there, as before. Anywhere else, a thread inside initialize() or log_file_opened() at that moment leaves
// the published logger running.
struct StopAtExit {
    constexpr StopAtExit() noexcept = default;
    ~StopAtExit() {
        std::unique_lock<std::mutex> lock(initialize_mutex(), std::try_to_lock);
        if (!lock.owns_lock())
            return;
        if (NanoLogger *const published = atomic_nanologger.load(std::memory_order_relaxed))
            published->stop();
    }
};
StopAtExit stop_at_exit;
} // namespace

bool
NanoLog::operator==(NanoLogLine &logline) {
    atomic_nanologger.load(std::memory_order_acquire)->add(std::move(logline));
    return true;
}

namespace {
// qb fork (Huly QB-1207): the logger for a queue of `ring_items` (0: guaranteed) -- the stopped one stopped longest ago
// taken back, or a new one `make` builds -- published, then the one it replaces stopped. A throw before the publication
// leaves the published logger as it was (the new file, created by then, stays).
template <typename Make>
void
publish_logger(std::size_t const ring_items, Make const &make, std::filesystem::path const &log_file_path,
               uint32_t const log_file_roll_size_mb) {
    std::lock_guard<std::mutex> lock(initialize_mutex());
    NanoLogger *const           replaced = atomic_nanologger.load(std::memory_order_relaxed);
    NanoLogger                 *next     = nullptr;
    for (NanoLogger *logger = loggers; logger != nullptr; logger = logger->next)
        if (logger != replaced && logger->ring_items() == ring_items && logger->owned_here() // qb fork (Huly QB-1246)
            && (next == nullptr || logger->stopped_at < next->stopped_at))
            next = logger;
    if (next != nullptr) {
        next->restart(log_file_path, log_file_roll_size_mb);
    } else {
        next       = make();
        next->next = loggers;
        loggers    = next;
    }
    last_open            = next->file_opened() ? 1 : 0;
    last_open_generation = current_fork_generation(); // qb fork (Huly QB-1246)
    atomic_nanologger.store(next, std::memory_order_seq_cst);
    if (replaced != nullptr) {
        replaced->stop();
        replaced->stopped_at = ++replacements;
    }
}
} // namespace

// qb fork (Huly QB-1207): both overloads publish through publish_logger(), which replaces the logger safely.
void
initialize(NonGuaranteedLogger ngl, std::filesystem::path const &log_file_path, uint32_t log_file_roll_size_mb) {
    publish_logger(
        ring_buffer_items(ngl.ring_buffer_size_mb), [&] { return new NanoLogger(ngl, log_file_path, log_file_roll_size_mb); }, log_file_path,
        log_file_roll_size_mb);
}

void
initialize(GuaranteedLogger gl, std::filesystem::path const &log_file_path, uint32_t log_file_roll_size_mb) {
    publish_logger(0, [&] { return new NanoLogger(gl, log_file_path, log_file_roll_size_mb); }, log_file_path, log_file_roll_size_mb);
}

bool
log_file_opened() {
    // qb fork (Huly QB-1207): this thread's last initialize(), whatever another thread did since -- qb fork (Huly
    // QB-1246): made in this process, not copied from a parent by fork()
    if (last_open >= 0 && last_open_generation == current_fork_generation())
        return last_open == 1;
    std::lock_guard<std::mutex> lock(initialize_mutex()); // qb fork (Huly QB-1207): ordered after restart()'s write
    NanoLogger const *const     logger = atomic_nanologger.load(std::memory_order_relaxed); // qb fork (Huly QB-1207): the lock orders it
    return logger && logger->owned_here() && logger->file_opened(); // qb fork (Huly QB-1246): a parent's writes nothing here
}

std::atomic<unsigned int> loglevel = {0};

void
set_log_level(LogLevel level) {
    loglevel.store(static_cast<unsigned int>(level), std::memory_order_release);
}

bool
is_logged(LogLevel level) {
    return static_cast<unsigned int>(level) >= loglevel.load(std::memory_order_relaxed);
}
} // namespace nanolog

#undef NANOLOG_TEST_HOOK
