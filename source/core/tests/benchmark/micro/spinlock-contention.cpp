/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file benchmark/micro/spinlock-contention.cpp
 * @brief `qb::lockfree::SpinLock` — uncontended acquire/release latency + N-thread contended throughput.
 *
 * Two complementary micro-benches over the framework spinlock (`qb/system/lockfree/spinlock.h`), with
 * no `qb::Main` / event loop:
 *   - `BM_SpinLock_Uncontended` — single thread does `lock()`+`unlock()` in a tight loop. With zero
 *     contention this is the floor cost of one acquire/release pair (the TTAS fast path). A
 *     non-atomic counter incremented inside the critical section is `DoNotOptimize`d so the body
 *     cannot be elided.
 *   - `BM_SpinLock_Contended` — `threads` worker threads each take the lock `kItersPerThread` times
 *     and increment ONE shared non-atomic counter under it; the iteration measures the wall-clock to
 *     drain all `threads * kItersPerThread` critical sections. The workload shape is lifted from the
 *     `SpinLock.MutualExclusionUnderContention` unit case (8 threads × 5000), generalised across a
 *     thread sweep capped to `cappedBenchmarkCores()`.
 *
 * The contended bench keeps the unit test's load-bearing oracle as a benchmark-time invariant: the
 * final counter MUST equal `threads * kItersPerThread`. A broken lock loses increments, so this is
 * the correctness proof that the throughput number is real (not a lock that "ran fast" by dropping
 * critical sections). It is checked OUT of the timed loop (one-shot, after the run) — never an
 * `EXPECT_LT(duration,…)` gate; this is a perf harness, not a ctest assertion.
 *
 * Benchmark methodology:
 *   - per-iteration thread spin-up is the cost of a contended lock acquisition pattern, so it stays
 *     inside the timed region deliberately (the bench measures end-to-end contended drain); the
 *     fresh `SpinLock` + counter are constructed inside the loop and only their teardown is trivial;
 *   - `SetItemsProcessed` is the total critical-section count, assigned ONCE after the loop;
 *   - the start gate (`std::barrier`) releases all workers together so the run reflects sustained
 *     contention, not staggered thread launch.
 *
 * New bench (no predecessor in the flat `benchmark/` set); pairs with the SpinLock unit suite.
 */

#include <atomic>
#include <barrier>
#include <benchmark/benchmark.h>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <qb/system/lockfree/spinlock.h>
#include <qb/utility/compat.h> // qb::jthread

#include "../../shared/BenchmarkCores.h"

namespace {

using qb::lockfree::SpinLock;

// Per-thread critical-section count under contention (matches the unit test's 5000/thread shape).
constexpr std::uint64_t kItersPerThread = 5000;

void
BM_SpinLock_Uncontended(benchmark::State &state) {
    SpinLock      sl;
    std::uint64_t counter = 0; // non-atomic: guarded by the (uncontended) lock

    for (auto _ : state) {
        sl.lock();
        ++counter; // critical section — DoNotOptimize keeps the body live
        benchmark::DoNotOptimize(counter);
        sl.unlock();
    }

    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
}

// Drain `threads * kItersPerThread` critical sections; return the final guarded counter so the
// caller can assert mutual exclusion held (no lost increments).
[[nodiscard]] std::uint64_t
run_contended(std::size_t const threads) {
    SpinLock      sl;
    std::uint64_t counter = 0; // guarded by sl — NON-atomic, so exclusivity must be real
    std::barrier  start_gate(static_cast<std::ptrdiff_t>(threads));

    std::vector<qb::jthread> workers;
    workers.reserve(threads);
    for (std::size_t t = 0; t < threads; ++t) {
        workers.emplace_back([&sl, &counter, &start_gate] {
            start_gate.arrive_and_wait();
            for (std::uint64_t i = 0; i < kItersPerThread; ++i) {
                sl.lock();
                ++counter; // critical section
                sl.unlock();
            }
        });
    }
    workers.clear(); // join all workers
    return counter;
}

void
BM_SpinLock_Contended(benchmark::State &state) {
    const auto threads = static_cast<std::size_t>(state.range(0));
    if (threads == 0u) {
        state.SkipWithError("invalid range: threads is zero");
        return;
    }
    const std::uint64_t expected = static_cast<std::uint64_t>(threads) * kItersPerThread;

    // One-shot out-of-loop correctness probe: mutual exclusion must hold (no lost increments). A
    // broken lock fails HERE, before timing — not via a duration gate.
    {
        std::uint64_t got = run_contended(threads);
        benchmark::DoNotOptimize(got);
        if (got != expected) {
            state.SkipWithError("spinlock lost increments under contention (mutual exclusion broken)");
            return;
        }
    }

    for (auto _ : state) {
        std::uint64_t got = run_contended(threads);
        benchmark::DoNotOptimize(got);
    }

    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * expected));
    state.counters["critical_sections"] = benchmark::Counter(static_cast<double>(expected), benchmark::Counter::kIsIterationInvariantRate);
    state.counters["threads"]           = static_cast<double>(threads);
}

void
ArgsContended(benchmark::internal::Benchmark *b) {
    const auto cap = qb::bench::cappedBenchmarkCores();
    for (std::uint32_t t = 1u; t <= std::min<std::uint32_t>(8u, cap); t *= 2u)
        b->Args({static_cast<std::int64_t>(t)});
    // Always include the unit-test shape (8 threads) even on smaller machines, to exercise
    // oversubscribed contention.
    if (cap < 8u)
        b->Args({8});
}

} // namespace

BENCHMARK(BM_SpinLock_Uncontended)->Unit(benchmark::kNanosecond);
BENCHMARK(BM_SpinLock_Contended)->Apply(ArgsContended)->ArgNames({"threads"})->Unit(benchmark::kMicrosecond)->UseRealTime();

BENCHMARK_MAIN();
