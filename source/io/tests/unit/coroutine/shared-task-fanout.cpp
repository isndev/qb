/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/coroutine/shared-task-fanout.cpp
 * @brief `qb::io::async::shared_task<T>` — copyable, multi-consumer coroutine result fan-out.
 *
 * Covers `shared_task<T>` and its `void` specialisation plus `make_shared_task`: a single
 * underlying computation whose result (or exception) is delivered to ANY number of
 * coroutines, each awaiting the same copyable handle, with no recomputation. Exercises single
 * + multiple consumers, copyable handles, late joiners (cached-result path), exception
 * fan-out to every waiter, `valid()`/`is_ready()` state transitions, non-default-constructible
 * result types, and a scope-based 5-worker fan-out.
 *
 * Hardened over the original test-coroutine-shared-task.cpp:
 *   - every multi-consumer test now carries an explicit per-consumer `done`/`woken` guard
 *     asserted AFTER the pump, so a stalled scheduler can never read as a pass via the
 *     init-value-as-guard happenstance the original relied on;
 *   - the three overlapping valid/ready tests are consolidated into one;
 *   - ADDED: double-await of the SAME handle from the SAME consumer (the cached result must
 *     return twice), a LATE joiner that awaits AFTER the producing task already threw
 *     (exception CACHING, not just live propagation), and dropping every handle copy BEFORE
 *     the underlying task completes (the shared_ptr-backed frame must still run to completion
 *     and free cleanly);
 *   - every test gates on `qb::io::test::pump_until`; the four boilerplate fixtures collapse
 *     into one base and the file-local `main()` is removed (shared gtest_main).
 */

#include <atomic>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <qb/io/async/coroutine.h>

#include "../../shared/coroutine_test_support.h"

using namespace qb::io::async;
using namespace std::chrono_literals;
using qb::io::test::pump_until;

namespace {

class SharedTaskFanout : public ::testing::Test {
protected:
    void
    SetUp() override {
        qb::io::test::reset_async_context();
    }
    void
    TearDown() override {
        if (qb::io::async::listener::current.has_coro_scheduler()) {
            qb::io::async::run_for(5ms);
            qb::io::async::listener::current.reset_coro_scheduler();
        }
        qb::io::async::listener::current.clear();
    }
};

task<int>
compute_after(int value, std::chrono::milliseconds delay) {
    co_await sleep(delay);
    co_return value;
}

// A free coroutine taking its dependency by reference — NOT an immediately-invoked capturing
// lambda. An `[&](){...}()` closure is a temporary that dies at the end of the full expression,
// so a frame that suspends and resumes later would read the captured `&flag` out of the dead
// closure (stack-use-after-scope). A by-reference parameter instead binds to the caller's live
// object for the whole frame lifetime, which is the safe way to feed state into a detached frame.
task<void>
set_flag_after(std::atomic<bool> &flag, std::chrono::milliseconds delay) {
    co_await sleep(delay);
    flag.store(true);
}

task<int>
shared_task_thrower() {
    co_await sleep(5ms);
    throw std::runtime_error("boom");
    co_return 0;
}

task<void>
shared_task_void_failing() {
    co_await sleep(5ms);
    throw std::runtime_error("void-fail");
}

task<void>
wait_and_signal(std::atomic<bool> &flag, std::chrono::milliseconds delay) {
    co_await sleep(delay);
    flag.store(true);
}

} // namespace

// =============================================================================
// shared_task<int>
// =============================================================================

TEST_F(SharedTaskFanout, SingleConsumerReceivesResult) {
    std::atomic<int>  result{0};
    std::atomic<bool> done{false};

    auto sh = make_shared_task(compute_after(42, 10ms));
    coro_scheduler().spawn([sh, &result, &done]() mutable -> task<void> {
        result.store(co_await sh);
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "single consumer never resumed";
    EXPECT_EQ(result.load(), 42);
}

TEST_F(SharedTaskFanout, MultipleConsumersReceiveSameResult) {
    std::vector<int>  results(3, 0);
    std::atomic<int>  woken{0};

    auto sh = make_shared_task(compute_after(99, 10ms));
    for (int i = 0; i < 3; ++i) {
        coro_scheduler().spawn([sh, &results, &woken, i]() mutable -> task<void> {
            results[i] = co_await sh;
            woken.fetch_add(1);
        });
    }

    EXPECT_TRUE(pump_until([&] { return woken.load() == 3; })) << "not every consumer resumed";
    for (int r : results)
        EXPECT_EQ(r, 99) << "every consumer must read the single shared result";
}

TEST_F(SharedTaskFanout, LateJoinerGetsCachedResultImmediately) {
    std::atomic<int>  late_result{0};
    std::atomic<bool> done{false};

    auto sh = make_shared_task(compute_after(77, 5ms));
    coro_scheduler().spawn([sh, &late_result, &done]() mutable -> task<void> {
        co_await sleep(50ms); // join well after the task completed (~5ms)
        late_result.store(co_await sh);
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "late joiner never resumed";
    EXPECT_EQ(late_result.load(), 77);
}

TEST_F(SharedTaskFanout, CopiedHandlesShareOneComputation) {
    auto sh  = make_shared_task(compute_after(10, 5ms));
    auto sh2 = sh;
    auto sh3 = sh;

    std::atomic<int> r1{0}, r2{0}, r3{0};
    std::atomic<int> woken{0};

    coro_scheduler().spawn([sh, &r1, &woken]() mutable -> task<void> {
        r1.store(co_await sh);
        woken.fetch_add(1);
    });
    coro_scheduler().spawn([sh2, &r2, &woken]() mutable -> task<void> {
        r2.store(co_await sh2);
        woken.fetch_add(1);
    });
    coro_scheduler().spawn([sh3, &r3, &woken]() mutable -> task<void> {
        r3.store(co_await sh3);
        woken.fetch_add(1);
    });

    EXPECT_TRUE(pump_until([&] { return woken.load() == 3; })) << "not every handle copy resumed";
    EXPECT_EQ(r1.load(), 10);
    EXPECT_EQ(r2.load(), 10);
    EXPECT_EQ(r3.load(), 10);
}

TEST_F(SharedTaskFanout, DoubleAwaitOfSameHandleReturnsCachedResultTwice) {
    // ADDED: a single consumer awaits the SAME handle twice. The second await must return the
    // cached result without recomputation (is_done() fast path), not hang or recompute.
    std::atomic<int>  first{0}, second{0};
    std::atomic<bool> done{false};

    auto sh = make_shared_task(compute_after(55, 10ms));
    coro_scheduler().spawn([sh, &first, &second, &done]() mutable -> task<void> {
        first.store(co_await sh);
        second.store(co_await sh); // already ready -> cached
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "double-await consumer never resumed";
    EXPECT_EQ(first.load(), 55);
    EXPECT_EQ(second.load(), 55) << "the second await of the same handle must return the cached result";
}

TEST_F(SharedTaskFanout, ExceptionPropagatedToAllWaiters) {
    auto              sh = make_shared_task(shared_task_thrower());
    std::vector<bool> caught(3, false);
    std::atomic<int>  woken{0};

    for (int i = 0; i < 3; ++i) {
        coro_scheduler().spawn([sh, &caught, &woken, i]() mutable -> task<void> {
            try {
                co_await sh;
            } catch (const std::runtime_error &) {
                caught[i] = true;
            }
            woken.fetch_add(1);
        });
    }

    EXPECT_TRUE(pump_until([&] { return woken.load() == 3; })) << "not every waiter resumed";
    for (bool c : caught)
        EXPECT_TRUE(c) << "every waiter must observe the producing task's exception";
}

TEST_F(SharedTaskFanout, LateJoinerAfterThrowGetsCachedException) {
    // ADDED: a joiner that awaits AFTER the producing task already threw must observe the
    // CACHED exception (the failed-status fast path), not a silent success or a hang.
    std::atomic<bool> caught{false};
    std::atomic<bool> done{false};

    auto sh = make_shared_task(shared_task_thrower()); // throws at ~5ms
    coro_scheduler().spawn([sh, &caught, &done]() mutable -> task<void> {
        co_await sleep(50ms); // join long after the throw
        try {
            co_await sh;
        } catch (const std::runtime_error &e) {
            caught.store(std::string(e.what()) == "boom");
        }
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "late-after-throw joiner never resumed";
    EXPECT_TRUE(caught.load()) << "a late joiner must observe the cached exception";
}

TEST_F(SharedTaskFanout, AllHandleCopiesDroppedBeforeCompletionStillRunsCleanly) {
    // ADDED: drop EVERY shared_task handle copy before the underlying task completes. The
    // computation is owned by the spawned runner (shared_ptr-backed state), so it must still
    // run to completion and free cleanly with no leak — the oracle is live_frames returning
    // to baseline after the runner finishes.
    const long        baseline = detail::CoroutineFrameAllocator::live_frames;
    std::atomic<bool> ran{false};

    {
        auto sh = make_shared_task(set_flag_after(ran, 10ms));
        // sh goes out of scope here — the only remaining owner is the spawned runner.
    }

    EXPECT_TRUE(pump_until([&] { return ran.load(); })) << "the underlying task never ran after all handles were dropped";
    coro_scheduler().run_ready(); // drain the runner's final-suspend defer-destroy
    const long after = detail::CoroutineFrameAllocator::live_frames;
    EXPECT_EQ(after, baseline) << "dropping all handles before completion leaked " << (after - baseline) << " frame(s)";
}

TEST_F(SharedTaskFanout, ValidAndReadyStateTransitions) {
    // Consolidates the original ValidAndReadyState + DefaultConstructedIsInvalid +
    // SharedTaskCopyAndValidState into one transition check.
    shared_task<int> empty;
    EXPECT_FALSE(empty.valid());
    EXPECT_FALSE(empty.is_ready());

    auto sh   = make_shared_task(compute_after(1, 5ms));
    auto copy = sh;
    EXPECT_TRUE(sh.valid());
    EXPECT_TRUE(copy.valid());
    EXPECT_FALSE(sh.is_ready());
    EXPECT_FALSE(copy.is_ready());

    std::atomic<bool> done{false};
    coro_scheduler().spawn([sh, &done]() mutable -> task<void> {
        co_await sh;
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "valid/ready consumer never resumed";
    EXPECT_TRUE(sh.is_ready());
    EXPECT_TRUE(copy.is_ready()) << "the copy shares the same readiness state";
}

TEST_F(SharedTaskFanout, NonDefaultConstructibleResultType) {
    struct NoDefault {
        int value;
        explicit NoDefault(int v)
            : value(v) {}
        NoDefault() = delete;
    };

    std::atomic<int>  value{-1};
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        auto sh = make_shared_task([]() -> task<NoDefault> {
            co_await sleep(10ms);
            co_return NoDefault{42};
        }());
        auto result = co_await sh;
        value.store(result.value);
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "non-default-constructible consumer never resumed";
    EXPECT_EQ(value.load(), 42) << "shared_task must store a non-default-constructible T via std::optional";
}

// =============================================================================
// shared_task<void>
// =============================================================================

TEST_F(SharedTaskFanout, VoidMultipleWaitersAllWoken) {
    std::atomic<bool> flag{false};
    std::atomic<int>  woken{0};

    auto sh = make_shared_task(wait_and_signal(flag, 10ms));
    for (int i = 0; i < 4; ++i) {
        coro_scheduler().spawn([sh, &woken]() mutable -> task<void> {
            co_await sh;
            woken.fetch_add(1);
        });
    }

    EXPECT_TRUE(pump_until([&] { return woken.load() == 4; })) << "not every void waiter resumed";
    EXPECT_TRUE(flag.load());
}

TEST_F(SharedTaskFanout, VoidLateJoinerResumes) {
    std::atomic<bool> flag{false};
    std::atomic<bool> late{false};

    auto sh = make_shared_task(wait_and_signal(flag, 5ms));
    coro_scheduler().spawn([sh, &late]() mutable -> task<void> {
        co_await sleep(50ms);
        co_await sh; // already done
        late.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return late.load(); })) << "void late joiner never resumed";
    EXPECT_TRUE(flag.load());
}

TEST_F(SharedTaskFanout, VoidExceptionPropagated) {
    auto              sh = make_shared_task(shared_task_void_failing());
    std::atomic<bool> caught{false};
    std::atomic<bool> done{false};

    coro_scheduler().spawn([sh, &caught, &done]() mutable -> task<void> {
        try {
            co_await sh;
        } catch (const std::runtime_error &) {
            caught.store(true);
        }
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "void exception consumer never resumed";
    EXPECT_TRUE(caught.load());
}

// =============================================================================
// scope-based fan-out
// =============================================================================

TEST_F(SharedTaskFanout, ScopeFanOutMultipleWorkersAwaitSameData) {
    std::atomic<bool> done{false};
    std::vector<int>  outputs;

    coro_scheduler().spawn([&done, &outputs]() -> task<void> {
        auto data_handle = make_shared_task([]() -> task<int> {
            co_await sleep(10ms);
            co_return 100;
        }());
        coroutine_scope scope;

        for (int i = 0; i < 5; ++i) {
            scope.spawn([data_handle, &outputs, i]() mutable -> task<void> {
                int data = co_await data_handle;
                outputs.push_back(data + i);
            });
        }

        co_await scope.join_all();
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "scope fan-out coordinator never finished";
    ASSERT_EQ(outputs.size(), 5u);
    int sum = 0;
    for (int v : outputs)
        sum += v;
    EXPECT_EQ(sum, 510) << "(100+0)+(100+1)+(100+2)+(100+3)+(100+4)";
}
