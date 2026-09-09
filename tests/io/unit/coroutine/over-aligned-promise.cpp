/**
 * @file unit/coroutine/over-aligned-promise.cpp
 * @brief A `task<T>` whose promise is OVER-ALIGNED (a `T` with `alignas(64)`, the shape of every
 *        `task<qb::Event>`) round-trips its value, on every toolchain (Huly QB-200).
 *
 * The standard `coroutine_handle<P>::from_promise()` / `promise()` pair is what `get_return_object`
 * and every promise access rely on. With clang-cl on the MSVC STL both are computed with an
 * alignment of ZERO, which is right for a promise aligned 16 or less and lands 48 bytes into the
 * frame for one aligned 64: every `qb::ask` crashed there on its first `co_await`. qb's
 * `detail::handle_from_promise` / `promise_of` hand the compiler the real alignment, and this file
 * is what keeps them honest: the promise's alignment is ASSERTED to be over 16 first (a test that
 * ran on an ordinarily aligned promise would prove nothing), then the handle built from the
 * promise is checked to lead back to the same promise, and a coroutine returning the over-aligned
 * value is awaited and read through the same path `qb::ask` takes.
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
#include <chrono>
#include <cstdint>
#include <cstdlib>

#include <gtest/gtest.h>
#include <qb/io/async/coroutine.h>
#include <qb/io/async/coroutine/promise_access.h>

#include "../../shared/coroutine_test_support.h"

using namespace qb::io::async;
using namespace std::chrono_literals;
using qb::io::test::pump_until;
using qb::io::test::reset_async_context;

namespace over_aligned_promise_test {

/// The shape of a `qb::Event`: a value whose alignment is a cache line, so the promise that holds
/// one is over-aligned -- past what any `operator new` guarantees and past what the MSVC STL's
/// `from_promise` assumes.
struct alignas(64) Wide {
    std::uint64_t words[8]{};
};
static_assert(alignof(Wide) == 64);

using wide_task    = task<Wide>;
using wide_promise = wide_task::promise_type;
// The anti-vacuity floor: the promise IS over-aligned, or this file would be testing the ordinary
// path and a regression on clang-cl would pass it.
static_assert(alignof(wide_promise) >= 64, "task<Wide>'s promise must be over-aligned for this test to mean anything");

wide_task
produce(std::uint64_t const seed) {
    Wide w;
    for (std::uint64_t i = 0; i < 8; ++i)
        w.words[i] = seed * 8 + i;
    co_return w;
}

/// A coroutine that hands its own promise out, so the conversion can be checked from the outside.
struct probe_task {
    struct promise_type {
        void *self_address = nullptr; ///< `this` of the promise, as the coroutine sees it
        probe_task
        get_return_object() {
            self_address = this;
            return probe_task{detail::handle_from_promise(*this)};
        }
        std::suspend_always
        initial_suspend() noexcept {
            return {};
        }
        std::suspend_always
        final_suspend() noexcept {
            return {};
        }
        void
        return_void() noexcept {}
        void
        unhandled_exception() noexcept {
            std::abort();
        }
        alignas(64) std::uint64_t pad[8]{}; ///< what makes THIS promise over-aligned
    };
    std::coroutine_handle<promise_type> handle;
    ~probe_task() {
        if (handle)
            handle.destroy();
    }
};
static_assert(alignof(probe_task::promise_type) >= 64);

probe_task
probe() {
    co_return;
}

class OverAlignedPromise : public ::testing::Test {
protected:
    void
    SetUp() override {
        reset_async_context();
    }
    void
    TearDown() override {
        if (listener::current.has_coro_scheduler()) {
            run_for(5ms);
            listener::current.reset_coro_scheduler();
        }
        listener::current.clear();
    }
};

} // namespace over_aligned_promise_test

using namespace over_aligned_promise_test;

/**
 * @test The handle built from an over-aligned promise leads back to that promise, and resumes.
 */
TEST_F(OverAlignedPromise, HandleFromPromiseRoundTripsAndResumes) {
    probe_task t = probe();
    ASSERT_TRUE(t.handle);
    auto &p = detail::promise_of(t.handle);
    EXPECT_EQ(static_cast<void *>(&p), p.self_address) << "promise_of(handle_from_promise(p)) is not p: the handle "
                                                          "was computed with the wrong promise offset";
    // NOT asserted: the promise's address being 64-aligned. C++20 makes no such promise for a
    // coroutine frame (P2014 is not in it): clang realigns the frame, GCC and MSVC do not (16
    // mod 64 measured on g++-14), and the code above is correct on all three because the
    // conversion, not the address, is what it fixes.
    EXPECT_FALSE(t.handle.done());
    t.handle.resume(); // runs `co_return` to final_suspend: a wrong handle would jump into garbage here
    EXPECT_TRUE(t.handle.done());
}

/**
 * @test A `task<Wide>` -- the shape of every `task<qb::Event>` -- is awaited and its value read
 *       through the promise, from a spawned coroutine driven by the scheduler.
 */
TEST_F(OverAlignedPromise, TaskOverAWideValueRoundTrips) {
    std::atomic<bool>          done{false};
    std::atomic<std::uint64_t> sum{0};
    auto                       done_ptr = &done;
    auto                       sum_ptr  = &sum;
    coro_scheduler().spawn([done_ptr, sum_ptr]() -> task<void> {
        std::uint64_t s = 0;
        for (std::uint64_t seed = 1; seed <= 16; ++seed) {
            const Wide w = co_await produce(seed);
            for (auto const x : w.words)
                s += x;
        }
        sum_ptr->store(s);
        done_ptr->store(true);
        co_return;
    });
    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "the spawned coroutine never completed";
    std::uint64_t expect = 0;
    for (std::uint64_t seed = 1; seed <= 16; ++seed)
        for (std::uint64_t i = 0; i < 8; ++i)
            expect += seed * 8 + i;
    EXPECT_EQ(sum.load(), expect);
}
