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
#include <iosfwd>
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
    ~NanoLogLine();

    NanoLogLine(NanoLogLine &&) = default;
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

    template <size_t N>
    NanoLogLine &
    operator<<(const char (&arg)[N]) {
        encode(string_literal_t(arg));
        return *this;
    }

    struct string_literal_t {
        explicit string_literal_t(char const *s)
            : m_s(s) {}
        char const *m_s;
    };

private:
    char *buffer();

    template <typename Arg>
    void encode(Arg arg);

    template <typename Arg>
    void encode(Arg arg, uint8_t type_id);

    void encode(char *arg);
    void encode(char const *arg);
    void encode(string_literal_t arg);
    void encode_c_string(char const *arg, size_t length);
    void resize_buffer_if_needed(size_t additional_bytes);
    void stringify(std::ostream &os, char *start, char const *const end);

private:
    size_t m_bytes_used;
    size_t m_buffer_size;
    std::unique_ptr<char[]> m_heap_buffer;
    char m_stack_buffer[256 - 2 * sizeof(size_t) - sizeof(decltype(m_heap_buffer)) -
                        8 /* Reserved */];
};

template <>
NanoLogLine &NanoLogLine::operator<<<char>(char const &arg);
template <>
NanoLogLine &NanoLogLine::operator<<<char const *>(char const *const &arg);
template <>
NanoLogLine &NanoLogLine::operator<<<int32_t>(int32_t const &arg);
template <>
NanoLogLine &NanoLogLine::operator<<<uint32_t>(uint32_t const &arg);
template <>
NanoLogLine &NanoLogLine::operator<<<int64_t>(int64_t const &arg);
template <>
NanoLogLine &NanoLogLine::operator<<<uint64_t>(uint64_t const &arg);
template <>
NanoLogLine &NanoLogLine::operator<<<double>(double const &arg);
template <>
NanoLogLine &NanoLogLine::operator<<<std::string>(std::string const &arg);
template <>
NanoLogLine &NanoLogLine::operator<<<std::string_view>(std::string_view const &arg);

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
void initialize(GuaranteedLogger gl, std::string const &log_file_path,
                uint32_t log_file_roll_size_mb);
void initialize(NonGuaranteedLogger ngl, std::string const &log_file_path,
                uint32_t log_file_roll_size_mb);

} // namespace nanolog

#define NANO_LOG(LEVEL) \
    nanolog::NanoLog() == nanolog::NanoLogLine(LEVEL, __FILE__, __func__, __LINE__)
#ifdef QB_LOGGER
#    define LOG_DEBUG(X)                                       \
        (void)(nanolog::is_logged(nanolog::LogLevel::DEBUG) && \
               NANO_LOG(nanolog::LogLevel::DEBUG) << X)
#    define LOG_VERB(X)                                          \
        (void)(nanolog::is_logged(nanolog::LogLevel::VERBOSE) && \
               NANO_LOG(nanolog::LogLevel::VERBOSE) << X)
#    define LOG_INFO(X)                                       \
        (void)(nanolog::is_logged(nanolog::LogLevel::INFO) && \
               NANO_LOG(nanolog::LogLevel::INFO) << X)
#    define LOG_WARN(X)                                       \
        (void)(nanolog::is_logged(nanolog::LogLevel::WARN) && \
               NANO_LOG(nanolog::LogLevel::WARN) << X)
#    define LOG_CRIT(X)                                       \
        (void)(nanolog::is_logged(nanolog::LogLevel::CRIT) && \
               NANO_LOG(nanolog::LogLevel::CRIT) << X)
#endif
#endif /* NANO_LOG_HEADER_GUARD */
