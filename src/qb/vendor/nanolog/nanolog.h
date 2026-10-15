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

#ifndef NANO_LOG_HEADER_GUARD
#define NANO_LOG_HEADER_GUARD
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iosfwd>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>

namespace nanolog {
enum class LogLevel : uint8_t { DEBUG, VERBOSE, INFO, WARN, CRIT };

class NanoLogLine {
public:
    NanoLogLine(LogLevel level, char const *file, char const *function, uint32_t line);
    ~NanoLogLine() = default;

    NanoLogLine(NanoLogLine &&)            = default;
    NanoLogLine &operator=(NanoLogLine &&) = default;

    void stringify(std::ostream &os);

    template <typename T>
    NanoLogLine &
    operator<<(T const &data) {
        std::ostringstream os;
        os << data;
        *this << os.str();
        return *this;
    }

    // qb fork (Huly QB-366): a char ARRAY is copied into the record, up to its first NUL and never past its end -- the
    // record is written out later, on the writer thread. Every `const char (&)[N]` used to be stored as a POINTER, the
    // form meant for string literals, which the type cannot tell from a char buffer filled by snprintf, a const local
    // array or an array member: the writer read them after they had changed or died. Borrowing static text is
    // explicit: `string_literal_t`, below.
    // The copy is written inline, by append_text (Huly QB-1004): for a literal the compiler knows the length and the
    // bytes, so the field is a few stores -- what the borrowed pointer cost -- and no call. The length is read in the
    // form every compiler folds for a literal: an array whose last byte is NUL (a literal always, and the compiler
    // knows it) is measured by strlen, which stops inside the array -- g++ and MSVC both fold it, where MSVC calls
    // memchr for char_traits::find; only an array whose last byte is not NUL is searched, bounded by its N bytes.
    template <size_t N>
    NanoLogLine &
    operator<<(const char (&arg)[N]) {
        static_assert(N > 0, "an array has at least one element");
        size_t length;
        if (arg[N - 1] == '\0') {
            length = std::char_traits<char>::length(arg);
        } else {
            char const *const nul = std::char_traits<char>::find(arg, N, '\0');
            length                = nul ? static_cast<size_t>(nul - arg) : N;
        }
        append_text(arg, length);
        return *this;
    }

    // Text of STATIC storage duration, borrowed: the record stores the pointer and the writer thread reads the text
    // when it writes the record out, so the caller vouches that it outlives the logger (a string literal does).
    struct string_literal_t {
        explicit string_literal_t(char const *s)
            : m_s(s) {}
        char const *m_s;
    };

    NanoLogLine &
    operator<<(string_literal_t arg) {
        append_scalar(arg, tag_literal);
        return *this;
    }

private:
    // A field on the wire is its type tag, then its value: the tag is the index of the type in nanolog.cpp's
    // SupportedTypes (asserted there), which stringify() switches on when the writer thread decodes the record. A text
    // field (Huly QB-367) is tag_text, its length on 32 bits, then exactly that many bytes.
    enum Tag : uint8_t { tag_char = 0, tag_uint32 = 1, tag_uint64 = 2, tag_int32 = 3, tag_int64 = 4, tag_double = 5, tag_literal = 6, tag_text = 7 };
    static constexpr size_t text_field_overhead = 1 + sizeof(uint32_t);
    static constexpr size_t max_text_field      = std::numeric_limits<uint32_t>::max();

    // Out of line, in nanolog.cpp: the two writers that grow the buffer first, and the decoder.
    void encode_scalar(void const *value, size_t size, Tag tag);
    void encode_c_string(char const *arg, size_t length);
    void resize_buffer_if_needed(size_t additional_bytes);
    void stringify(std::ostream &os, char *start, char const *const end);

    char *
    cursor() noexcept {
        return (m_heap_buffer ? m_heap_buffer.get() : m_stack_buffer) + m_bytes_used;
    }

    // The fast paths (Huly QB-1004): a field the buffer holds is written here, inline in the caller -- a capacity
    // check, the tag, the value -- so a scalar is a few stores and a literal's copy a few more (the compiler knows its
    // length and its bytes). A field the buffer cannot hold goes out of line, where the buffer grows; so does a text
    // past 4 GiB, written as consecutive fields. An empty text writes no field, as there.
    template <typename Arg>
    void
    append_scalar(Arg const &arg, Tag tag) {
        static_assert(std::is_trivially_copyable_v<Arg>, "a scalar field is its bytes");
        if (m_bytes_used + 1 + sizeof(Arg) <= m_buffer_size) [[likely]] {
            char *const b = cursor();
            b[0]          = static_cast<char>(tag);
            std::memcpy(b + 1, &arg, sizeof(Arg));
            m_bytes_used += 1 + sizeof(Arg);
        } else {
            encode_scalar(&arg, sizeof(Arg), tag);
        }
    }

    void
    append_text(char const *text, size_t length) {
        if (length == 0)
            return;
        if (length <= max_text_field && m_bytes_used + text_field_overhead + length <= m_buffer_size) [[likely]] {
            char *const    b     = cursor();
            uint32_t const field = static_cast<uint32_t>(length);
            b[0]                 = static_cast<char>(tag_text);
            std::memcpy(b + 1, &field, sizeof(field));
            std::memcpy(b + text_field_overhead, text, length);
            m_bytes_used += text_field_overhead + length;
        } else {
            encode_c_string(text, length);
        }
    }

private:
    size_t                  m_bytes_used;
    size_t                  m_buffer_size;
    std::unique_ptr<char[]> m_heap_buffer;
    char                    m_stack_buffer[256 - 2 * sizeof(size_t) - sizeof(decltype(m_heap_buffer)) - 8 /* Reserved */];
};

// The fields the record encodes directly -- the rest go through the ostringstream of the primary template. Inline, so
// that a field is written in the caller (Huly QB-1004); they were defined in nanolog.cpp, a call per field.
template <>
inline NanoLogLine &
NanoLogLine::operator<< <char>(char const &arg) {
    append_scalar(arg, tag_char);
    return *this;
}
template <>
inline NanoLogLine &
NanoLogLine::operator<< <int32_t>(int32_t const &arg) {
    append_scalar(arg, tag_int32);
    return *this;
}
template <>
inline NanoLogLine &
NanoLogLine::operator<< <uint32_t>(uint32_t const &arg) {
    append_scalar(arg, tag_uint32);
    return *this;
}
template <>
inline NanoLogLine &
NanoLogLine::operator<< <int64_t>(int64_t const &arg) {
    append_scalar(arg, tag_int64);
    return *this;
}
template <>
inline NanoLogLine &
NanoLogLine::operator<< <uint64_t>(uint64_t const &arg) {
    append_scalar(arg, tag_uint64);
    return *this;
}
template <>
inline NanoLogLine &
NanoLogLine::operator<< <double>(double const &arg) {
    append_scalar(arg, tag_double);
    return *this;
}
// A C string is copied up to its NUL; a null pointer writes nothing.
template <>
inline NanoLogLine &
NanoLogLine::operator<< <char const *>(char const *const &arg) {
    if (arg != nullptr)
        append_text(arg, std::strlen(arg));
    return *this;
}
template <>
inline NanoLogLine &
NanoLogLine::operator<< <char *>(char *const &arg) {
    if (arg != nullptr)
        append_text(arg, std::strlen(arg));
    return *this;
}
template <>
inline NanoLogLine &
NanoLogLine::operator<< <std::string>(std::string const &arg) {
    append_text(arg.data(), arg.size());
    return *this;
}
template <>
inline NanoLogLine &
NanoLogLine::operator<< <std::string_view>(std::string_view const &arg) {
    append_text(arg.data(), arg.size());
    return *this;
}

struct NanoLog {
    /*
     * Ideally this should have been operator+=
     * Could not get that to compile, so here we are...
     */
    bool operator==(NanoLogLine &);
};

void set_log_level(LogLevel level);

bool is_logged(LogLevel level);

/*
 * Non guaranteed logging. Uses a ring buffer to hold log lines.
 * When the ring gets full, the previous log line in the slot will be dropped.
 * Does not block producer even if the ring buffer is full.
 * ring_buffer_size_mb - LogLines are pushed into a mpsc ring buffer whose size
 * is determined by this parameter. Since each LogLine is 256 bytes,
 * ring_buffer_size = ring_buffer_size_mb * 1024 * 1024 / 256
 * qb fork (Huly QB-368): at least 1 MB, at most 1048575 MB -- the ring is indexed by 32-bit counters, and 1048576 MB
 * is 2^32 items. initialize() refuses a larger ring with std::length_error before it allocates the ring's slots.
 */
struct NonGuaranteedLogger {
    NonGuaranteedLogger(uint32_t ring_buffer_size_mb_)
        : ring_buffer_size_mb(ring_buffer_size_mb_) {}
    uint32_t ring_buffer_size_mb;
};

/*
 * Provides a guarantee log lines will not be dropped.
 */
struct GuaranteedLogger {};

/*
 * Ensure initialize() is called prior to any log statements.
 * log_directory - where to create the logs. For example - "/tmp/"
 * log_file_name - root of the file name. For example - "nanolog"
 * This will create log files of the form -
 * /tmp/nanolog.1.txt
 * /tmp/nanolog.2.txt
 * etc.
 * log_file_roll_size_mb - mega bytes after which we roll to next log file.
 */
void initialize(GuaranteedLogger gl, std::filesystem::path const &log_file_path, uint32_t log_file_roll_size_mb);
void initialize(NonGuaranteedLogger ngl, std::filesystem::path const &log_file_path, uint32_t log_file_roll_size_mb);

/*
 * qb fork (Huly QB-327): whether the log file of the last initialize() -- its first roll, <log_file_path>.1.log, which
 * initialize() opens before it returns -- was OPENED. False before any initialize(). A file that cannot be opened (a
 * read-only directory, a directory of that name) leaves every line logged through it discarded.
 */
bool log_file_opened();

} // namespace nanolog

#define NANO_LOG(LEVEL) nanolog::NanoLog() == nanolog::NanoLogLine(LEVEL, __FILE__, __func__, __LINE__)
// The macros qb's own 127 call sites use. They are QB_-prefixed since 3.0.0: `LOG_INFO`,
// `LOG_DEBUG` and `LOG_CRIT` are also POSIX <syslog.h> names, and this header reaches every
// consumer of <qb/io.h>, <qb/main.h>, <qb/actor.h> and every qbm umbrella -- so the unprefixed
// spellings used to REPLACE a consumer's own, with zero warnings under the -isystem line qb's
// CMake package exports (measured: the consumer's LOG_INFO stopped being called, silently).
// The unprefixed names still work; qb/io.h defines them as aliases, guarded so that a consumer
// who has their own keeps theirs.
#ifdef QB_WITH_LOGGING
#define QB_LOG_DEBUG(X) (void) (nanolog::is_logged(nanolog::LogLevel::DEBUG) && NANO_LOG(nanolog::LogLevel::DEBUG) << X)
#define QB_LOG_VERB(X) (void) (nanolog::is_logged(nanolog::LogLevel::VERBOSE) && NANO_LOG(nanolog::LogLevel::VERBOSE) << X)
#define QB_LOG_INFO(X) (void) (nanolog::is_logged(nanolog::LogLevel::INFO) && NANO_LOG(nanolog::LogLevel::INFO) << X)
#define QB_LOG_WARN(X) (void) (nanolog::is_logged(nanolog::LogLevel::WARN) && NANO_LOG(nanolog::LogLevel::WARN) << X)
#define QB_LOG_CRIT(X) (void) (nanolog::is_logged(nanolog::LogLevel::CRIT) && NANO_LOG(nanolog::LogLevel::CRIT) << X)
#endif
#endif /* NANO_LOG_HEADER_GUARD */
