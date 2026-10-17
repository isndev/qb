/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/core/core-affinity.cpp
 * @brief Where a VirtualCore's thread runs, read back from the OS: the inherited mask, explicit CPUs, the calling
 *        thread of `start(false)`, and the start warning for spinning cores beyond the usable CPUs.
 *
 * Every placement is read back from the OS on the core's own thread -- `sched_getaffinity` on Linux,
 * `GetThreadGroupAffinity` on Windows -- never from qb. The mask a core's thread inherits is narrowed the way a
 * deployment narrows it: on Linux the calling thread's own mask (what `taskset` and a cgroup cpuset set, and what
 * every thread it starts inherits), on Windows the process's (what `start /affinity` and a job object set).
 *
 *   - Default cores take the CPUs of the inherited mask in order, and a core the mask has no CPU for runs on the
 *     whole mask (Huly QB-1081). Before, core i was pinned to CPU i: outside the mask on Linux, and a refused
 *     `SetThreadAffinityMask` on Windows, which left every core on the whole mask.
 *   - An explicit CPU outside the inherited mask is left out; a set with none inside pins nothing (QB-1081). Before,
 *     Linux pinned it -- out of the deployment's mask -- and Windows refused the whole set over one such CPU.
 *   - `start(false)` puts the calling thread's affinity back when it returns, after a run and after a failed init
 *     (Huly QB-1083). Before, the thread stayed pinned to its core's CPU.
 *   - `start()` warns, on stderr, when more cores spin at latency 0 than the process has CPUs to run them, and only
 *     then (Huly QB-1082): such cores preempt one another and every hop between them waits for a time slice.
 *
 * macOS has no affinity mask -- its affinity is a scheduling tag, and nothing at all on Apple Silicon
 * (`qb::CPU::ThreadPinningSupported()`): the mask cases skip there, the warning is judged against its CPU count, and
 * the `start(false)` case reads the tag where the kernel has one.
 */

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <sstream>
#include <streambuf>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/main.h>
#include <qb/system/cpu.h>

#if defined(__linux__)
#include <sched.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/thread_act.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#endif

using namespace std::chrono_literals;

namespace core_affinity_test {

using Cpus = std::vector<unsigned>;

std::string
show(Cpus const &cpus) {
    std::ostringstream out;
    out << '{';
    for (std::size_t i = 0; i < cpus.size(); ++i)
        out << (i ? "," : "") << cpus[i];
    out << '}';
    return out.str();
}

#if defined(__linux__) || defined(_WIN32)
constexpr bool kHasMask = true;
#else
constexpr bool kHasMask = false;
#endif

#if defined(__linux__)
Cpus
cpus_of(cpu_set_t const &set) {
    Cpus cpus;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
        if (CPU_ISSET(cpu, &set))
            cpus.push_back(static_cast<unsigned>(cpu));
    return cpus;
}

/// The calling thread's mask, as the kernel holds it.
Cpus
thread_cpus() {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) != 0)
        return {};
    return cpus_of(set);
}

/// What a thread started from here inherits: on Linux, the calling thread's own mask.
Cpus
inherited_cpus() {
    return thread_cpus();
}

/// Narrows what a thread started from here inherits to @p cpus, for its lifetime -- a `taskset` of this thread.
class InheritedMask {
    cpu_set_t _saved{};
    bool      _ok = false;

public:
    explicit InheritedMask(Cpus const &cpus) {
        CPU_ZERO(&_saved);
        if (sched_getaffinity(0, sizeof(_saved), &_saved) != 0)
            return;
        cpu_set_t set;
        CPU_ZERO(&set);
        for (const unsigned cpu : cpus)
            CPU_SET(static_cast<int>(cpu), &set);
        _ok = sched_setaffinity(0, sizeof(set), &set) == 0;
    }
    ~InheritedMask() {
        sched_setaffinity(0, sizeof(_saved), &_saved);
    }
    InheritedMask(InheritedMask const &)            = delete;
    InheritedMask &operator=(InheritedMask const &) = delete;
    [[nodiscard]] bool
    ok() const {
        return _ok;
    }
};

/// The calling thread's whole affinity, compared before and after a `start(false)`.
struct Affinity {
    Cpus cpus;
    bool operator==(Affinity const &) const = default;
};
std::string
show(Affinity const &a) {
    return show(a.cpus);
}
Affinity
caller_affinity() {
    return {thread_cpus()};
}
void
set_caller_affinity(Affinity const &a) {
    cpu_set_t set;
    CPU_ZERO(&set);
    for (const unsigned cpu : a.cpus)
        CPU_SET(static_cast<int>(cpu), &set);
    sched_setaffinity(0, sizeof(set), &set);
}

#elif defined(_WIN32)
Cpus
cpus_of(DWORD_PTR const mask) {
    Cpus cpus;
    for (unsigned cpu = 0; cpu < sizeof(DWORD_PTR) * 8u; ++cpu)
        if (mask & (static_cast<DWORD_PTR>(1) << cpu))
            cpus.push_back(cpu);
    return cpus;
}

/// The calling thread's mask in its processor group, as the kernel holds it.
Cpus
thread_cpus() {
    GROUP_AFFINITY ga{};
    if (!GetThreadGroupAffinity(GetCurrentThread(), &ga))
        return {};
    return cpus_of(static_cast<DWORD_PTR>(ga.Mask));
}

/// What a thread started from here inherits: on Windows, the process's mask.
Cpus
inherited_cpus() {
    DWORD_PTR process = 0, system = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &process, &system))
        return {};
    return cpus_of(process);
}

/// Narrows what a thread started from here inherits to @p cpus, for its lifetime -- a `start /affinity`.
class InheritedMask {
    DWORD_PTR _saved = 0;
    bool      _ok    = false;

public:
    explicit InheritedMask(Cpus const &cpus) {
        DWORD_PTR system = 0;
        if (!GetProcessAffinityMask(GetCurrentProcess(), &_saved, &system))
            return;
        DWORD_PTR mask = 0;
        for (const unsigned cpu : cpus)
            mask |= static_cast<DWORD_PTR>(1) << cpu;
        _ok = SetProcessAffinityMask(GetCurrentProcess(), mask) != 0;
    }
    ~InheritedMask() {
        if (_saved)
            SetProcessAffinityMask(GetCurrentProcess(), _saved);
    }
    InheritedMask(InheritedMask const &)            = delete;
    InheritedMask &operator=(InheritedMask const &) = delete;
    [[nodiscard]] bool
    ok() const {
        return _ok;
    }
};

/// The calling thread's whole affinity: its group and its mask in it.
struct Affinity {
    unsigned  group = 0;
    DWORD_PTR mask  = 0;
    Cpus      cpus;
    bool
    operator==(Affinity const &o) const {
        return group == o.group && mask == o.mask;
    }
};
std::string
show(Affinity const &a) {
    return "group " + std::to_string(a.group) + " " + show(a.cpus);
}
Affinity
caller_affinity() {
    GROUP_AFFINITY ga{};
    if (!GetThreadGroupAffinity(GetCurrentThread(), &ga))
        return {};
    return {ga.Group, static_cast<DWORD_PTR>(ga.Mask), cpus_of(static_cast<DWORD_PTR>(ga.Mask))};
}
void
set_caller_affinity(Affinity const &a) {
    GROUP_AFFINITY ga{};
    ga.Group = static_cast<WORD>(a.group);
    ga.Mask  = static_cast<KAFFINITY>(a.mask);
    SetThreadGroupAffinity(GetCurrentThread(), &ga, nullptr);
}

#else
Cpus
thread_cpus() {
    return {};
}
Cpus
inherited_cpus() {
    return {};
}
class InheritedMask {
public:
    explicit InheritedMask(Cpus const &) {}
    [[nodiscard]] bool
    ok() const {
        return false;
    }
};

#if defined(__APPLE__)
/// macOS: the THREAD_AFFINITY_POLICY tag -- the only affinity a Mac has, a hint and not a pin.
struct Affinity {
    integer_t tag   = THREAD_AFFINITY_TAG_NULL;
    bool      known = false;
    Cpus      cpus;
    bool
    operator==(Affinity const &o) const {
        return known == o.known && tag == o.tag;
    }
};
std::string
show(Affinity const &a) {
    return a.known ? "tag " + std::to_string(a.tag) : std::string("no tag");
}
Affinity
caller_affinity() {
    thread_affinity_policy_data_t policy      = {THREAD_AFFINITY_TAG_NULL};
    mach_msg_type_number_t        count       = THREAD_AFFINITY_POLICY_COUNT;
    boolean_t                     get_default = FALSE;
    if (thread_policy_get(pthread_mach_thread_np(pthread_self()), THREAD_AFFINITY_POLICY, reinterpret_cast<thread_policy_t>(&policy), &count,
                          &get_default)
        != KERN_SUCCESS)
        return {};
    return {policy.affinity_tag, true, {}};
}
void
set_caller_affinity(Affinity const &a) {
    if (!a.known)
        return;
    thread_affinity_policy_data_t policy = {a.tag};
    thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_AFFINITY_POLICY, reinterpret_cast<thread_policy_t>(&policy),
                      THREAD_AFFINITY_POLICY_COUNT);
}
#else
/// Any other system: no affinity this test can read.
struct Affinity {
    Cpus cpus;
    bool operator==(Affinity const &) const = default;
};
std::string
show(Affinity const &) {
    return "unknown";
}
Affinity
caller_affinity() {
    return {};
}
void
set_caller_affinity(Affinity const &) {}
#endif
#endif

/// gtest prints an `Affinity` the way the messages do, not as bytes.
void
PrintTo(Affinity const &a, std::ostream *out) {
    *out << show(a);
}

/// Puts the calling thread's affinity back on every exit of a case -- so that a defective `start(false)`, pinning the
/// gtest thread, cannot narrow the mask the next case inherits.
class CallerAffinityGuard {
    Affinity _saved = caller_affinity();

public:
    ~CallerAffinityGuard() {
        set_caller_affinity(_saved);
    }
};

/// Everything written to `std::cerr` while it lives.
class CerrCapture {
    std::stringstream _buf;
    std::streambuf   *_saved;

public:
    CerrCapture()
        : _saved(std::cerr.rdbuf(_buf.rdbuf())) {}
    ~CerrCapture() {
        std::cerr.rdbuf(_saved);
    }
    CerrCapture(CerrCapture const &)            = delete;
    CerrCapture &operator=(CerrCapture const &) = delete;
    [[nodiscard]] std::string
    str() const {
        return _buf.str();
    }
};

/// Each core's mask as its own thread read it, after the core's start pinned it; read after `join()`.
std::array<Cpus, 4> g_seen;

/// Reads where its core's thread may run, then leaves.
class Report : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        g_seen[getIndex()] = thread_cpus();
        kill();
        co_return true;
    }
};

/// Leaves at once.
class Quit : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        kill();
        co_return true;
    }
};

/// Refuses to start: the core's init fails.
class RefuseInit : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        co_return false;
    }
};

/// The words of the start warning the cases look for (Main.cpp).
std::string
spinning_words(std::size_t const spinning) {
    return std::to_string(spinning) + " VirtualCores spin at latency 0";
}

} // namespace core_affinity_test

using namespace core_affinity_test;

/**
 * @test Default cores take the CPUs of the inherited mask in order; the third, which the mask has no CPU for, runs on
 *       the whole mask. The mask is the last two CPUs this thread may run on, so core i is never CPU i on a host of
 *       three CPUs or more.
 */
TEST(CoreAffinity, DefaultCoresTakeTheInheritedCpusInOrder) {
    if (!kHasMask)
        GTEST_SKIP() << "no affinity mask on this platform: macOS affinity is a scheduling tag";
    const Cpus mask = inherited_cpus();
    if (mask.size() < 2)
        GTEST_SKIP() << "needs an inherited mask of two CPUs at least, has " << show(mask);
    const Cpus    two{mask[mask.size() - 2], mask.back()};
    InheritedMask narrowed(two);
    ASSERT_TRUE(narrowed.ok()) << "could not narrow the inherited mask to " << show(two);
    ASSERT_EQ(inherited_cpus(), two);

    g_seen = {};
    {
        qb::Main engine;
        for (qb::CoreId core = 0; core < 3; ++core)
            engine.core(core).setLatency(1ms).addActor<Report>(); // parking cores: three over two CPUs is no load
        engine.start();
        engine.join();
        EXPECT_FALSE(engine.hasError());
    }
    EXPECT_EQ(g_seen[0], (Cpus{two[0]})) << "core 0 takes the first CPU of the inherited mask " << show(two);
    EXPECT_EQ(g_seen[1], (Cpus{two[1]})) << "core 1 takes the second CPU of the inherited mask " << show(two);
    EXPECT_EQ(g_seen[2], two) << "the mask has no third CPU: core 2 must run on the whole mask " << show(two);
}

/**
 * @test An explicit CPU outside the inherited mask is left out of the core's pin, and a set with none inside pins
 *       nothing; the CPUs inside are pinned as asked, and `qb::NoAffinity` still pins nothing.
 */
TEST(CoreAffinity, ExplicitCpusOutsideTheInheritedMaskAreLeftOut) {
    if (!kHasMask)
        GTEST_SKIP() << "no affinity mask on this platform: macOS affinity is a scheduling tag";
    const Cpus mask = inherited_cpus();
    if (mask.size() < 3)
        GTEST_SKIP() << "needs an inherited mask of three CPUs at least, has " << show(mask);
    const unsigned outside = mask.front();
    const Cpus     two{mask[mask.size() - 2], mask.back()};
    if (two[1] >= qb::MaxCores)
        GTEST_SKIP() << "a CoreIdSet holds CPU numbers below " << qb::MaxCores << ", the mask ends at " << two[1];
    InheritedMask narrowed(two);
    ASSERT_TRUE(narrowed.ok()) << "could not narrow the inherited mask to " << show(two);

    const auto id = [](unsigned cpu) {
        return static_cast<qb::CoreId>(cpu);
    };
    g_seen = {};
    {
        qb::Main engine;
        engine.core(0).setLatency(1ms).setAffinity(qb::CoreIdSet{id(outside)}).addActor<Report>();
        engine.core(1).setLatency(1ms).setAffinity(qb::CoreIdSet{id(outside), id(two[1])}).addActor<Report>();
        engine.core(2).setLatency(1ms).setAffinity(qb::CoreIdSet{id(two[0])}).addActor<Report>();
        engine.core(3).setLatency(1ms).setAffinity(qb::CoreIdSet{qb::NoAffinity}).addActor<Report>();
        engine.start();
        engine.join();
        EXPECT_FALSE(engine.hasError());
    }
    EXPECT_EQ(g_seen[0], two) << "CPU " << outside << " is outside the inherited mask " << show(two)
                              << ": nothing is pinned, the core runs on the mask";
    EXPECT_EQ(g_seen[1], (Cpus{two[1]})) << "of {" << outside << "," << two[1] << "} only " << two[1] << " is inside the mask";
    EXPECT_EQ(g_seen[2], (Cpus{two[0]})) << "a CPU inside the mask is pinned as asked";
    EXPECT_EQ(g_seen[3], two) << "qb::NoAffinity pins nothing";
}

namespace {
/// Runs a one-core engine on the calling thread -- core 1, so that its default CPU is the second of the mask (and
/// a macOS tag of 1, where tag 0 means none) -- with @p Init, and returns whether it reported an error.
template <typename Init>
bool
run_on_the_calling_thread() {
    qb::Main engine;
    engine.core(1).addActor<Init>();
    engine.start(false);
    return engine.hasError();
}
} // namespace

/**
 * @test `start(false)` puts the calling thread's affinity back when it returns.
 */
TEST(CoreAffinity, StartOnTheCallingThreadPutsItsAffinityBack) {
    if (!qb::CPU::ThreadPinningSupported())
        GTEST_SKIP() << "this host pins nothing (Apple Silicon): there is nothing to put back";
    const Affinity before = caller_affinity();
    if (kHasMask && before.cpus.size() < 2)
        GTEST_SKIP() << "the calling thread may run on one CPU only: a pin to it is its mask";
    CallerAffinityGuard restore_after;
    EXPECT_FALSE(run_on_the_calling_thread<Quit>());
    EXPECT_EQ(caller_affinity(), before) << "start(false) left the calling thread at " << show(caller_affinity()) << ", it was at "
                                         << show(before);
}

/**
 * @test ... and after a core whose init failed, the other way `start(false)` returns.
 */
TEST(CoreAffinity, StartOnTheCallingThreadPutsItsAffinityBackAfterAFailedInit) {
    if (!qb::CPU::ThreadPinningSupported())
        GTEST_SKIP() << "this host pins nothing (Apple Silicon): there is nothing to put back";
    const Affinity before = caller_affinity();
    if (kHasMask && before.cpus.size() < 2)
        GTEST_SKIP() << "the calling thread may run on one CPU only: a pin to it is its mask";
    CallerAffinityGuard restore_after;
    EXPECT_TRUE(run_on_the_calling_thread<RefuseInit>()) << "the core's init was meant to fail";
    EXPECT_EQ(caller_affinity(), before) << "a failed start(false) left the calling thread at " << show(caller_affinity()) << ", it was at "
                                         << show(before);
}

namespace {
/// The CPUs the warning cases leave this process: one, where a mask can be narrowed; the host's, where none exists.
unsigned
usable_for_the_warning_cases() {
    if (kHasMask)
        return 1u;
    return std::max(1u, std::thread::hardware_concurrency());
}

/// Starts @p spinning cores at latency 0 and @p parking ones at 1 ms, inherited mask narrowed to one CPU where there
/// is one, and returns what the engine wrote on stderr.
std::string
start_and_capture(std::size_t const spinning, std::size_t const parking) {
    const Cpus    mask = inherited_cpus();
    InheritedMask narrowed(kHasMask ? Cpus{mask.empty() ? 0u : mask.back()} : Cpus{});
    if (kHasMask && !narrowed.ok()) {
        ADD_FAILURE() << "could not narrow the inherited mask to one CPU of " << show(mask);
        return {};
    }
    CerrCapture capture;
    {
        qb::Main   engine;
        qb::CoreId core = 0;
        for (std::size_t i = 0; i < spinning; ++i)
            engine.core(core++).addActor<Quit>();
        for (std::size_t i = 0; i < parking; ++i)
            engine.core(core++).setLatency(1ms).addActor<Quit>();
        engine.start();
        engine.join();
        EXPECT_FALSE(engine.hasError());
    }
    return capture.str();
}
} // namespace

/**
 * @test More cores at latency 0 than the CPUs the process can run on: `start()` says so on stderr, with both counts.
 */
TEST(CoreAffinity, SpinningCoresBeyondTheUsableCpusWarnAtStart) {
    const unsigned    usable = usable_for_the_warning_cases();
    const std::string said   = start_and_capture(usable + 1u, 0);
    EXPECT_NE(said.find(spinning_words(usable + 1u)), std::string::npos)
        << "no warning for " << usable + 1u << " spinning cores on " << usable << " usable CPU(s); stderr was:\n"
        << said;
    EXPECT_NE(said.find("only " + std::to_string(usable) + " CPU"), std::string::npos) << "the warning must name the usable CPUs; stderr was:\n"
                                                                                       << said;
}

/**
 * @test As many cores at latency 0 as usable CPUs, and one more that parks: no warning -- a core that parks yields
 *       its CPU.
 */
TEST(CoreAffinity, SpinningCoresWithinTheUsableCpusStartSilently) {
    const unsigned    usable = usable_for_the_warning_cases();
    const std::string said   = start_and_capture(usable, 1);
    EXPECT_EQ(said.find("VirtualCores spin at latency 0"), std::string::npos)
        << "a warning for " << usable << " spinning core(s) on " << usable << " usable CPU(s); stderr was:\n"
        << said;
}
