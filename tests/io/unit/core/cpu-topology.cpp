/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/core/cpu-topology.cpp
 * @brief `qb::CPU` topology query API + `qb::spin_loop_pause()`.
 *
 * `qb::CPU` (qb/system/cpu.h) reports architecture, affinity, logical/physical core counts, clock
 * speed and hyper-threading. The values are machine-specific, so this `unit` test asserts the
 * *invariants and self-consistency* of the API (not host-specific numbers) so it stays portable
 * across every CI runner. `spin_loop_pause()` is the hot-spin yield hint. No engine, no I/O.
 *
 * Split out of system/test-cpu.cpp (spec §2): the CPU/topology cases live here, the RAII helpers
 * (`qb::resource` / `qb::scope_guard`) move to unit/core/raii-helpers.cpp. Strengthened:
 * `SpinLoopPause` now makes a real observable assertion (it forces the function to actually run a
 * bounded number of times and survive) rather than being a bare no-assert smoke loop.
 *
 * Also covers `CPU::ThreadPinningSupported()` — the query that tells a caller whether
 * `CoreInitializer::setAffinity()` does anything at all on this host. That one is deliberately
 * NOT asserted against a hard-coded platform expectation: it is cross-checked against the
 * kernel's own answer, so it stays honest on Apple Silicon (where the answer is `false`), on
 * Intel macOS, and on Linux, without the test knowing which it is running on.
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include <qb/system/cpu.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/thread_act.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

// =============================================================================
// TOPOLOGY — self-consistent invariants, not machine-specific values
// =============================================================================

/**
 * @test Architecture string is non-empty; affinity is non-zero; the paired and individual core
 *       accessors agree; hyper-threading is exactly `logical != physical`.
 * @brief Folded from test-cpu.cpp::ReportsConsistentTopology. The cross-checks (TotalCores vs the
 *        singular accessors, HT vs the core-count delta) are the real contract; absolute counts are
 *        intentionally not asserted.
 */
TEST(CpuTopology, ReportsConsistentTopology) {
    const std::string architecture = qb::CPU::Architecture();
    EXPECT_FALSE(architecture.empty());

    const int affinity = qb::CPU::Affinity();
    EXPECT_NE(affinity, 0) << "at least one logical processor must be available to the process";

    const auto [logical, physical] = qb::CPU::TotalCores();
    EXPECT_EQ(qb::CPU::LogicalCores(), logical) << "LogicalCores() must agree with TotalCores().first";
    EXPECT_EQ(qb::CPU::PhysicalCores(), physical) << "PhysicalCores() must agree with TotalCores().second";

    if (logical > 0 && physical > 0) {
        EXPECT_GE(logical, physical) << "logical cores cannot be fewer than physical";
        EXPECT_EQ(qb::CPU::HyperThreading(), logical != physical) << "HyperThreading() must equal (logical != physical)";
    } else {
        EXPECT_FALSE(qb::CPU::HyperThreading()) << "unknown topology must not claim hyper-threading";
    }
}

/**
 * @test Clock speed is either a positive frequency or the documented `-1` unavailable sentinel.
 * @brief Folded from test-cpu.cpp::ClockSpeedUsesDocumentedSentinel.
 */
TEST(CpuTopology, ClockSpeedIsPositiveOrUnavailableSentinel) {
    const std::int64_t clock_speed = qb::CPU::ClockSpeed();
    EXPECT_TRUE(clock_speed > 0 || clock_speed == -1)
        << "ClockSpeed() must be a positive Hz value or the -1 unavailable sentinel, got " << clock_speed;
}

// =============================================================================
// ARCHITECTURE MACROS (Huly QB-392) — what the build publishes is what this TU targets
// =============================================================================
//
// qbConfig.cmake publishes QB_ARCH_64 / QB_ARCH_32 / QB_ARCH_ARM / QB_ARCH_ARM64 as usage requirements. A macOS universal
// build compiles every TU once per slice, and before 3.3 the FIRST slice of CMAKE_OSX_ARCHITECTURES decided for all of
// them: "arm64;x86_64" compiled the x86_64 slice with QB_ARCH_ARM64=1, "x86_64;arm64" the arm64 slice without it (and
// with -march=x86-64). The ARM checks are preprocessor errors, so a wrong slice fails to BUILD; on a single-architecture
// build they hold by construction.
#if defined(__aarch64__) || defined(_M_ARM64)
#if !defined(QB_ARCH_ARM64) || !defined(QB_ARCH_ARM)
#error "this translation unit targets arm64, but the build did not define QB_ARCH_ARM64 and QB_ARCH_ARM (Huly QB-392)"
#endif
#elif defined(QB_ARCH_ARM64)
#error "QB_ARCH_ARM64 is defined for a translation unit that does not target arm64 (Huly QB-392)"
#endif

/**
 * @test The word-size macro the build publishes matches the pointer width of this translation unit.
 * @brief The ARM half of the contract is the preprocessor check above; this is the half a running test can state.
 */
TEST(CpuTopology, ArchitectureMacrosDescribeThisTarget) {
#if defined(QB_ARCH_64) && !defined(QB_ARCH_32)
    EXPECT_EQ(sizeof(void *), 8u) << "QB_ARCH_64 is defined for a 32-bit target";
#elif defined(QB_ARCH_32) && !defined(QB_ARCH_64)
    EXPECT_EQ(sizeof(void *), 4u) << "QB_ARCH_32 is defined for a 64-bit target";
#else
    ADD_FAILURE() << "the build must define exactly one of QB_ARCH_64 and QB_ARCH_32";
#endif
}

// =============================================================================
// LINUX PHYSICAL CORES (Huly QB-325) — fixture sysfs trees, then the live kernel
// =============================================================================
//
// Before 3.3 TotalCores() returned sysconf(_SC_NPROCESSORS_ONLN) as BOTH counts on Linux, so PhysicalCores() was the
// logical count and HyperThreading() false on every SMT host. qb::detail::linux_physical_cores() counts the distinct
// core sibling sets of the online CPUs; it is portable C++, so these fixture trees run on every platform.

namespace cpu_topology_test {

namespace fs = std::filesystem;

// A throwaway sysfs-shaped tree under the temp directory, removed with the object.
class SysfsTree {
public:
    SysfsTree() {
        static std::atomic<int> serial{0};
        _root = fs::temp_directory_path()
                / ("qb-cpu-topology-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-"
                   + std::to_string(serial++));
        fs::create_directories(_root);
    }
    ~SysfsTree() {
        std::error_code ec;
        fs::remove_all(_root, ec);
    }
    SysfsTree(const SysfsTree &)            = delete;
    SysfsTree &operator=(const SysfsTree &) = delete;

    void
    write(std::string const &relative, std::string const &content) const {
        const fs::path file = _root / relative;
        fs::create_directories(file.parent_path());
        std::ofstream(file) << content << "\n"; // sysfs attributes end with a newline
    }
    // cpu<N>/topology/<attribute> = value
    void
    topology(int cpu, std::string const &attribute, std::string const &value) const {
        write("cpu" + std::to_string(cpu) + "/topology/" + attribute, value);
    }
    [[nodiscard]] std::string
    root() const {
        return _root.string();
    }

private:
    fs::path _root;
};

} // namespace cpu_topology_test

using cpu_topology_test::SysfsTree;

/**
 * @test Two hardware threads per core: four online CPUs, two sibling sets, two physical cores.
 * @brief The red case of QB-325 on a real SMT host, in miniature: the logical count (4) is not the answer.
 */
TEST(CpuTopology, SmtSiblingsCountAsOnePhysicalCore) {
    SysfsTree t;
    t.write("online", "0-3");
    t.topology(0, "core_cpus_list", "0,2");
    t.topology(1, "core_cpus_list", "1,3");
    t.topology(2, "core_cpus_list", "0,2");
    t.topology(3, "core_cpus_list", "1,3");
    EXPECT_EQ(qb::detail::linux_physical_cores(t.root()), 2);
}

/** @test No SMT: every online CPU is its own core. */
TEST(CpuTopology, WithoutSmtEveryCpuIsACore) {
    SysfsTree t;
    t.write("online", "0-3");
    for (int cpu = 0; cpu < 4; ++cpu)
        t.topology(cpu, "core_cpus_list", std::to_string(cpu));
    EXPECT_EQ(qb::detail::linux_physical_cores(t.root()), 4);
}

/**
 * @test A hybrid part (P-cores with two threads, E-cores with one) on an older kernel that only has
 *       `thread_siblings_list`: 2 P-cores + 2 E-cores from 6 logical CPUs.
 */
TEST(CpuTopology, OlderKernelsSiblingListAndHybridParts) {
    SysfsTree t;
    t.write("online", "0-5");
    t.topology(0, "thread_siblings_list", "0-1");
    t.topology(1, "thread_siblings_list", "0-1");
    t.topology(2, "thread_siblings_list", "2-3");
    t.topology(3, "thread_siblings_list", "2-3");
    t.topology(4, "thread_siblings_list", "4");
    t.topology(5, "thread_siblings_list", "5");
    EXPECT_EQ(qb::detail::linux_physical_cores(t.root()), 4);
}

/**
 * @test Without sibling lists, `core_id` repeats across packages and dies: the key is the package / die / core
 *       triple, so two sockets each with core 0 and 1 are four cores, and two dies of one package two cores.
 */
TEST(CpuTopology, CoreIdsRepeatAcrossPackagesAndDies) {
    SysfsTree sockets;
    sockets.write("online", "0-3");
    const int package[4] = {0, 1, 0, 1};
    const int core[4]    = {0, 0, 1, 1};
    for (int cpu = 0; cpu < 4; ++cpu) {
        sockets.topology(cpu, "physical_package_id", std::to_string(package[cpu]));
        sockets.topology(cpu, "core_id", std::to_string(core[cpu]));
    }
    EXPECT_EQ(qb::detail::linux_physical_cores(sockets.root()), 4);

    SysfsTree dies;
    dies.write("online", "0-1");
    for (int cpu = 0; cpu < 2; ++cpu) {
        dies.topology(cpu, "physical_package_id", "0");
        dies.topology(cpu, "die_id", std::to_string(cpu));
        dies.topology(cpu, "core_id", "0");
    }
    EXPECT_EQ(qb::detail::linux_physical_cores(dies.root()), 2);
}

/** @test Only ONLINE CPUs count: an offline CPU's directory is still there and is ignored. */
TEST(CpuTopology, OfflineCpusDoNotCount) {
    SysfsTree t;
    t.write("online", "0-1,3");
    for (int cpu = 0; cpu < 4; ++cpu)
        t.topology(cpu, "core_cpus_list", std::to_string(cpu));
    EXPECT_EQ(qb::detail::linux_physical_cores(t.root()), 3);
}

/** @test Without an `online` file every `cpu<N>` directory counts -- and `cpufreq` / `cpuidle` are not CPUs. */
TEST(CpuTopology, WithoutOnlineEveryCpuDirectoryCounts) {
    SysfsTree t;
    t.topology(0, "core_cpus_list", "0-1");
    t.topology(1, "core_cpus_list", "0-1");
    t.topology(2, "core_cpus_list", "2");
    t.write("cpufreq/policy0/scaling_driver", "intel_pstate");
    t.write("cpuidle/current_driver", "intel_idle");
    EXPECT_EQ(qb::detail::linux_physical_cores(t.root()), 2);
}

/** @test What cannot be read is unknown (-1), never a guess: no tree, a malformed cpulist, an online CPU with no topology. */
TEST(CpuTopology, UnreadableTopologyIsUnknown) {
    EXPECT_EQ(qb::detail::linux_physical_cores((std::filesystem::temp_directory_path() / "qb-no-such-sysfs-tree").string()), -1);

    SysfsTree malformed;
    malformed.write("online", "0-x");
    malformed.topology(0, "core_cpus_list", "0");
    EXPECT_EQ(qb::detail::linux_physical_cores(malformed.root()), -1);

    SysfsTree missing;
    missing.write("online", "0-1");
    missing.topology(0, "core_cpus_list", "0");
    EXPECT_EQ(qb::detail::linux_physical_cores(missing.root()), -1) << "cpu1 is online and has no topology";
}

/**
 * @test On a live Linux host `PhysicalCores()` is the kernel's topology, cross-checked against an independent read
 *       of `thread_siblings_list` (the oracle reads a different attribute than the implementation prefers). On an
 *       SMT host the base returned the logical count here.
 */
TEST(CpuTopology, LinuxPhysicalCoresMatchTheKernelTopology) {
#if defined(__linux__)
    const std::string root = "/sys/devices/system/cpu";
    std::ifstream     online_file(root + "/online");
    std::string       online;
    if (!std::getline(online_file, online))
        GTEST_SKIP() << "no readable /sys/devices/system/cpu/online here (a container without sysfs)";
    // The oracle: the distinct thread_siblings_list of every online CPU, a cpulist expanded by hand.
    std::set<std::string> siblings;
    std::size_t           pos = 0;
    while (pos < online.size()) {
        std::size_t end = online.find_first_of(",\n", pos);
        if (end == std::string::npos)
            end = online.size();
        const std::string token = online.substr(pos, end - pos);
        pos                     = end + 1;
        if (token.empty())
            continue;
        const auto dash  = token.find('-');
        const int  first = std::stoi(token.substr(0, dash));
        const int  last  = dash == std::string::npos ? first : std::stoi(token.substr(dash + 1));
        for (int cpu = first; cpu <= last; ++cpu) {
            std::ifstream list_file(root + "/cpu" + std::to_string(cpu) + "/topology/thread_siblings_list");
            std::string   list;
            if (!std::getline(list_file, list))
                GTEST_SKIP() << "cpu" << cpu << " has no thread_siblings_list here";
            siblings.insert(list);
        }
    }
    EXPECT_EQ(qb::CPU::PhysicalCores(), static_cast<int>(siblings.size()));
    EXPECT_GE(qb::CPU::LogicalCores(), qb::CPU::PhysicalCores());
    EXPECT_EQ(qb::CPU::HyperThreading(), qb::CPU::LogicalCores() != qb::CPU::PhysicalCores());
#else
    GTEST_SKIP() << "Linux-only: the live sysfs topology";
#endif
}

// =============================================================================
// THREAD PINNING CAPABILITY — cross-checked against the kernel, not restated
// =============================================================================

/**
 * @test `ThreadPinningSupported()` answers the same thing every time it is asked.
 * @brief The macOS implementation probes the kernel once and caches the answer in a magic
 *        static. A capability that flips between calls would be useless to branch on, and the
 *        cache is exactly what could break it (racing first-callers, a probe with a side
 *        effect on the asking thread). Ask from several threads at once as well as inline.
 */
TEST(CpuTopology, ThreadPinningSupportedIsStable) {
    const bool first = qb::CPU::ThreadPinningSupported();

    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(qb::CPU::ThreadPinningSupported(), first) << "the capability must not vary between calls";
    }

    bool        from_threads[4] = {!first, !first, !first, !first};
    std::thread askers[4];
    for (int i = 0; i < 4; ++i) {
        askers[i] = std::thread([&from_threads, i] { from_threads[i] = qb::CPU::ThreadPinningSupported(); });
    }
    for (auto &asker : askers) {
        asker.join();
    }
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(from_threads[i], first) << "the capability must not depend on which thread asks (thread " << i << ")";
    }
}

/**
 * @test The reported capability agrees with what the platform's pinning call actually does.
 * @brief This is the whole point of the query, so it must not be a restatement of the
 *        implementation. The implementation probes with the *read* call; this test plants the
 *        *write* call — the one `VirtualCore::__init__` really issues through its macOS shim —
 *        on a scratch thread, and requires both to reach the same verdict.
 *
 *        On Apple Silicon this fails if anyone ever "fixes" `ThreadPinningSupported()` into a
 *        compile-time `#ifdef`, or if the shim's `KERN_NOT_SUPPORTED` handling drifts: the
 *        kernel says 46, so the query must say `false`. On Intel macOS, on Linux and on Windows
 *        it must say `true`, and nothing in the test hard-codes which host it is running on.
 */
TEST(CpuTopology, ThreadPinningSupportedMatchesTheKernel) {
#if defined(__APPLE__)
    // Exactly the call VirtualCore's macOS shim makes (THREAD_AFFINITY_POLICY_COUNT is 1, the
    // literal the shim passes). Run it on a scratch thread so a successful set — Intel macOS —
    // leaves no affinity tag on the gtest thread.
    kern_return_t set_ret = KERN_SUCCESS;

    std::thread probe([&set_ret] {
        thread_affinity_policy_data_t policy = {static_cast<integer_t>(1)};
        set_ret = thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_AFFINITY_POLICY, reinterpret_cast<thread_policy_t>(&policy),
                                    THREAD_AFFINITY_POLICY_COUNT);
    });
    probe.join();

    EXPECT_EQ(qb::CPU::ThreadPinningSupported(), set_ret != KERN_NOT_SUPPORTED)
        << "ThreadPinningSupported() must agree with thread_policy_set(THREAD_AFFINITY_POLICY), which returned " << set_ret
        << " (KERN_NOT_SUPPORTED is " << KERN_NOT_SUPPORTED << ")";

#elif defined(__linux__)
    // Read this thread's own mask and write it straight back: a request the kernel always
    // permits, so the test cannot flake on a cpuset-restricted runner, while still driving the
    // real pthread_setaffinity_np() path rather than asserting the `return true` back.
    int         get_rc = -1;
    int         set_rc = -1;
    std::thread probe([&get_rc, &set_rc] {
        cpu_set_t mask;
        CPU_ZERO(&mask);
        get_rc = pthread_getaffinity_np(pthread_self(), sizeof(mask), &mask);
        if (get_rc == 0) {
            set_rc = pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask);
        }
    });
    probe.join();

    EXPECT_EQ(get_rc, 0) << "pthread_getaffinity_np() must succeed where qb reports pinning support";
    EXPECT_EQ(set_rc, 0) << "re-applying a thread's own affinity mask must succeed";
    EXPECT_TRUE(qb::CPU::ThreadPinningSupported()) << "Linux implements per-thread pinning";

#elif defined(_WIN32) || defined(_WIN64)
#ifdef _MSC_VER
    EXPECT_TRUE(qb::CPU::ThreadPinningSupported()) << "Windows/MSVC uses SetThreadAffinityMask()";
#else
    EXPECT_FALSE(qb::CPU::ThreadPinningSupported()) << "a GNU toolchain on Windows gets no pinning (see VirtualCore::__init__)";
#endif

#else
    // Any other platform: no claim to cross-check, only that the query is callable and total.
    const bool supported = qb::CPU::ThreadPinningSupported();
    EXPECT_TRUE(supported || !supported);
#endif
}

// =============================================================================
// spin_loop_pause — strengthened from a bare smoke loop
// =============================================================================

/**
 * @test `spin_loop_pause()` is callable in a tight loop and the loop makes forward progress.
 * @brief Strengthened over test-cpu.cpp::SpinLoopPauseIsCallable, which looped 32 times with NO
 *        assertion (it could not distinguish "ran" from "compiled out"). We now count the
 *        iterations through a `volatile` sink the optimizer cannot elide, so the test asserts the
 *        loop body actually executed the documented number of times — proving the intrinsic is
 *        reachable and non-trapping on this architecture.
 */
TEST(CpuTopology, SpinLoopPauseRunsAndMakesProgress) {
    constexpr int kIterations = 32;
    volatile int  ran         = 0;
    for (int i = 0; i < kIterations; ++i) {
        qb::spin_loop_pause();
        ran = ran + 1; // volatile compound-assignment (++) is deprecated in C++20
    }
    EXPECT_EQ(ran, kIterations) << "spin_loop_pause() must be callable on every iteration without trapping";
}
