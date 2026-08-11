/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file benchmark/micro/mpsc-mailbox-fanin.cpp
 * @brief Lock-free MPSC mailbox fan-in sweep (pseudo-framework primitive, no `qb::Main`).
 *
 * Drains `qb::lockfree::mpsc::ringbuffer<EventBucket, Cap, 0>` — the exact element type and
 * per-producer-slice routing model the engine uses for actor mailboxes — under sustained N→1
 * fan-in. The Google Benchmark template parameter `MailboxCap` is the compile-time per-producer
 * ring capacity (mirrors `Main.h`'s `MaxRingEvents`); registering several values lets you compare
 * throughput and `enqueue_failures` (busy-retry count when a ring is full) across sizes.
 *
 * The launch model (barrier start gate, one `qb::jthread` per producer busy-enqueueing its even
 * quota, one consumer draining by batch with the same `dequeue(Func, buf, batch)` overload as
 * `VirtualCore::__receive__`) lives once in `MpscFanInHarness.h`; this bench supplies a payload-free
 * `make_bucket` (a default `EventBucket` — payload-free is *deliberate*: this sweep measures raw ring
 * pressure / throughput, NOT payload routing, which is the router bench's job) and a no-op `consume`.
 *
 * Benchmark methodology (perf harness, never a ctest gate — no `EXPECT_LT(duration,…)`):
 *   - the whole timed region is `run_mpsc_fan_in()`; thread spin-up is absorbed by the start gate so
 *     each iteration measures sustained pressure, not launch cost;
 *   - `SetItemsProcessed` / `SetBytesProcessed` and the descriptor counters are assigned ONCE after
 *     the loop (`bytes_per_second` derives from `SetBytesProcessed`; payload-free buckets still move
 *     `sizeof(EventBucket)` cache-line-sized slots through the ring, so the byte column stays
 *     comparable across benches);
 *   - a one-shot, out-of-loop correctness probe runs the smallest fan-in and `DoNotOptimize`s the
 *     drained-bucket total, so a structurally broken harness (a consumer that never reaches `total`)
 *     is caught before any timing rather than becoming a timing gate. The harness bounds that drain
 *     and reports `stalled`, so the probe SkipWithErrors with the count it reached instead of
 *     hanging — which is how the same defect used to present.
 *
 * Rewritten from the former `bm-mpsc-mailbox-sweep.cpp` onto the shared harness.
 */

#include <algorithm>
#include <benchmark/benchmark.h>
#include <cstddef>
#include <cstdint>
#include <string>

#include <qb/utility/prefix.h>

#include "../../shared/MpscFanInHarness.h"

namespace {

using ::EventBucket; // EventBucket is declared in the GLOBAL namespace (qb/utility/prefix.h), not qb::

// Payload-free bucket: this sweep deliberately measures ring pressure / throughput only — no payload
// is written or inspected (the router bench owns the typed-dispatch dimension).
[[nodiscard]] EventBucket
make_empty_bucket(std::size_t /*pid*/, std::uint64_t /*sent*/) noexcept {
    return EventBucket{};
}

// No-op consumer: a drained batch is counted by the harness; nothing here inspects the payload.
void
consume_noop(EventBucket * /*buffer*/, std::size_t /*nb*/) noexcept {}

template <std::size_t MailboxCap>
void
BM_MpscMailbox_FanInDrain(benchmark::State &state) {
    const auto nb_producers  = static_cast<std::size_t>(state.range(0));
    const auto total         = static_cast<std::uint64_t>(state.range(1));
    const auto dequeue_batch = static_cast<std::size_t>(state.range(2));

    if (nb_producers == 0u || total == 0ull) {
        state.SkipWithError("invalid range: producers or total is zero");
        return;
    }

    // One-shot out-of-loop correctness probe: a consumer that never reaches `total` is a structurally
    // broken harness, caught here before any timing. It used to be caught by HANGING — the harness now
    // bounds the drain and reports it, so the same defect names itself instead of arriving as a job
    // that never finished. DoNotOptimize keeps the probe from folding.
    {
        const auto    probe       = qb::bench::run_mpsc_fan_in<MailboxCap>(nb_producers, total, dequeue_batch, make_empty_bucket, consume_noop);
        std::uint64_t probe_fails = probe.enqueue_failures; // non-const lvalue: the const-ref DoNotOptimize overload is deprecated
        benchmark::DoNotOptimize(probe_fails);
        if (probe.stalled) {
            state.SkipWithError(
                ("fan-in drain stalled: consumer took " + std::to_string(probe.drained) + " of " + std::to_string(total) + " buckets").c_str());
            return;
        }
    }

    std::uint64_t fails = 0;
    for (auto _ : state) {
        const auto run = qb::bench::run_mpsc_fan_in<MailboxCap>(nb_producers, total, dequeue_batch, make_empty_bucket, consume_noop);
        fails          = run.enqueue_failures;
        benchmark::DoNotOptimize(fails);
    }

    // Counters assigned once, after the timed loop.
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * total));
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * total * sizeof(EventBucket)));
    state.counters["items_per_s"]      = benchmark::Counter(static_cast<double>(total), benchmark::Counter::kIsIterationInvariantRate);
    state.counters["enqueue_failures"] = benchmark::Counter(static_cast<double>(fails), benchmark::Counter::kIsIterationInvariant);
    state.counters["failures_per_item"] =
        benchmark::Counter(static_cast<double>(fails) / static_cast<double>(total), benchmark::Counter::kIsIterationInvariant);
}

} // namespace

#define QB_REGISTER_MPSC_MAILBOX_BENCH(Cap)                          \
    BENCHMARK_TEMPLATE(BM_MpscMailbox_FanInDrain, Cap)               \
        ->Name("BM_MpscMailbox_FanInDrain/capacity_" #Cap)           \
        ->Apply(qb::bench::apply_mpsc_fan_in_args)                   \
        ->ArgNames({"producers", "total_messages", "dequeue_batch"}) \
        ->Unit(benchmark::kMillisecond)                              \
        ->UseRealTime()

// Sizes: small rings, powers of two, and ~framework mailbox depth (1023).
QB_REGISTER_MPSC_MAILBOX_BENCH(64);
QB_REGISTER_MPSC_MAILBOX_BENCH(128);
QB_REGISTER_MPSC_MAILBOX_BENCH(256);
QB_REGISTER_MPSC_MAILBOX_BENCH(512);
QB_REGISTER_MPSC_MAILBOX_BENCH(1023);
QB_REGISTER_MPSC_MAILBOX_BENCH(1024);
QB_REGISTER_MPSC_MAILBOX_BENCH(2048);

#undef QB_REGISTER_MPSC_MAILBOX_BENCH

BENCHMARK_MAIN();
