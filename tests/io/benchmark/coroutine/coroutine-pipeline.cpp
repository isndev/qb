/**
 * @file qb/io/tests/benchmark/coroutine/coroutine-pipeline.cpp
 * @brief Benchmarks for qb-io coroutine pipelines — channels, streams, generators.
 *
 * The data-plane of the coroutine layer:
 *   - `channel<T>` try_send/try_recv (the synchronous fast path) and the
 *     suspending send/recv fast path (buffer has space / receiver waiting) —
 *     msgs/sec;
 *   - `range_stream().filter().map().reduce()` / `.collect()` — the functional
 *     async-stream transform chain — items/sec;
 *   - `map_to_vector` / `filter_to_vector` over an `async_generator` — items/sec;
 *   - `recv_for`/`send_for`: the timed slow path won early, a fan-in `select` retracting its
 *     entries, and a `send_for` after a history of `recv_for` timeouts (Huly QB-273) — msgs/sec.
 *
 * All single-thread cooperative; throughput here is the per-message / per-item
 * scheduling + transform cost. Frameworks under
 * qb/io/async/coroutine/{channel,stream,generator}.h.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
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
 * limitations under the License.
 * @ingroup IO
 */

#include <atomic>
#include <benchmark/benchmark.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <qb/io/async/coroutine.h>

namespace {

using namespace qb::io::async;
using namespace std::chrono_literals;

void
reset_async_context() {
    qb::io::async::listener::current.clear();
    qb::io::async::init();
}

template <typename Predicate>
void
drain_until(Predicate &&pred, std::chrono::milliseconds timeout = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred() && std::chrono::steady_clock::now() < deadline) {
        coro_scheduler().run_ready();
        // Non-blocking pump: fire what is due, resume what is ready, never sleep. The previous
        // `run_for(1ms)` slept 1 ms per trip (~5 ms at Windows' timer granularity) even on the
        // trip that had just satisfied the predicate, so every cell measured the sleep, not the
        // work (0 CPU-time per iteration was the tell).
        qb::io::async::run(EVRUN_NOWAIT);
    }
}

// ---------------------------------------------------------------------------
// channel try_send / try_recv: the lock-free synchronous fast path. A buffered
// channel large enough to hold the batch, so try_send always succeeds and
// try_recv always returns a value. msgs/sec == 2 ops (send + recv) per message.
// ---------------------------------------------------------------------------
void
BM_Channel_TrySendTryRecv(benchmark::State &state) {
    const auto count = static_cast<std::size_t>(state.range(0));
    qb::io::async::init();

    channel<std::uint64_t> ch(count + 1u);
    std::uint64_t          sink = 0;

    for (auto _ : state) {
        for (std::uint64_t i = 0; i < count; ++i)
            benchmark::DoNotOptimize(ch.try_send(i));
        for (std::size_t i = 0; i < count; ++i) {
            auto v = ch.try_recv();
            sink += v ? *v : 0u;
        }
        benchmark::DoNotOptimize(sink);
    }

    qb::io::async::listener::current.clear();

    // Expected total: each iteration drains 0+1+...+(count-1).
    const std::uint64_t per_iter = (count == 0) ? 0u : (count - 1u) * count / 2u;
    const std::uint64_t expected = per_iter * static_cast<std::uint64_t>(state.iterations());
    if (sink != expected)
        state.SkipWithError("channel try_send/try_recv produced an unexpected sum");

    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(count));
}

// Producer coroutine: send `count` values through the channel, then close.
task<void>
channel_producer(channel<std::uint64_t> *ch, std::size_t count) {
    for (std::uint64_t i = 0; i < count; ++i)
        co_await ch->send(i);
    ch->close();
    co_return;
}

// Consumer coroutine: drain the channel via the suspending recv fast path.
task<void>
channel_consumer(channel<std::uint64_t> *ch, std::atomic<std::uint64_t> *sink, std::atomic<bool> *done) {
    std::uint64_t acc = 0;
    while (true) {
        auto v = co_await ch->recv();
        if (!v)
            break;
        acc += *v;
    }
    sink->store(acc, std::memory_order_relaxed);
    done->store(true, std::memory_order_relaxed);
    co_return;
}

// ---------------------------------------------------------------------------
// channel send/recv suspending fast path: a buffered producer/consumer pair
// drains `count` messages through co_await send/recv. Measures the awaiter +
// scheduler hand-off per message (the parked-then-woken hot path).
// ---------------------------------------------------------------------------
void
BM_Channel_SendRecv(benchmark::State &state) {
    const auto count = static_cast<std::size_t>(state.range(0));

    std::uint64_t last = 0;
    for (auto _ : state) {
        state.PauseTiming();
        reset_async_context();
        channel<std::uint64_t>     ch(64);
        std::atomic<std::uint64_t> sink{0};
        std::atomic<bool>          done{false};
        state.ResumeTiming();

        coro_scheduler().spawn(channel_consumer(&ch, &sink, &done));
        coro_scheduler().spawn(channel_producer(&ch, count));
        drain_until([&done] { return done.load(std::memory_order_relaxed); });

        last = sink.load(std::memory_order_relaxed);
        benchmark::DoNotOptimize(last);

        state.PauseTiming();
        reset_async_context();
        state.ResumeTiming();
    }

    const std::uint64_t expected = (count == 0) ? 0u : (count - 1u) * count / 2u;
    if (last != expected)
        state.SkipWithError("channel send/recv produced an unexpected sum");

    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(count));
}

// Coordinator: range_stream(0, count).filter(even).map(*2).reduce(sum).
task<void>
run_stream_reduce(std::uint64_t count, std::atomic<std::uint64_t> *sink, std::atomic<bool> *done) {
    auto total = co_await range_stream<std::uint64_t>(0, count)
                     .filter([](std::uint64_t v) { return (v & 1u) == 0u; })
                     .map([](std::uint64_t v) { return v * 2u; })
                     .reduce(std::uint64_t{0}, [](std::uint64_t acc, std::uint64_t v) { return acc + v; });
    sink->store(total, std::memory_order_relaxed);
    done->store(true, std::memory_order_relaxed);
    co_return;
}

// Coordinator: range_stream(0, count).map(+1).collect() — vector materialize.
task<void>
run_stream_collect(std::uint64_t count, std::atomic<std::size_t> *sink, std::atomic<bool> *done) {
    auto vec = co_await range_stream<std::uint64_t>(0, count).map([](std::uint64_t v) { return v + 1u; }).collect();
    sink->store(vec.size(), std::memory_order_relaxed);
    done->store(true, std::memory_order_relaxed);
    co_return;
}

// ---------------------------------------------------------------------------
// async_stream transform chain: range_stream().filter().map().reduce(). The
// functional pipeline cost — each element threads through the chained _next
// closures. items/sec is the source range size.
// ---------------------------------------------------------------------------
void
BM_Stream_FilterMapReduce(benchmark::State &state) {
    const auto count = static_cast<std::uint64_t>(state.range(0));

    std::uint64_t last = 0;
    for (auto _ : state) {
        state.PauseTiming();
        reset_async_context();
        std::atomic<std::uint64_t> sink{0};
        std::atomic<bool>          done{false};
        state.ResumeTiming();

        coro_scheduler().spawn(run_stream_reduce(count, &sink, &done));
        drain_until([&done] { return done.load(std::memory_order_relaxed); });

        last = sink.load(std::memory_order_relaxed);
        benchmark::DoNotOptimize(last);

        state.PauseTiming();
        reset_async_context();
        state.ResumeTiming();
    }

    // sum of 2*v over even v in [0, count).
    std::uint64_t expected = 0;
    for (std::uint64_t v = 0; v < count; v += 2u)
        expected += v * 2u;
    if (last != expected)
        state.SkipWithError("stream filter/map/reduce produced an unexpected sum");

    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(count));
}

// ---------------------------------------------------------------------------
// async_stream .collect(): range_stream().map().collect() into a vector. The
// terminal-materialize path. items/sec is the source range size.
// ---------------------------------------------------------------------------
void
BM_Stream_MapCollect(benchmark::State &state) {
    const auto count = static_cast<std::uint64_t>(state.range(0));

    std::size_t last = 0;
    for (auto _ : state) {
        state.PauseTiming();
        reset_async_context();
        std::atomic<std::size_t> sink{0};
        std::atomic<bool>        done{false};
        state.ResumeTiming();

        coro_scheduler().spawn(run_stream_collect(count, &sink, &done));
        drain_until([&done] { return done.load(std::memory_order_relaxed); });

        last = sink.load(std::memory_order_relaxed);
        benchmark::DoNotOptimize(last);

        state.PauseTiming();
        reset_async_context();
        state.ResumeTiming();
    }

    if (last != static_cast<std::size_t>(count))
        state.SkipWithError("stream map/collect produced an unexpected element count");

    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(count));
}

// An async generator yielding [0, count) with no suspension between yields.
async_generator<std::uint64_t>
gen_range(std::uint64_t count) {
    for (std::uint64_t i = 0; i < count; ++i)
        co_yield i;
}

// Coordinator: map_to_vector(gen, *2) then sum, and filter_to_vector(gen, even) then count.
task<void>
run_ag_map_filter(std::uint64_t count, std::atomic<std::uint64_t> *map_sum, std::atomic<std::size_t> *filt_count, std::atomic<bool> *done) {
    auto          doubled = co_await map_to_vector(gen_range(count), [](std::uint64_t v) { return v * 2u; });
    std::uint64_t s       = 0;
    for (auto v : doubled)
        s += v;
    map_sum->store(s, std::memory_order_relaxed);

    auto evens = co_await filter_to_vector(gen_range(count), [](std::uint64_t v) { return (v & 1u) == 0u; });
    filt_count->store(evens.size(), std::memory_order_relaxed);

    done->store(true, std::memory_order_relaxed);
    co_return;
}

// ---------------------------------------------------------------------------
// async_generator combinators: map_to_vector + filter_to_vector collecting into vectors. The
// co_yield → next() symmetric-transfer hand-off per element. items/sec counts
// both passes (map over count + filter over count).
// ---------------------------------------------------------------------------
void
BM_Generator_MapFilter(benchmark::State &state) {
    const auto count = static_cast<std::uint64_t>(state.range(0));

    std::uint64_t last_sum   = 0;
    std::size_t   last_count = 0;
    for (auto _ : state) {
        state.PauseTiming();
        reset_async_context();
        std::atomic<std::uint64_t> map_sum{0};
        std::atomic<std::size_t>   filt_count{0};
        std::atomic<bool>          done{false};
        state.ResumeTiming();

        coro_scheduler().spawn(run_ag_map_filter(count, &map_sum, &filt_count, &done));
        drain_until([&done] { return done.load(std::memory_order_relaxed); });

        last_sum   = map_sum.load(std::memory_order_relaxed);
        last_count = filt_count.load(std::memory_order_relaxed);
        benchmark::DoNotOptimize(last_sum);
        benchmark::DoNotOptimize(last_count);

        state.PauseTiming();
        reset_async_context();
        state.ResumeTiming();
    }

    const std::uint64_t expected_sum   = (count == 0) ? 0u : (count - 1u) * count;    // 2 * (0+...+count-1)
    const std::size_t   expected_count = static_cast<std::size_t>((count + 1u) / 2u); // evens in [0,count)
    if (last_sum != expected_sum || last_count != expected_count)
        state.SkipWithError("async_generator map_to_vector/filter_to_vector produced unexpected results");

    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(count) * 2);
}

// ---------------------------------------------------------------------------
// Huly QB-273: the timed slow path, a channel's history, and the retraction of a fan-in select.
//
// BM_Channel_TimedOpsWonEarly: `count` recv_for(1s) on a rendezvous channel against a plain producer. About
// half the receives park and are won by the next send (registration, watchdog armed then stopped, resume);
// the other half find the producer already parked and are resolved at registration, no watchdog armed. Before
// QB-273 every one of them allocated a shared select state and spawned a timer coroutine whose ev_timer stayed
// armed to the deadline; since, the awaiter owns both inside its frame (nothing allocated beyond the recv_for
// frame itself), registers the frame in the scheduler's suspended set while the watchdog is armed, as a sleep
// does, and stops the watchdog as the wait ends.
//
// BM_Channel_SelectFanIn: `k` selects parked at once on one shared channel (each with a channel of its own as
// the other operand), resolved one by one through their own channel -- the fan-in shape (N workers each
// selecting on a broadcast channel and a private one). Each resolved select retracts its entry from the shared
// channel: before QB-273 that entry stayed for a later send to skip, each registration holding a shared state
// allocated for it; since, the select's node lives in its awaiter and the retraction is one O(1) unlink from the
// shared channel's list, wherever the node sits -- constant per select, in k and in the order the selects resolve
// (this cell resolves them in registration order, so it always unlinks the head). This cell measures that path.
//
// BM_Channel_SendForAfterRecvForHistory: a rendezvous channel that saw `history` recv_for timeouts
// (set up outside the timing), then 64 send_for that must park and are taken by a recv. The
// send_for's fast-path decision asks `has_live_select_waiter()`, which scanned the whole history
// before QB-273 (one resolved entry per timeout, kept until a later send met it); since, a timed-out
// recv_for retracts its entry and the history is 0 by construction, so the cell is flat in `history`.
// ---------------------------------------------------------------------------

// Consumer: `count` timed receives with a far deadline, each won by the producer.
task<void>
timed_consumer(channel<std::uint64_t> *ch, std::size_t count, std::atomic<std::uint64_t> *sink, std::atomic<bool> *done) {
    std::uint64_t acc = 0;
    for (std::size_t i = 0; i < count; ++i) {
        auto v = co_await ch->recv_for(1s);
        if (!v)
            break;
        acc += *v;
    }
    sink->store(acc, std::memory_order_relaxed);
    done->store(true, std::memory_order_relaxed);
    co_return;
}

// Producer: `count` plain sends, no close (the consumer counts, not the close).
task<void>
plain_producer(channel<std::uint64_t> *ch, std::size_t count) {
    for (std::uint64_t i = 0; i < count; ++i)
        co_await ch->send(i);
    co_return;
}

void
BM_Channel_TimedOpsWonEarly(benchmark::State &state) {
    const auto count = static_cast<std::size_t>(state.range(0));

    std::uint64_t last = 0;
    for (auto _ : state) {
        state.PauseTiming();
        reset_async_context();
        channel<std::uint64_t>     ch(0); // rendezvous: every recv_for takes the slow path
        std::atomic<std::uint64_t> sink{0};
        std::atomic<bool>          done{false};
        state.ResumeTiming();

        coro_scheduler().spawn(timed_consumer(&ch, count, &sink, &done));
        coro_scheduler().spawn(plain_producer(&ch, count));
        drain_until([&done] { return done.load(std::memory_order_relaxed); });

        last = sink.load(std::memory_order_relaxed);
        benchmark::DoNotOptimize(last);

        state.PauseTiming();
        reset_async_context();
        state.ResumeTiming();
    }

    const std::uint64_t expected = (count == 0) ? 0u : (count - 1u) * count / 2u;
    if (last != expected)
        state.SkipWithError("channel timed ops produced an unexpected sum");

    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(count));
}

// One worker of the fan-in: selects on the shared channel and its own, resumes when its own delivers.
task<void>
fan_in_worker(channel<std::uint64_t> *shared, channel<std::uint64_t> *own, std::atomic<std::uint64_t> *sink, std::atomic<std::size_t> *done) {
    auto r = co_await select(*shared, *own);
    if (!r.closed && r.index == 1u)
        sink->fetch_add(r.get<std::uint64_t>(), std::memory_order_relaxed);
    done->fetch_add(1, std::memory_order_relaxed);
    co_return;
}

void
BM_Channel_SelectFanIn(benchmark::State &state) {
    const auto k = static_cast<std::size_t>(state.range(0));

    std::uint64_t last = 0;
    for (auto _ : state) {
        state.PauseTiming();
        reset_async_context();
        channel<std::uint64_t>                               shared(0);
        std::vector<std::unique_ptr<channel<std::uint64_t>>> own;
        own.reserve(k);
        std::atomic<std::uint64_t> sink{0};
        std::atomic<std::size_t>   done{0};
        for (std::size_t i = 0; i < k; ++i) {
            own.push_back(std::make_unique<channel<std::uint64_t>>(0));
            coro_scheduler().spawn(fan_in_worker(&shared, own.back().get(), &sink, &done));
        }
        drain_until([&shared, k] { return shared.waiter_census().selects == k; }); // every worker parked
        state.ResumeTiming();

        // Resolve them one by one through their own channel: each resume retracts one entry from `shared`.
        for (std::size_t i = 0; i < k; ++i)
            (void) own[i]->try_send(static_cast<std::uint64_t>(i + 1));
        drain_until([&done, k] { return done.load(std::memory_order_relaxed) == k; });

        last = sink.load(std::memory_order_relaxed);
        benchmark::DoNotOptimize(last);

        state.PauseTiming();
        reset_async_context();
        state.ResumeTiming();
    }

    const std::uint64_t expected = k * (k + 1u) / 2u;
    if (last != expected)
        state.SkipWithError("channel select fan-in produced an unexpected sum");

    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(k));
}

// The history: `history` timed receives on an empty rendezvous channel, one cooperative turn each,
// every one a timeout. Outside the timing.
task<void>
recv_for_history(channel<std::uint64_t> *ch, std::size_t history, std::atomic<bool> *done) {
    for (std::size_t i = 0; i < history; ++i)
        (void) co_await ch->recv_for(0ms);
    done->store(true, std::memory_order_relaxed);
    co_return;
}

// One timed send that must park: no receiver yet, no buffer (rendezvous).
task<void>
send_for_one(channel<std::uint64_t> *ch, std::uint64_t v) {
    (void) co_await ch->send_for(v, 1s);
    co_return;
}

// 64 rounds: a send_for parks (its fast-path decision scans the select registrations first), then a recv takes
// its value through the wake. The yield lets the sender run before the receiver parks, or the receiver's
// own entry would satisfy the sender's fast path before the scan.
task<void>
timed_senders_taken_by_recv(channel<std::uint64_t> *ch, std::size_t count, std::atomic<std::uint64_t> *sink, std::atomic<bool> *done) {
    std::uint64_t acc = 0;
    for (std::uint64_t i = 0; i < count; ++i) {
        coro_scheduler().spawn(send_for_one(ch, i));
        co_await sleep(0ms);
        auto v = co_await ch->recv();
        if (v)
            acc += *v;
    }
    sink->store(acc, std::memory_order_relaxed);
    done->store(true, std::memory_order_relaxed);
    co_return;
}

void
BM_Channel_SendForAfterRecvForHistory(benchmark::State &state) {
    const auto            history = static_cast<std::size_t>(state.range(0));
    constexpr std::size_t count   = 64;

    std::uint64_t last = 0;
    for (auto _ : state) {
        state.PauseTiming();
        reset_async_context();
        channel<std::uint64_t>     ch(0);
        std::atomic<std::uint64_t> sink{0};
        std::atomic<bool>          history_done{false};
        std::atomic<bool>          done{false};
        coro_scheduler().spawn(recv_for_history(&ch, history, &history_done));
        drain_until([&history_done] { return history_done.load(std::memory_order_relaxed); });
        state.ResumeTiming();

        coro_scheduler().spawn(timed_senders_taken_by_recv(&ch, count, &sink, &done));
        drain_until([&done] { return done.load(std::memory_order_relaxed); });

        last = sink.load(std::memory_order_relaxed);
        benchmark::DoNotOptimize(last);

        state.PauseTiming();
        reset_async_context();
        state.ResumeTiming();
    }

    const std::uint64_t expected = (count - 1u) * count / 2u;
    if (last != expected)
        state.SkipWithError("channel send_for after history produced an unexpected sum");

    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(count));
}

} // namespace

BENCHMARK(BM_Channel_TrySendTryRecv)->Arg(64)->Arg(1024)->Arg(8192)->ArgName("messages")->Unit(benchmark::kNanosecond);
BENCHMARK(BM_Channel_SendRecv)->Arg(64)->Arg(512)->Arg(2048)->ArgName("messages")->Unit(benchmark::kMicrosecond)->UseRealTime();
BENCHMARK(BM_Stream_FilterMapReduce)->Arg(64)->Arg(512)->Arg(4096)->ArgName("items")->Unit(benchmark::kMicrosecond)->UseRealTime();
BENCHMARK(BM_Stream_MapCollect)->Arg(64)->Arg(512)->Arg(4096)->ArgName("items")->Unit(benchmark::kMicrosecond)->UseRealTime();
BENCHMARK(BM_Generator_MapFilter)->Arg(64)->Arg(512)->Arg(4096)->ArgName("items")->Unit(benchmark::kMicrosecond)->UseRealTime();
BENCHMARK(BM_Channel_TimedOpsWonEarly)->Arg(64)->Arg(512)->Arg(2048)->ArgName("messages")->Unit(benchmark::kMicrosecond)->UseRealTime();
BENCHMARK(BM_Channel_SelectFanIn)->Arg(64)->Arg(256)->Arg(1024)->ArgName("selects")->Unit(benchmark::kMicrosecond)->UseRealTime();
BENCHMARK(BM_Channel_SendForAfterRecvForHistory)
    ->Arg(0)
    ->Arg(1024)
    ->Arg(8192)
    ->ArgName("history")
    ->Unit(benchmark::kMicrosecond)
    ->UseRealTime();

BENCHMARK_MAIN();
