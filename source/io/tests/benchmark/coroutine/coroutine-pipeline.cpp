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
 *   - `ag_map` / `ag_filter` over an `async_generator` — items/sec.
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
        qb::io::async::run_for(1ms);
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
                     .reduce([](std::uint64_t acc, std::uint64_t v) { return acc + v; }, std::uint64_t{0});
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

// Coordinator: ag_map(gen, *2) then sum, and ag_filter(gen, even) then count.
task<void>
run_ag_map_filter(std::uint64_t count, std::atomic<std::uint64_t> *map_sum, std::atomic<std::size_t> *filt_count, std::atomic<bool> *done) {
    auto          doubled = co_await ag_map(gen_range(count), [](std::uint64_t v) { return v * 2u; });
    std::uint64_t s       = 0;
    for (auto v : doubled)
        s += v;
    map_sum->store(s, std::memory_order_relaxed);

    auto evens = co_await ag_filter(gen_range(count), [](std::uint64_t v) { return (v & 1u) == 0u; });
    filt_count->store(evens.size(), std::memory_order_relaxed);

    done->store(true, std::memory_order_relaxed);
    co_return;
}

// ---------------------------------------------------------------------------
// async_generator combinators: ag_map + ag_filter collecting into vectors. The
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
        state.SkipWithError("async_generator ag_map/ag_filter produced unexpected results");

    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(count) * 2);
}

} // namespace

BENCHMARK(BM_Channel_TrySendTryRecv)->Arg(64)->Arg(1024)->Arg(8192)->ArgName("messages")->Unit(benchmark::kNanosecond);
BENCHMARK(BM_Channel_SendRecv)->Arg(64)->Arg(512)->Arg(2048)->ArgName("messages")->Unit(benchmark::kMicrosecond)->UseRealTime();
BENCHMARK(BM_Stream_FilterMapReduce)->Arg(64)->Arg(512)->Arg(4096)->ArgName("items")->Unit(benchmark::kMicrosecond)->UseRealTime();
BENCHMARK(BM_Stream_MapCollect)->Arg(64)->Arg(512)->Arg(4096)->ArgName("items")->Unit(benchmark::kMicrosecond)->UseRealTime();
BENCHMARK(BM_Generator_MapFilter)->Arg(64)->Arg(512)->Arg(4096)->ArgName("items")->Unit(benchmark::kMicrosecond)->UseRealTime();

BENCHMARK_MAIN();
