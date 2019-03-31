/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2019 isndev (www.qbaf.io). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 *         limitations under the License.
 */

#ifndef FEATURES_TIMESTAMP_H
#define FEATURES_TIMESTAMP_H

#include <exception>
#include <chrono>
#include <ctime>
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
#include <thread>

namespace qb {

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
        static uint64_t nano() {
			// Store system time and steady time on first call
            static const std::chrono::time_point<std::chrono::system_clock> clk_system_start = std::chrono::system_clock::now();
            static const std::chrono::time_point<std::chrono::steady_clock> clk_steady_start = std::chrono::steady_clock::now();

			// Nano timestamp is (system_start + (steady_now - steady_start))
            return std::chrono::duration_cast<std::chrono::nanoseconds>(
                    clk_system_start.time_since_epoch() +
                    (std::chrono::steady_clock::now().time_since_epoch() - clk_steady_start.time_since_epoch())
            ).count();
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

        UtcTimestamp() : Timestamp(Timestamp::nano()) {}
        UtcTimestamp(const Timestamp& timestamp) : Timestamp(timestamp) {}
    };

    class LocalTimestamp : public Timestamp
    {
    public:
        using Timestamp::Timestamp;

        LocalTimestamp() : Timestamp(Timestamp::nano()) {}
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
// namespace qb

#endif