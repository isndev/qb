
#ifndef FEATURES_TIMESTAMP_H
#define FEATURES_TIMESTAMP_H

#include <exception>
#include <chrono>
#if defined(__APPLE__)
#include <mach/mach.h>
 #include <mach/mach_time.h>
 #include <sys/time.h>
 #include <math.h>
 #include <time.h>
#elif defined(unix) || defined(__unix) || defined(__unix__)
#include <time.h>
#elif defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#endif


namespace cube {

class Timespan {
public:
    Timespan() noexcept : _duration(0) {}

    explicit Timespan(int64_t duration) noexcept : _duration(duration) {}
    Timespan(const Timespan &) noexcept = default;
    Timespan(Timespan &&) noexcept = default;
    
    ~Timespan() noexcept = default;

    Timespan &operator=(int64_t duration) noexcept {
        _duration = duration;
        return *this;
    }
    Timespan &operator=(const Timespan &) noexcept = default;
    Timespan &operator=(Timespan &&) noexcept = default;

    // Timespan offset operations
    Timespan operator+() const { return Timespan(+_duration); }
    Timespan operator-() const { return Timespan(-_duration); }
    Timespan &operator+=(int64_t offset) noexcept {
        _duration += offset;
        return *this;
    }
    Timespan &operator+=(const Timespan &offset) noexcept {
        _duration += offset.total();
        return *this;
    }
    Timespan &operator-=(int64_t offset) noexcept {
        _duration -= offset;
        return *this;
    }
    Timespan &operator-=(const Timespan &offset) noexcept {
        _duration -= offset.total();
        return *this;
    }

    friend Timespan operator+(const Timespan &timespan, int64_t offset) noexcept {
        return Timespan(timespan.total() + offset);
    }
    friend Timespan operator+(int64_t offset, const Timespan &timespan) noexcept {
        return Timespan(offset + timespan.total());
    }
    friend Timespan operator+(const Timespan &timespan1, const Timespan &timespan2) noexcept {
        return Timespan(timespan1.total() + timespan2.total());
    }
    friend Timespan operator-(const Timespan &timespan, int64_t offset) noexcept {
        return Timespan(timespan.total() - offset);
    }
    friend Timespan operator-(int64_t offset, const Timespan &timespan) noexcept {
        return Timespan(offset - timespan.total());
    }
    friend Timespan operator-(const Timespan &timespan1, const Timespan &timespan2) noexcept {
        return Timespan(timespan1.total() - timespan2.total());
    }

    // Timespan comparison
    friend bool operator==(const Timespan &timespan, int64_t offset) noexcept { return timespan.total() == offset; }
    friend bool operator==(int64_t offset, const Timespan &timespan) noexcept { return offset == timespan.total(); }
    friend bool operator==(const Timespan &timespan1, const Timespan &timespan2) noexcept {
        return timespan1.total() == timespan2.total();
    }
    friend bool operator!=(const Timespan &timespan, int64_t offset) noexcept { return timespan.total() != offset; }
    friend bool operator!=(int64_t offset, const Timespan &timespan) noexcept { return offset != timespan.total(); }
    friend bool operator!=(const Timespan &timespan1, const Timespan &timespan2) noexcept {
        return timespan1.total() != timespan2.total();
    }
    friend bool operator>(const Timespan &timespan, int64_t offset) noexcept { return timespan.total() > offset; }
    friend bool operator>(int64_t offset, const Timespan &timespan) noexcept { return offset > timespan.total(); }
    friend bool operator>(const Timespan &timespan1, const Timespan &timespan2) noexcept {
        return timespan1.total() > timespan2.total();
    }
    friend bool operator<(const Timespan &timespan, int64_t offset) noexcept { return timespan.total() < offset; }
    friend bool operator<(int64_t offset, const Timespan &timespan) noexcept { return offset < timespan.total(); }
    friend bool operator<(const Timespan &timespan1, const Timespan &timespan2) noexcept {
        return timespan1.total() < timespan2.total();
    }
    friend bool operator>=(const Timespan &timespan, int64_t offset) noexcept { return timespan.total() >= offset; }
    friend bool operator>=(int64_t offset, const Timespan &timespan) noexcept { return offset >= timespan.total(); }
    friend bool operator>=(const Timespan &timespan1, const Timespan &timespan2) noexcept {
        return timespan1.total() >= timespan2.total();
    }
    friend bool operator<=(const Timespan &timespan, int64_t offset) noexcept { return timespan.total() <= offset; }
    friend bool operator<=(int64_t offset, const Timespan &timespan) noexcept { return offset <= timespan.total(); }
    friend bool operator<=(const Timespan &timespan1, const Timespan &timespan2) noexcept {
        return timespan1.total() <= timespan2.total();
    }

    std::chrono::duration<int64_t, std::nano> chrono() const noexcept { return std::chrono::nanoseconds(_duration); }

    template<class Rep, class Period>
    static Timespan chrono(const std::chrono::duration<Rep, Period> &duration) noexcept {
        return Timespan(std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count());
    }

    int64_t days() const noexcept { return _duration / (24 * 60 * 60 * 1000000000ll); }
    int64_t hours() const noexcept { return _duration / (60 * 60 * 1000000000ll); }
    int64_t minutes() const noexcept { return _duration / (60 * 1000000000ll); }
    int64_t seconds() const noexcept { return _duration / 1000000000; }
    int64_t milliseconds() const noexcept { return _duration / 1000000; }
    int64_t microseconds() const noexcept { return _duration / 1000; }
    int64_t nanoseconds() const noexcept { return _duration; }
    int64_t total() const noexcept { return _duration; }

    static Timespan days(int64_t days) noexcept { return Timespan(days * 24 * 60 * 60 * 1000000000ll); }
    static Timespan hours(int64_t hours) noexcept { return Timespan(hours * 60 * 60 * 1000000000ll); }
    static Timespan minutes(int64_t minutes) noexcept { return Timespan(minutes * 60 * 1000000000ll); }
    static Timespan seconds(int64_t seconds) noexcept { return Timespan(seconds * 1000000000); }
    static Timespan milliseconds(int64_t milliseconds) noexcept { return Timespan(milliseconds * 1000000); }
    static Timespan microseconds(int64_t microseconds) noexcept { return Timespan(microseconds * 1000); }
    static Timespan nanoseconds(int64_t nanoseconds) noexcept { return Timespan(nanoseconds); }
    static Timespan zero() noexcept { return Timespan(0); }
 
    void swap(Timespan &timespan) noexcept;
    friend void swap(Timespan &timespan1, Timespan &timespan2) noexcept;

private:
    int64_t _duration;
};

class Timestamp
{
public:
    Timestamp() noexcept : _timestamp(epoch()) {}

    explicit Timestamp(uint64_t timestamp) noexcept : _timestamp(timestamp) {}
    Timestamp(const Timestamp&) noexcept = default;
    Timestamp(Timestamp&&) noexcept = default;
    ~Timestamp() noexcept = default;

    Timestamp& operator=(uint64_t timestamp) noexcept
    { _timestamp = timestamp; return *this; }
    Timestamp& operator=(const Timestamp&) noexcept = default;
    Timestamp& operator=(Timestamp&&) noexcept = default;

    // Timestamp offset operations
    Timestamp& operator+=(int64_t offset) noexcept
    { _timestamp += offset; return *this; }
    Timestamp& operator+=(const Timespan& offset) noexcept
    { _timestamp += offset.total(); return *this; }

    Timestamp& operator-=(int64_t offset) noexcept
    { _timestamp -= offset; return *this; }
    Timestamp& operator-=(const Timespan& offset) noexcept
    { _timestamp -= offset.total(); return *this; }

    friend Timestamp operator+(const Timestamp& timestamp, int64_t offset) noexcept
    { return Timestamp(timestamp.total() + offset); }
    friend Timestamp operator+(int64_t offset, const Timestamp& timestamp) noexcept
    { return Timestamp(offset + timestamp.total()); }
    friend Timestamp operator+(const Timestamp& timestamp, const Timespan& offset) noexcept
    { return Timestamp(timestamp.total() + offset.total()); }
    friend Timestamp operator+(const Timespan& offset, const Timestamp& timestamp) noexcept
    { return Timestamp(offset.total() + timestamp.total()); }

    friend Timestamp operator-(const Timestamp& timestamp, int64_t offset) noexcept
    { return Timestamp(timestamp.total() - offset); }
    friend Timestamp operator-(int64_t offset, const Timestamp& timestamp) noexcept
    { return Timestamp(offset - timestamp.total()); }
    friend Timestamp operator-(const Timestamp& timestamp, const Timespan& offset) noexcept
    { return Timestamp(timestamp.total() - offset.total()); }
    friend Timestamp operator-(const Timespan& offset, const Timestamp& timestamp) noexcept
    { return Timestamp(offset.total() - timestamp.total()); }

    friend Timespan operator-(const Timestamp& timestamp1, const Timestamp& timestamp2) noexcept
    { return Timespan(timestamp1.total() - timestamp2.total()); }

    // Timestamp comparison
    friend bool operator==(const Timestamp& timestamp1, uint64_t timestamp2) noexcept
    { return timestamp1.total() == timestamp2; }
    friend bool operator==(uint64_t timestamp1, const Timestamp& timestamp2) noexcept
    { return timestamp1 == timestamp2.total(); }
    friend bool operator==(const Timestamp& timestamp1, const Timestamp& timestamp2) noexcept
    { return timestamp1.total() == timestamp2.total(); }

    friend bool operator!=(const Timestamp& timestamp1, uint64_t timestamp2) noexcept
    { return timestamp1.total() != timestamp2; }
    friend bool operator!=(uint64_t timestamp1, const Timestamp& timestamp2) noexcept
    { return timestamp1 != timestamp2.total(); }
    friend bool operator!=(const Timestamp& timestamp1, const Timestamp& timestamp2) noexcept
    { return timestamp1.total() != timestamp2.total(); }

    friend bool operator>(const Timestamp& timestamp1, uint64_t timestamp2) noexcept
    { return timestamp1.total() > timestamp2; }
    friend bool operator>(uint64_t timestamp1, const Timestamp& timestamp2) noexcept
    { return timestamp1 > timestamp2.total(); }
    friend bool operator>(const Timestamp& timestamp1, const Timestamp& timestamp2) noexcept
    { return timestamp1.total() > timestamp2.total(); }

    friend bool operator<(const Timestamp& timestamp1, uint64_t timestamp2) noexcept
    { return timestamp1.total() < timestamp2; }
    friend bool operator<(uint64_t timestamp1, const Timestamp& timestamp2) noexcept
    { return timestamp1 < timestamp2.total(); }
    friend bool operator<(const Timestamp& timestamp1, const Timestamp& timestamp2) noexcept
    { return timestamp1.total() < timestamp2.total(); }

    friend bool operator>=(const Timestamp& timestamp1, uint64_t timestamp2) noexcept
    { return timestamp1.total() >= timestamp2; }
    friend bool operator>=(uint64_t timestamp1, const Timestamp& timestamp2) noexcept
    { return timestamp1 >= timestamp2.total(); }
    friend bool operator>=(const Timestamp& timestamp1, const Timestamp& timestamp2) noexcept
    { return timestamp1.total() >= timestamp2.total(); }

    friend bool operator<=(const Timestamp& timestamp1, uint64_t timestamp2) noexcept
    { return timestamp1.total() <= timestamp2; }
    friend bool operator<=(uint64_t timestamp1, const Timestamp& timestamp2) noexcept
    { return timestamp1 <= timestamp2.total(); }
    friend bool operator<=(const Timestamp& timestamp1, const Timestamp& timestamp2) noexcept
    { return timestamp1.total() <= timestamp2.total(); }

    std::chrono::time_point<std::chrono::system_clock, std::chrono::duration<uint64_t, std::nano>> chrono() const noexcept
    { return std::chrono::time_point<std::chrono::system_clock>() + std::chrono::nanoseconds(_timestamp); }
    template <class Clock, class Duration>
    static Timestamp chrono(const std::chrono::time_point<Clock, Duration>& time_point) noexcept
    { return Timestamp(std::chrono::duration_cast<std::chrono::nanoseconds>(time_point.time_since_epoch()).count()); }

    uint64_t days() const noexcept
    { return _timestamp / (24 * 60 * 60 * 1000000000ull); }
    uint64_t hours() const noexcept
    { return _timestamp / (60 * 60 * 1000000000ull); }
    uint64_t minutes() const noexcept
    { return _timestamp / (60 * 1000000000ull); }
    uint64_t seconds() const noexcept
    { return _timestamp / 1000000000; }
    uint64_t milliseconds() const noexcept
    { return _timestamp / 1000000; }
    uint64_t microseconds() const noexcept
    { return _timestamp / 1000; }
    uint64_t nanoseconds() const noexcept
    { return _timestamp; }

    uint64_t total() const noexcept { return _timestamp; }

    static Timestamp days(int64_t days) noexcept
    { return Timestamp(days * 24 * 60 * 60 * 1000000000ull); }
    static Timestamp hours(int64_t hours) noexcept
    { return Timestamp(hours * 60 * 60 * 1000000000ull); }
    static Timestamp minutes(int64_t minutes) noexcept
    { return Timestamp(minutes * 60 * 1000000000ull); }
    static Timestamp seconds(int64_t seconds) noexcept
    { return Timestamp(seconds * 1000000000); }
    static Timestamp milliseconds(int64_t milliseconds) noexcept
    { return Timestamp(milliseconds * 1000000); }
    static Timestamp microseconds(int64_t microseconds) noexcept
    { return Timestamp(microseconds * 1000); }
    static Timestamp nanoseconds(int64_t nanoseconds) noexcept
    { return Timestamp(nanoseconds); }


    static uint64_t epoch() noexcept { return 0; }
    static uint64_t utc()  {
#if defined(unix) || defined(__unix) || defined(__unix__) || defined(__APPLE__)
        struct timespec timestamp;
    if (clock_gettime(CLOCK_REALTIME, &timestamp) != 0)
        throw std::runtime_error("Cannot get value of CLOCK_REALTIME timer!");
    return (timestamp.tv_sec * 1000000000) + timestamp.tv_nsec;
#elif defined(_WIN32) || defined(_WIN64)
        FILETIME ft;
        GetSystemTimePreciseAsFileTime(&ft);

        ULARGE_INTEGER result;
        result.LowPart = ft.dwLowDateTime;
        result.HighPart = ft.dwHighDateTime;
        return (result.QuadPart - 116444736000000000ull) * 100;
#endif
    }
    static uint64_t local() {
#if defined(unix) || defined(__unix) || defined(__unix__) || defined(__APPLE__)
        uint64_t timestamp = utc();

    // Adjust UTC time with local timezone offset
    struct tm local;
    time_t seconds = timestamp / (1000000000);
    if (localtime_r(&seconds, &local) != &local)
        throw std::runtime_error("Cannot convert CLOCK_REALTIME time to local date & time structure!");
    return timestamp + (local.tm_gmtoff * 1000000000);
#elif defined(_WIN32) || defined(_WIN64)
        FILETIME ft;
        GetSystemTimePreciseAsFileTime(&ft);

        FILETIME ft_local;
        if (!FileTimeToLocalFileTime(&ft, &ft_local))
            throw std::runtime_error("Cannot convert UTC file time to local file time structure!");

        ULARGE_INTEGER result;
        result.LowPart = ft_local.dwLowDateTime;
        result.HighPart = ft_local.dwHighDateTime;
        return (result.QuadPart - 116444736000000000ull) * 100;
#endif
    }
    static uint64_t nano() {
#if defined(__APPLE__)
        static mach_timebase_info_data_t info;
    static uint64_t bias = Internals::PrepareTimebaseInfo(info);
    return ((mach_absolute_time() - bias) * info.numer) / info.denom;
#elif defined(unix) || defined(__unix) || defined(__unix__)
        struct timespec timestamp = {0};
    if (clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0)
        throw std::runtime_error("Cannot get value of CLOCK_MONOTONIC timer!");
    return (timestamp.tv_sec * 1000000000) + timestamp.tv_nsec;
#elif defined(_WIN32) || defined(_WIN64)
        static uint64_t offset = 0;
        static LARGE_INTEGER first = { 0 };
        static LARGE_INTEGER frequency = { 0 };
        static bool initialized = false;
        static bool qpc = true;

        if (!initialized)
        {
            // Calculate timestamp offset
            FILETIME timestamp;
            GetSystemTimePreciseAsFileTime(&timestamp);

            ULARGE_INTEGER result;
            result.LowPart = timestamp.dwLowDateTime;
            result.HighPart = timestamp.dwHighDateTime;

            // Convert 01.01.1601 to 01.01.1970
            result.QuadPart -= 116444736000000000ll;
            offset = result.QuadPart * 100;

            // Setup performance counter
            qpc = QueryPerformanceFrequency(&frequency) && QueryPerformanceCounter(&first);

            initialized = true;
        }

        if (qpc)
        {
            LARGE_INTEGER timestamp = { 0 };
            QueryPerformanceCounter(&timestamp);
            timestamp.QuadPart -= first.QuadPart;
            return offset + timestamp.QuadPart /  frequency.QuadPart * 1000000000;
        }
        else
            return offset;
#endif
    }
    static uint64_t rdts() {
#if defined(_MSC_VER)
        return __rdtsc();
#elif defined(__i386__)
        uint64_t x;
    __asm__ volatile (".byte 0x0f, 0x31" : "=A" (x));
    return x;
#elif defined(__x86_64__)
        unsigned hi, lo;
        __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
        return ((uint64_t) lo) | (((uint64_t) hi) << 32);
#endif
    }

    void swap(Timestamp& timestamp) noexcept;
    friend void swap(Timestamp& timestamp1, Timestamp& timestamp2) noexcept;

protected:
    uint64_t _timestamp;
};

class UtcTimestamp : public Timestamp
{
public:
    using Timestamp::Timestamp;

    UtcTimestamp() : Timestamp(Timestamp::utc()) {}
    UtcTimestamp(const Timestamp& timestamp) : Timestamp(timestamp) {}
};

class LocalTimestamp : public Timestamp
{
public:
    using Timestamp::Timestamp;

    LocalTimestamp() : Timestamp(Timestamp::local()) {}
    LocalTimestamp(const Timestamp& timestamp) : Timestamp(timestamp) {}
};

class NanoTimestamp : public Timestamp
{
public:
    using Timestamp::Timestamp;

    NanoTimestamp() : Timestamp(Timestamp::nano()) {}
    NanoTimestamp(const Timestamp& timestamp) : Timestamp(timestamp) {}
};

class RdtsTimestamp : public Timestamp
{
public:
    using Timestamp::Timestamp;

    RdtsTimestamp() : Timestamp(Timestamp::rdts()) {}
    RdtsTimestamp(const Timestamp& timestamp) : Timestamp(timestamp) {}
};

}
// namespace cube

#endif //FEATURES_TIMESTAMP_H
