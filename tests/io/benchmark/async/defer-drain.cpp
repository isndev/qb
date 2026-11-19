/**
 * @file qb/io/tests/benchmark/async/defer-drain.cpp
 * @brief What a deferred callback costs to queue and to run: a turn's drain, a turn opened from inside a drain, and the
 *        in-loop drain of a blocking run.
 *
 * `listener::defer()` is how a handler continues after it has unwound; a turn of the loop drains what was queued
 * before it began. Three shapes:
 *
 *  - `BM_Defer_DrainBatch`: N callbacks queued, one `run(EVRUN_NOWAIT)` drains them at its tail -- the cost of a
 *    deferred callback end to end (the queue's slot, the call through `std::function`, the drain's bound).
 *  - `BM_Defer_NestedTurn`: the first of N callbacks runs the loop itself, and that nested turn drains the N-1 queued
 *    behind it (Huly QB-1246) -- the same work through one more turn; it must read as `DrainBatch` does.
 *  - `BM_Defer_BlockingRunChain`: under a blocking `run()`, a callback that defers its successor N times. The first
 *    link is handed to the in-loop drain when the run begins; every later one is left by a callback that drain runs,
 *    so it costs the drain's zero-delay re-arm, that timer's expiry and one iteration of the loop with its poll --
 *    the path a standalone main loop takes for every deferred continuation of a deferred callback.
 *
 * Every cell checks, outside the timed loop, that exactly the callbacks it queued ran, where it says they ran.
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

#include <benchmark/benchmark.h>
#include <chrono>
#include <cstddef>
#include <cstdint>

#include <qb/io/async.h>
#include <qb/io/async/listener.h>

namespace {

namespace async = qb::io::async;

void
BM_Defer_DrainBatch(benchmark::State &state) {
    async::init();
    async::listener::current.clear();
    const auto    batch = static_cast<std::size_t>(state.range(0));
    std::uint64_t ran   = 0;
    for (auto _ : state) {
        for (std::size_t i = 0; i < batch; ++i)
            async::defer([&ran] { ++ran; });
        async::listener::current.run(EVRUN_NOWAIT);
    }
    if (ran != static_cast<std::uint64_t>(state.iterations()) * batch)
        state.SkipWithError("a turn did not run exactly the callbacks queued before it");
    if (async::listener::current.has_deferred())
        state.SkipWithError("a callback was left queued");
    state.SetItemsProcessed(static_cast<std::int64_t>(ran));
    benchmark::DoNotOptimize(ran);
}

void
BM_Defer_NestedTurn(benchmark::State &state) {
    async::init();
    async::listener::current.clear();
    const auto    batch      = static_cast<std::size_t>(state.range(0));
    std::uint64_t ran        = 0;
    std::uint64_t ran_inside = 0; // by the turn the first callback opened
    for (auto _ : state) {
        async::defer([&ran, &ran_inside] {
            const std::uint64_t before = ran;
            async::listener::current.run(EVRUN_NOWAIT);
            ran_inside += ran - before;
            ++ran;
        });
        for (std::size_t i = 1; i < batch; ++i)
            async::defer([&ran] { ++ran; });
        async::listener::current.run(EVRUN_NOWAIT);
    }
    const auto turns = static_cast<std::uint64_t>(state.iterations());
    if (ran != turns * batch)
        state.SkipWithError("the turns did not run exactly the callbacks queued");
    if (ran_inside != turns * (batch - 1))
        state.SkipWithError("the turn a callback opened did not run the callbacks queued behind it");
    state.SetItemsProcessed(static_cast<std::int64_t>(ran));
    benchmark::DoNotOptimize(ran);
}

/// One link of BM_Defer_BlockingRunChain: runs, and defers the next while any is left; the last one ends the run.
struct Link {
    std::uint64_t *ran;
    std::size_t    left;

    void
    operator()() const {
        ++*ran;
        if (left != 0)
            async::defer(Link{ran, left - 1});
        else
            async::break_parent();
    }
};

void
BM_Defer_BlockingRunChain(benchmark::State &state) {
    async::init();
    async::listener::current.clear();
    const auto    links = static_cast<std::size_t>(state.range(0));
    std::uint64_t ran   = 0;
    // A watcher that never fires: with none the loop has nothing to block on, and `run()` would be one turn.
    async::callback([] {}, std::chrono::hours(1));
    for (auto _ : state) {
        async::defer(Link{&ran, links - 1});
        // Blocking: the in-loop drain runs one link an iteration, and the last link breaks the loop.
        async::listener::current.run();
    }
    const bool left_queued = async::listener::current.has_deferred();
    async::listener::current.clear(); // the watcher
    if (ran != static_cast<std::uint64_t>(state.iterations()) * links)
        state.SkipWithError("the blocking run did not run every link of the chain");
    if (left_queued)
        state.SkipWithError("a link was left queued");
    state.SetItemsProcessed(static_cast<std::int64_t>(ran));
    benchmark::DoNotOptimize(ran);
}

} // namespace

BENCHMARK(BM_Defer_DrainBatch)->Arg(1)->Arg(16)->Arg(256)->ArgName("callbacks")->Unit(benchmark::kNanosecond);
BENCHMARK(BM_Defer_NestedTurn)->Arg(16)->Arg(256)->ArgName("callbacks")->Unit(benchmark::kNanosecond);
BENCHMARK(BM_Defer_BlockingRunChain)->Arg(256)->ArgName("links")->Unit(benchmark::kNanosecond);

BENCHMARK_MAIN();
