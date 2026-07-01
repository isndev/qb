/**
 * @file qb/io/tests/benchmark/coroutine/sync-primitives.cpp
 * @brief Throughput of the qb-io coroutine synchronization primitives (sync.h).
 *
 * `semaphore` / `async_mutex` / `async_rw_lock` / `async_latch` are single-thread cooperative
 * awaiters (no OS locks): a coroutine that cannot acquire PARKS (suspends, yielding the thread) and
 * is resumed when a holder `release()`s. This bench prices exactly that machinery as the number of
 * contending coroutines grows — the fast path (acquire with a free permit, no suspension) vs the
 * park/wake path (acquire behind a held lock ⇒ suspend + later resume). These primitives gained a
 * destroy-while-parked retracting-dtor guard (wave10); this is their first perf coverage.
 *
 * Harness: the standard qb-io coroutine-bench pattern — a fresh single-thread loop + coro scheduler
 * per iteration (`reset_async_context`), N worker `task<void>`s spawned onto `coro_scheduler()`, and
 * `drain_until` pumping the ready-queue + libev loop to completion. No actor engine.
 *
 * Correctness: each worker bumps a shared counter inside its critical section and a completion
 * atomic on exit; a one-shot out-of-loop probe asserts `counter == N` (every critical section ran —
 * a wedged park/wake would leave a worker parked and the count short). The counter is also the
 * anti-elision sink. NB: the scheduler is cooperative and the critical sections contain no
 * `co_await`, so this is a completion guard, not a mutual-exclusion proof.
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

// --- workers ----------------------------------------------------------------

task<void>
semaphore_worker(semaphore *sem, std::uint64_t *counter, std::atomic<int> *done) {
    co_await sem->acquire();
    ++(*counter);
    sem->release();
    done->fetch_add(1, std::memory_order_relaxed);
    co_return;
}

task<void>
mutex_worker(async_mutex *mtx, std::uint64_t *counter, std::atomic<int> *done) {
    co_await mtx->lock();
    ++(*counter);
    mtx->unlock();
    done->fetch_add(1, std::memory_order_relaxed);
    co_return;
}

task<void>
read_worker(async_rw_lock *rw, std::uint64_t const *shared, std::atomic<std::uint64_t> *sink, std::atomic<int> *done) {
    co_await rw->lock_read();
    sink->fetch_add(*shared, std::memory_order_relaxed); // observe the shared value under the read lock
    rw->unlock_read();
    done->fetch_add(1, std::memory_order_relaxed);
    co_return;
}

task<void>
write_worker(async_rw_lock *rw, std::uint64_t *counter, std::atomic<int> *done) {
    co_await rw->lock_write();
    ++(*counter);
    rw->unlock_write();
    done->fetch_add(1, std::memory_order_relaxed);
    co_return;
}

task<void>
latch_arriver(async_latch *latch, std::atomic<int> *done) {
    latch->count_down();
    done->fetch_add(1, std::memory_order_relaxed);
    co_return;
}

task<void>
latch_awaiter(async_latch *latch, std::atomic<bool> *released) {
    co_await latch->wait();
    released->store(true, std::memory_order_relaxed);
    co_return;
}

// --- benchmarks -------------------------------------------------------------

// Semaphore FAST PATH: permits == N ⇒ every worker acquires without suspending.
void
BM_Sync_Semaphore_Uncontended(benchmark::State &state) {
    const auto n = static_cast<int>(state.range(0));
    for (auto _ : state) {
        state.PauseTiming();
        reset_async_context();
        semaphore        sem{static_cast<std::size_t>(n)};
        std::uint64_t    counter = 0;
        std::atomic<int> done{0};
        state.ResumeTiming();

        for (int i = 0; i < n; ++i)
            coro_scheduler().spawn(semaphore_worker(&sem, &counter, &done));
        drain_until([&done, n] { return done.load(std::memory_order_relaxed) == n; });

        benchmark::DoNotOptimize(counter);
        state.PauseTiming();
        if (counter != static_cast<std::uint64_t>(n)) {
            state.SkipWithError("semaphore(uncontended): not every critical section ran (counter != N)");
            reset_async_context();
            return;
        }
        reset_async_context();
        state.ResumeTiming();
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * static_cast<std::uint64_t>(n)));
}

// Semaphore PARK/WAKE PATH: permits == 1 ⇒ N-1 workers park and are resumed on release.
void
BM_Sync_Semaphore_Contended(benchmark::State &state) {
    const auto n = static_cast<int>(state.range(0));
    for (auto _ : state) {
        state.PauseTiming();
        reset_async_context();
        semaphore        sem{1};
        std::uint64_t    counter = 0;
        std::atomic<int> done{0};
        state.ResumeTiming();

        for (int i = 0; i < n; ++i)
            coro_scheduler().spawn(semaphore_worker(&sem, &counter, &done));
        drain_until([&done, n] { return done.load(std::memory_order_relaxed) == n; });

        benchmark::DoNotOptimize(counter);
        state.PauseTiming();
        if (counter != static_cast<std::uint64_t>(n)) {
            state.SkipWithError("semaphore(contended): a parked worker was never resumed (counter != N)");
            reset_async_context();
            return;
        }
        reset_async_context();
        state.ResumeTiming();
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * static_cast<std::uint64_t>(n)));
}

// async_mutex: N workers serialize through the mutex (each after the first parks + wakes).
void
BM_Sync_AsyncMutex(benchmark::State &state) {
    const auto n = static_cast<int>(state.range(0));
    for (auto _ : state) {
        state.PauseTiming();
        reset_async_context();
        async_mutex      mtx;
        std::uint64_t    counter = 0;
        std::atomic<int> done{0};
        state.ResumeTiming();

        for (int i = 0; i < n; ++i)
            coro_scheduler().spawn(mutex_worker(&mtx, &counter, &done));
        drain_until([&done, n] { return done.load(std::memory_order_relaxed) == n; });

        benchmark::DoNotOptimize(counter);
        state.PauseTiming();
        if (counter != static_cast<std::uint64_t>(n)) {
            state.SkipWithError("async_mutex: a parked worker was never resumed (counter != N)");
            reset_async_context();
            return;
        }
        reset_async_context();
        state.ResumeTiming();
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * static_cast<std::uint64_t>(n)));
}

// async_rw_lock READ fast path: N shared readers acquire concurrently (no writer waiting ⇒ no park).
void
BM_Sync_RwLock_Read(benchmark::State &state) {
    const auto n = static_cast<int>(state.range(0));
    for (auto _ : state) {
        state.PauseTiming();
        reset_async_context();
        async_rw_lock              rw;
        const std::uint64_t        shared = 3u;
        std::atomic<std::uint64_t> sink{0};
        std::atomic<int>           done{0};
        state.ResumeTiming();

        for (int i = 0; i < n; ++i)
            coro_scheduler().spawn(read_worker(&rw, &shared, &sink, &done));
        drain_until([&done, n] { return done.load(std::memory_order_relaxed) == n; });

        benchmark::DoNotOptimize(sink.load(std::memory_order_relaxed));
        state.PauseTiming();
        if (sink.load(std::memory_order_relaxed) != shared * static_cast<std::uint64_t>(n)) {
            state.SkipWithError("async_rw_lock(read): not every reader observed the shared value");
            reset_async_context();
            return;
        }
        reset_async_context();
        state.ResumeTiming();
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * static_cast<std::uint64_t>(n)));
}

// async_rw_lock WRITE path: N exclusive writers serialize (park/wake handoff).
void
BM_Sync_RwLock_Write(benchmark::State &state) {
    const auto n = static_cast<int>(state.range(0));
    for (auto _ : state) {
        state.PauseTiming();
        reset_async_context();
        async_rw_lock    rw;
        std::uint64_t    counter = 0;
        std::atomic<int> done{0};
        state.ResumeTiming();

        for (int i = 0; i < n; ++i)
            coro_scheduler().spawn(write_worker(&rw, &counter, &done));
        drain_until([&done, n] { return done.load(std::memory_order_relaxed) == n; });

        benchmark::DoNotOptimize(counter);
        state.PauseTiming();
        if (counter != static_cast<std::uint64_t>(n)) {
            state.SkipWithError("async_rw_lock(write): a parked writer was never resumed (counter != N)");
            reset_async_context();
            return;
        }
        reset_async_context();
        state.ResumeTiming();
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * static_cast<std::uint64_t>(n)));
}

// async_latch: one awaiter parks on wait() until N arrivers count_down() to zero, then is released.
void
BM_Sync_Latch(benchmark::State &state) {
    const auto n = static_cast<int>(state.range(0));
    for (auto _ : state) {
        state.PauseTiming();
        reset_async_context();
        async_latch       latch{static_cast<std::size_t>(n)};
        std::atomic<int>  done{0};
        std::atomic<bool> released{false};
        state.ResumeTiming();

        coro_scheduler().spawn(latch_awaiter(&latch, &released));
        for (int i = 0; i < n; ++i)
            coro_scheduler().spawn(latch_arriver(&latch, &done));
        drain_until([&released] { return released.load(std::memory_order_relaxed); });

        benchmark::DoNotOptimize(done.load(std::memory_order_relaxed));
        state.PauseTiming();
        if (!released.load(std::memory_order_relaxed)) {
            state.SkipWithError("async_latch: awaiter never released after N count_downs");
            reset_async_context();
            return;
        }
        reset_async_context();
        state.ResumeTiming();
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * static_cast<std::uint64_t>(n)));
}

} // namespace

// N contending coroutines on the X axis.
BENCHMARK(BM_Sync_Semaphore_Uncontended)->Arg(1)->Arg(8)->Arg(64)->Arg(512)->ArgNames({"coros"})->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_Sync_Semaphore_Contended)->Arg(1)->Arg(8)->Arg(64)->Arg(512)->ArgNames({"coros"})->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_Sync_AsyncMutex)->Arg(1)->Arg(8)->Arg(64)->Arg(512)->ArgNames({"coros"})->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_Sync_RwLock_Read)->Arg(1)->Arg(8)->Arg(64)->Arg(512)->ArgNames({"coros"})->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_Sync_RwLock_Write)->Arg(1)->Arg(8)->Arg(64)->Arg(512)->ArgNames({"coros"})->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_Sync_Latch)->Arg(1)->Arg(8)->Arg(64)->Arg(512)->ArgNames({"arrivers"})->Unit(benchmark::kMicrosecond);

BENCHMARK_MAIN();
