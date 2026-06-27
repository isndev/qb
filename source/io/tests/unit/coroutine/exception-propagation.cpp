/**
 * @file unit/coroutine/exception-propagation.cpp
 * @brief The canonical exception-propagation file for the qb-io coroutine runtime.
 *
 * Exceptions in qb-io coroutines travel through the symmetric-transfer chain: a throw inside a coroutine
 * body is captured in its promise (`unhandled_exception()`), then re-thrown out of the awaiting
 * coroutine's `co_await` (`task<T>::await_resume`). This file is the single source of truth for that
 * behavior across every place a coroutine can suspend and resume: across an awaited `task<T>` (before and
 * after suspension), through deep call stacks, with catch-and-rethrow, out of a value-returning
 * `task<T>`, out of an `async_generator` pipeline (`co_await gen.next()` rethrows the generator's stored
 * exception), and out of a `channel` operation (`co_await ch.send()` on a closed channel throws
 * `channel_closed`). It also proves the scheduler stays usable after a throw and that parallel coroutines
 * fail independently. Everything runs in-process on the event loop (`init()` via `reset_async_context()`,
 * `spawn`, the de-flake pump `pump_until`) — NO sockets, NO daemon, NO TLS — a pure `unit` test.
 *
 * Consolidated here from the dissolved siblings so the duplicate copies can be retired:
 *   - test-coroutine-comprehensive.cpp::CoroutineExceptions::{ExceptionAfterMultipleSuspensions,
 *     ExceptionInSpawnedCoroutineHandled} fold in;
 *   - test-coroutine-basic.cpp::ExceptionHandling::{ExceptionPropagatesToAwaiter, ExceptionAfterSuspension}
 *     were exact duplicates of cases already here.
 * Added the genuinely-missing corners the duplicates only almost covered: exception out of an
 * async_generator, and exception during a co_await on a channel. Every fixed `run_for(Nms)` is replaced
 * by `pump_until`. No file-local main(): shared gtest_main.
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
 * @ingroup Tests
 */

#include <atomic>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>
#include <qb/io/async/coroutine.h>

#include "../../shared/coroutine_test_support.h"

using namespace qb::io::async;
using namespace std::chrono_literals;
using qb::io::test::pump_until;
using qb::io::test::reset_async_context;

// =============================================================================
// TEST FIXTURE
// =============================================================================

class CoroutineExceptionTests : public ::testing::Test {
protected:
    void
    SetUp() override {
        reset_async_context();
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

// Free-function coroutine used by the parallel-failure test (each instance owns its own frame).
static task<void>
parallel_throwing_task(std::atomic<int> *total_caught) {
    try {
        co_await sleep(5ms);
        throw std::runtime_error("parallel throw");
    } catch (...) {
        total_caught->fetch_add(1);
    }
    co_return;
}

// =============================================================================
// BASIC PROPAGATION ACROSS AN AWAITED task<T>
// =============================================================================

/**
 * @test Exception propagates from inner to outer coroutine, execution continues after catch
 * @brief A throw in an awaited inner task surfaces at the outer co_await; the outer keeps running.
 */
TEST_F(CoroutineExceptionTests, ExceptionPropagatesFromInnerCoroutine) {
    std::atomic<bool> caught{false};
    std::atomic<bool> after_catch{false};

    auto caught_ptr = &caught;
    auto after_ptr  = &after_catch;
    coro_scheduler().spawn([caught_ptr, after_ptr]() -> task<void> {
        try {
            auto inner_fn = []() -> task<void> {
                co_await sleep(1ms);
                throw std::runtime_error("test exception");
                co_return;
            };
            co_await inner_fn();
        } catch (const std::runtime_error &e) {
            if (std::string(e.what()) == "test exception")
                caught_ptr->store(true);
        }
        after_ptr->store(true);
        co_return;
    });

    EXPECT_TRUE(pump_until([&] { return after_catch.load(); })) << "outer coroutine never completed";
    EXPECT_TRUE(caught.load());
    EXPECT_TRUE(after_catch.load());
}

/**
 * @test Exception thrown before the inner suspension point
 * @brief A throw before the inner task's first co_await still propagates to the awaiter.
 */
TEST_F(CoroutineExceptionTests, ExceptionBeforeSuspension) {
    std::atomic<bool> caught{false};
    std::atomic<bool> done{false};

    auto caught_ptr = &caught;
    auto done_ptr   = &done;
    coro_scheduler().spawn([caught_ptr, done_ptr]() -> task<void> {
        try {
            auto inner_fn = []() -> task<void> {
                throw std::logic_error("immediate throw");
                co_await sleep(1ms); // never reached
                co_return;
            };
            co_await inner_fn();
        } catch (const std::logic_error &) {
            caught_ptr->store(true);
        }
        done_ptr->store(true);
        co_return;
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "coroutine never completed";
    EXPECT_TRUE(caught.load());
}

/**
 * @test Exception thrown after the inner suspension point
 * @brief A throw after the inner task resumes from its sleep propagates to the awaiter.
 */
TEST_F(CoroutineExceptionTests, ExceptionAfterSuspension) {
    std::atomic<bool> caught{false};
    std::atomic<bool> done{false};

    auto caught_ptr = &caught;
    auto done_ptr   = &done;
    coro_scheduler().spawn([caught_ptr, done_ptr]() -> task<void> {
        try {
            auto inner_fn = []() -> task<void> {
                co_await sleep(10ms);
                throw std::invalid_argument("after suspension");
                co_return;
            };
            co_await inner_fn();
        } catch (const std::invalid_argument &) {
            caught_ptr->store(true);
        }
        done_ptr->store(true);
        co_return;
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "coroutine never completed";
    EXPECT_TRUE(caught.load());
}

/**
 * @test Exception after MULTIPLE suspensions still propagates with all prior work observed
 * @brief Three sleeps run (steps == 3) before the throw; the awaiter catches it.
 *
 * Folded in from the dissolved test-coroutine-comprehensive.cpp::ExceptionAfterMultipleSuspensions.
 */
TEST_F(CoroutineExceptionTests, ExceptionAfterMultipleSuspensions) {
    std::atomic<int>  steps{0};
    std::atomic<bool> caught{false};
    std::atomic<bool> done{false};

    auto steps_ptr  = &steps;
    auto caught_ptr = &caught;
    auto done_ptr   = &done;
    coro_scheduler().spawn([steps_ptr, caught_ptr, done_ptr]() -> task<void> {
        auto unreliable = [steps_ptr]() -> task<int> {
            co_await sleep(10ms);
            steps_ptr->fetch_add(1);
            co_await sleep(10ms);
            steps_ptr->fetch_add(1);
            co_await sleep(10ms);
            steps_ptr->fetch_add(1);
            throw std::runtime_error("step 3 error");
            co_return 42;
        };
        try {
            (void) co_await unreliable();
        } catch (const std::runtime_error &) {
            caught_ptr->store(true);
        }
        done_ptr->store(true);
        co_return;
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "coroutine never completed";
    EXPECT_EQ(steps.load(), 3);
    EXPECT_TRUE(caught.load());
}

// =============================================================================
// EXCEPTION TYPES
// =============================================================================

/**
 * @test Distinct exception types each propagate with the correct dynamic type
 * @brief runtime_error, logic_error, and a custom exception are each caught by their own handler.
 */
TEST_F(CoroutineExceptionTests, DifferentExceptionTypes) {
    std::atomic<int>  caught_types{0};
    std::atomic<bool> done{false};

    auto types_ptr = &caught_types;
    auto done_ptr  = &done;
    coro_scheduler().spawn([types_ptr, done_ptr]() -> task<void> {
        try {
            auto fn1 = []() -> task<void> {
                throw std::runtime_error("runtime");
                co_return;
            };
            co_await fn1();
        } catch (const std::runtime_error &) {
            types_ptr->fetch_add(1);
        }

        try {
            auto fn2 = []() -> task<void> {
                throw std::logic_error("logic");
                co_return;
            };
            co_await fn2();
        } catch (const std::logic_error &) {
            types_ptr->fetch_add(10);
        }

        struct CustomException : std::exception {};
        try {
            auto fn3 = []() -> task<void> {
                throw CustomException{};
                co_return;
            };
            co_await fn3();
        } catch (const CustomException &) {
            types_ptr->fetch_add(100);
        }

        done_ptr->store(true);
        co_return;
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "coroutine never completed";
    EXPECT_EQ(caught_types.load(), 111); // 1 + 10 + 100
}

// =============================================================================
// CHAINING / RETHROW
// =============================================================================

/**
 * @test Exception propagates through multiple coroutine layers
 * @brief level3 throws; level2 transparently forwards; level1 catches with the original message intact.
 */
TEST_F(CoroutineExceptionTests, ExceptionThroughMultipleLayers) {
    std::atomic<int>  depth_caught{0};
    std::atomic<bool> done{false};

    auto depth_ptr = &depth_caught;
    auto done_ptr  = &done;
    coro_scheduler().spawn([depth_ptr, done_ptr]() -> task<void> {
        auto level3_fn = []() -> task<void> {
            co_await sleep(1ms);
            throw std::runtime_error("from level 3");
            co_return;
        };
        auto level2_fn = [level3_fn]() -> task<void> {
            co_await level3_fn(); // propagates through, no catch
            co_return;
        };
        try {
            co_await level2_fn();
        } catch (const std::runtime_error &e) {
            if (std::string(e.what()) == "from level 3")
                depth_ptr->store(3);
        }
        done_ptr->store(true);
        co_return;
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "coroutine never completed";
    EXPECT_EQ(depth_caught.load(), 3);
}

/**
 * @test Catch-and-rethrow across coroutine boundaries
 * @brief An intermediate coroutine catches, counts, and re-throws; the outer coroutine catches the rethrow.
 */
TEST_F(CoroutineExceptionTests, RethrowInCoroutine) {
    std::atomic<int>  catch_count{0};
    std::atomic<bool> done{false};

    auto count_ptr = &catch_count;
    auto done_ptr  = &done;
    coro_scheduler().spawn([count_ptr, done_ptr]() -> task<void> {
        auto thrower_fn = []() -> task<void> {
            throw std::runtime_error("original");
            co_return;
        };
        auto rethrow_fn = [thrower_fn, count_ptr]() -> task<void> {
            try {
                co_await thrower_fn();
            } catch (...) {
                count_ptr->fetch_add(1);
                throw; // rethrow
            }
            co_return;
        };
        try {
            co_await rethrow_fn();
        } catch (const std::runtime_error &) {
            count_ptr->fetch_add(10);
        }
        done_ptr->store(true);
        co_return;
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "coroutine never completed";
    EXPECT_EQ(catch_count.load(), 11); // inner(1) + outer(10)
}

// =============================================================================
// VALUE-RETURNING COROUTINES
// =============================================================================

/**
 * @test Exception out of a value-returning task<T> leaves no garbage value
 * @brief A throwing task<int> never delivers a value; the awaiter substitutes a fallback after catching.
 */
TEST_F(CoroutineExceptionTests, ExceptionFromValueReturningCoroutine) {
    std::atomic<bool> caught{false};
    std::atomic<int>  result{0};
    std::atomic<bool> done{false};

    auto caught_ptr = &caught;
    auto result_ptr = &result;
    auto done_ptr   = &done;
    coro_scheduler().spawn([caught_ptr, result_ptr, done_ptr]() -> task<void> {
        auto thrower_fn = []() -> task<int> {
            co_await sleep(1ms);
            throw std::runtime_error("no value");
            co_return 42; // never reached
        };
        int value = 0;
        try {
            value = co_await thrower_fn();
        } catch (const std::runtime_error &) {
            caught_ptr->store(true);
            value = -1; // fallback
        }
        result_ptr->store(value);
        done_ptr->store(true);
        co_return;
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "coroutine never completed";
    EXPECT_TRUE(caught.load());
    EXPECT_EQ(result.load(), -1);
}

// =============================================================================
// EXCEPTIONS THROUGH async_generator AND channel
// =============================================================================

namespace {
// MSVC's optimizer back-end raises a C1001 internal compiler error when a coroutine consumes an
// async_generator via `co_await gen.next()` inside a try/catch. Disable optimization for just
// these two helper coroutines on MSVC to dodge the compiler bug — behaviour is identical and
// these are unit-test helpers, so the lost optimization is irrelevant. Extracted to named
// coroutines (not nested lambdas) so the pragma can scope them cleanly.
#if defined(_MSC_VER)
#pragma optimize("", off)
#endif
async_generator<int>
make_failing_gen() {
    co_yield 1;
    co_yield 2;
    throw std::runtime_error("generator failed");
    co_yield 3; // never reached
}
task<void>
consume_failing_gen(std::atomic<int> *values, std::atomic<bool> *caught, std::atomic<bool> *done) {
    auto gen = make_failing_gen();
    try {
        for (;;) {
            auto v = co_await gen.next();
            if (!v)
                break;
            (void) *v;
            values->fetch_add(1);
        }
    } catch (const std::runtime_error &e) {
        if (std::string(e.what()) == "generator failed")
            caught->store(true);
    }
    done->store(true);
    co_return;
}
#if defined(_MSC_VER)
#pragma optimize("", on)
#endif
} // namespace

/**
 * @test Exception thrown inside an async_generator surfaces at co_await gen.next()
 * @brief The generator yields a few values, then throws; the consumer receives the prior values and then
 *        catches the exception out of `co_await gen.next()` (next_awaiter::await_resume rethrows the
 *        promise's stored exception).
 */
TEST_F(CoroutineExceptionTests, ExceptionFromAsyncGeneratorPropagates) {
    std::atomic<int>  values_seen{0};
    std::atomic<bool> caught{false};
    std::atomic<bool> done{false};

    auto values_ptr = &values_seen;
    auto caught_ptr = &caught;
    auto done_ptr   = &done;
    coro_scheduler().spawn([values_ptr, caught_ptr, done_ptr]() -> task<void> {
        co_await consume_failing_gen(values_ptr, caught_ptr, done_ptr);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "generator consumer never completed";
    EXPECT_EQ(values_seen.load(), 2) << "consumer should have received the two values before the throw";
    EXPECT_TRUE(caught.load()) << "async_generator exception did not surface at co_await gen.next()";
}

/**
 * @test Sending on a closed channel throws channel_closed at the co_await
 * @brief A closed channel rejects every subsequent send: `co_await ch.send(...)` throws `channel_closed`
 *        (a std::runtime_error) synchronously from await_resume. recv() on a closed channel, by contrast,
 *        returns nullopt — asserted here to pin both halves of the contract.
 */
TEST_F(CoroutineExceptionTests, ExceptionFromClosedChannelSend) {
    std::atomic<bool> send_threw{false};
    std::atomic<bool> recv_empty{false};
    std::atomic<bool> done{false};

    auto send_ptr = &send_threw;
    auto recv_ptr = &recv_empty;
    auto done_ptr = &done;
    coro_scheduler().spawn([send_ptr, recv_ptr, done_ptr]() -> task<void> {
        channel<int> ch(4);
        ch.close();

        // recv on a closed (empty) channel yields nullopt, not an exception.
        auto v = co_await ch.recv();
        if (!v.has_value())
            recv_ptr->store(true);

        // send on a closed channel throws channel_closed.
        try {
            co_await ch.send(99);
        } catch (const channel_closed &) {
            send_ptr->store(true);
        }
        done_ptr->store(true);
        co_return;
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "channel coroutine never completed";
    EXPECT_TRUE(recv_empty.load()) << "recv on a closed channel should yield nullopt";
    EXPECT_TRUE(send_threw.load()) << "send on a closed channel should throw channel_closed";
}

// =============================================================================
// SCHEDULER STABILITY / INDEPENDENCE
// =============================================================================

/**
 * @test An unhandled exception in one spawned coroutine does not stop another
 * @brief A throwing spawned root and a normal spawned coroutine: the normal one still completes.
 *
 * Folded in from the dissolved test-coroutine-comprehensive.cpp::ExceptionInSpawnedCoroutineHandled.
 */
TEST_F(CoroutineExceptionTests, UnhandledExceptionDoesNotStopOtherCoroutines) {
    std::atomic<bool> other_completed{false};

    auto throwing = []() -> task<void> {
        co_await sleep(10ms);
        throw std::runtime_error("spawned error");
        co_return;
    };
    auto other_ptr = &other_completed;
    auto normal    = [other_ptr]() -> task<void> {
        co_await sleep(20ms);
        other_ptr->store(true);
    };

    coro_scheduler().spawn(throwing());
    coro_scheduler().spawn(normal());

    EXPECT_TRUE(pump_until([&] { return other_completed.load(); }))
        << "a sibling throwing coroutine prevented the normal one from completing";
    EXPECT_TRUE(other_completed.load());
}

/**
 * @test Scheduler remains usable and drains to zero after a coroutine throws
 * @brief A catcher coroutine (catches an inner throw) and a normal coroutine both complete; the scheduler
 *        bookkeeping returns to empty.
 */
TEST_F(CoroutineExceptionTests, SchedulerStableAfterException) {
    std::atomic<int> completed_count{0};

    auto count_ptr = &completed_count;
    coro_scheduler().spawn([count_ptr]() -> task<void> {
        auto thrower_fn = []() -> task<void> {
            throw std::runtime_error("error");
            co_return;
        };
        try {
            co_await thrower_fn();
        } catch (...) {
            count_ptr->fetch_add(1);
        }
        co_return;
    });
    coro_scheduler().spawn([count_ptr]() -> task<void> {
        co_await sleep(10ms);
        count_ptr->fetch_add(10);
        co_return;
    });

    EXPECT_TRUE(pump_until([&] { return completed_count.load() == 11; })) << "both coroutines never completed";
    EXPECT_EQ(completed_count.load(), 11); // catcher(1) + normal(10)
    EXPECT_EQ(coro_scheduler().active_count(), 0u);
}

/**
 * @test Multiple coroutines throw and catch independently in parallel
 * @brief Five spawned coroutines each throw-and-catch their own exception; all five count.
 */
TEST_F(CoroutineExceptionTests, MultipleExceptionsInParallel) {
    std::atomic<int> total_caught{0};

    auto total_ptr = &total_caught;
    for (int i = 0; i < 5; ++i) {
        coro_scheduler().spawn(parallel_throwing_task(total_ptr));
    }

    EXPECT_TRUE(pump_until([&] { return total_caught.load() == 5; })) << "not all parallel throws were caught";
    EXPECT_EQ(total_caught.load(), 5);
}
