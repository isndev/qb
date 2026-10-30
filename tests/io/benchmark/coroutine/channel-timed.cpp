/**
 * @file qb/io/tests/benchmark/coroutine/channel-timed.cpp
 * @brief Benchmarks for the channel's timed slow path, its history and the retraction of a fan-in select (Huly QB-273).
 *
 * The three cells of QB-273 -- `recv_for`/`send_for` won early, a fan-in `select` retracting its entries, and a
 * `send_for` after a history of `recv_for` timeouts -- msgs/sec, single-thread cooperative. They live in a TU of
 * their own, apart from coroutine-pipeline.cpp: measured on g++ 14, the timed awaiters they instantiate grew that TU
 * past the compiler's per-unit inlining budget and `recv_awaiter::await_ready()` / `await_resume()` stopped inlining
 * into the pipeline cells that the dev/bench gate watches (+18.8 % instructions on `BM_Channel_SendRecv` with no
 * change to the channel). A gated cell's codegen must not depend on which other cells share its binary.
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
