/**
 * @file unit/coroutine/over-aligned-promise.cpp
 * @brief A coroutine whose frame holds an OVER-ALIGNED object (an `alignas(64)` value, the shape of every
 *        `qb::Event`) works on every toolchain: its promise round-trips through the handle (Huly QB-200), and
 *        the promise and the frame's locals sit at their alignment -- for `task<T>`, `generator<T>` and
 *        `async_generator<T>` alike (Huly QB-1246).
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
 * The second half is the frame's ADDRESS. C++20 does not align a coroutine frame for what it holds (P2014 is not
 * in it): the frame is placed at whatever the promise type's `operator new(size)` returns, or `::operator new(size)`
 * when it has none -- 16-aligned. GCC, clang and clang-cl lay the frame out from that address as if it were aligned
 * for the frame's widest member, so an `alignas(64)` promise or local lands at 16, 32 or 48 mod 64: undefined
 * behaviour, and a SIGSEGV wherever the compiler emits an aligned vector move (any AVX target -- this file's own
 * probe died so, in its promise's constructor, under the g++ 14 of an x86-64-v3 Ubuntu). MSVC 19.51 realigns the
 * frame itself. qb's coroutine types therefore take cache-line-aligned blocks: `task<T>` and `async_generator<T>` from
 * `detail::CoroutineFrameAllocator`, `generator<T>` from the aligned `::operator new`. The cases below create many
 * frames at once, a small block between each two, so that an allocator that lays consecutive blocks out in order (glibc's
 * and the Windows heap, measured) cannot hand all of them out 64-aligned by luck -- a size-class allocator may -- and
 * read every address through a volatile, which the optimiser cannot fold to "aligned" the way it may fold the residue
 * of a typed pointer.
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
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

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

// The same floor for the generators: a generator of a Wide holds one in its promise.
static_assert(alignof(generator<Wide>::promise_type) >= 64);
static_assert(alignof(async_generator<Wide>::promise_type) >= 64);
// And its opposite for the frame-local cases: there the promise is ordinary, so what is checked is the frame
// itself, not the promise.
static_assert(alignof(generator<std::uintptr_t>::promise_type) < 64);
static_assert(alignof(async_generator<std::uintptr_t>::promise_type) < 64);

/// How many frames each case keeps alive at once.
constexpr std::size_t kFrames = 64;

/// An address as a number the optimiser cannot reason about. An `alignas(64)` object at an address that is not a
/// multiple of 64 is undefined behaviour, so the residue of a TYPED pointer may be folded to 0 (clang-cl does it at
/// /O2); a value read back from a volatile is opaque.
std::uintptr_t
opaque_address(void const *p) noexcept {
    volatile std::uintptr_t laundered = reinterpret_cast<std::uintptr_t>(p);
    return laundered;
}

/// What a run of frames looked like: how many addresses were read, how many were misaligned, the first residue.
struct alignment_census {
    std::size_t    seen       = 0;
    std::size_t    misaligned = 0;
    std::uintptr_t first_off  = 0; ///< the first misaligned address, modulo the alignment

    void
    record(std::uintptr_t const address, std::size_t const alignment) noexcept {
        ++seen;
        if (address % alignment != 0 && misaligned++ == 0)
            first_off = address % alignment;
    }
};

::testing::AssertionResult
all_aligned(alignment_census const &c, std::size_t const expected, std::size_t const alignment) {
    if (c.seen != expected)
        return ::testing::AssertionFailure() << c.seen << " addresses read, " << expected << " expected";
    if (c.misaligned != 0)
        return ::testing::AssertionFailure() << c.misaligned << " of " << c.seen << " addresses are not " << alignment
                                             << "-aligned (the first at " << c.first_off << " mod " << alignment
                                             << "): the frame was not allocated at its alignment";
    return ::testing::AssertionSuccess();
}

/// One small heap block between two frames: an allocator that lays blocks out in order then hands consecutive frames out
/// at different residues modulo 64, so it cannot pass a case by placing every frame on a 64-byte boundary by accident.
using spacers = std::vector<std::unique_ptr<std::uint64_t>>;

/// The address of the promise of the coroutine that awaits it, through `detail::promise_of`; never suspends.
struct promise_address {
    std::uintptr_t *out;
    [[nodiscard]] bool
    await_ready() const noexcept {
        return false;
    }
    template <typename P>
    bool
    await_suspend(std::coroutine_handle<P> h) const noexcept {
        *out = opaque_address(&detail::promise_of(h));
        return false; // resume at once
    }
    void
    await_resume() const noexcept {}
};

wide_task
produce(std::uint64_t const seed, std::uintptr_t *promise_at) {
    if (promise_at)
        co_await promise_address{promise_at};
    Wide w;
    for (std::uint64_t i = 0; i < 8; ++i)
        w.words[i] = seed * 8 + i;
    co_return w;
}

/// A coroutine that hands its own promise out, so the conversion can be checked from the outside.
/// Its frame comes from the allocator `task<T>`'s does, `detail::CoroutineFrameAllocator`: without it, the frame is
/// `::operator new(size)`'s 16-aligned block on GCC and clang and the `alignas(64)` promise below is misaligned --
/// what g++ 14 on an x86-64-v3 Ubuntu met as a `vmovdqa` fault in this promise's constructor.
struct probe_task {
    struct promise_type {
        QB_CORO_PROMISE_POOLED_NEW_DELETE()

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

    explicit probe_task(std::coroutine_handle<promise_type> h) noexcept
        : handle(h) {}
    // Move-only: the frame is destroyed once, by the probe that holds it last.
    probe_task(probe_task &&other) noexcept
        : handle(std::exchange(other.handle, {})) {}
    probe_task(probe_task const &)            = delete;
    probe_task &operator=(probe_task const &) = delete;
    probe_task &operator=(probe_task &&)      = delete;
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

/// A generator whose promise holds a Wide: its one value is read through the iterator, which returns a reference to
/// the value the promise holds (`detail::promise_of(handle).current_value`).
generator<Wide>
wide_values(std::uint64_t const seed) {
    Wide w;
    w.words[0] = seed;
    co_yield w;
}

/// A generator of an ordinary value whose BODY holds a Wide across two `co_yield`s; it yields the local's address.
generator<std::uintptr_t>
wide_local_addresses(std::uint64_t const seed) {
    Wide local;
    local.words[0] = seed;
    co_yield opaque_address(&local);
    local.words[1] = seed;
    co_yield opaque_address(&local);
}

/// The async generator's promise, by the awaiter above, then one Wide.
async_generator<Wide>
wide_values_async(std::uint64_t const seed, std::uintptr_t *promise_at) {
    co_await promise_address{promise_at};
    Wide w;
    w.words[0] = seed;
    co_yield w;
}

/// The async generator whose body holds a Wide across two `co_yield`s.
async_generator<std::uintptr_t>
wide_local_addresses_async(std::uint64_t const seed) {
    Wide local;
    local.words[0] = seed;
    co_yield opaque_address(&local);
    local.words[1] = seed;
    co_yield opaque_address(&local);
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
 * @test The handle built from an over-aligned promise leads back to that promise, and resumes; the promise sits at
 *       its alignment.
 */
TEST_F(OverAlignedPromise, HandleFromPromiseRoundTripsAndResumes) {
    std::vector<probe_task> probes;
    spacers                 between;
    probes.reserve(kFrames);
    between.reserve(kFrames);
    for (std::size_t i = 0; i < kFrames; ++i) {
        probes.push_back(probe());
        between.push_back(std::make_unique<std::uint64_t>(i));
    }
    alignment_census census;
    for (auto &t : probes) {
        ASSERT_TRUE(t.handle);
        auto &p = detail::promise_of(t.handle);
        EXPECT_EQ(static_cast<void *>(&p), p.self_address) << "promise_of(handle_from_promise(p)) is not p: the handle "
                                                              "was computed with the wrong promise offset";
        // A contract since the promise's frame comes from the cache-line-aligned pool: without it, GCC and clang
        // place this promise at 16 mod 64 (C++20 does not align a coroutine frame, P2014 is not in it).
        census.record(opaque_address(&p), alignof(probe_task::promise_type));
        EXPECT_FALSE(t.handle.done());
        t.handle.resume(); // runs `co_return` to final_suspend: a wrong handle would jump into garbage here
        EXPECT_TRUE(t.handle.done());
    }
    EXPECT_TRUE(all_aligned(census, kFrames, alignof(probe_task::promise_type)));
}

/**
 * @test A `task<Wide>` -- the shape of every `task<qb::Event>` -- is awaited and its value read
 *       through the promise, from a spawned coroutine driven by the scheduler; every promise is 64-aligned.
 */
TEST_F(OverAlignedPromise, TaskOverAWideValueRoundTrips) {
    std::atomic<bool>           done{false};
    std::atomic<std::uint64_t>  sum{0};
    std::vector<std::uintptr_t> promises(kFrames, 0);
    auto                        done_ptr     = &done;
    auto                        sum_ptr      = &sum;
    auto                        promises_ptr = &promises;
    coro_scheduler().spawn([done_ptr, sum_ptr, promises_ptr]() -> task<void> {
        // Every task created before the first is awaited, so all their frames are alive at once.
        std::vector<wide_task> tasks;
        spacers                between;
        tasks.reserve(kFrames);
        between.reserve(kFrames);
        for (std::uint64_t seed = 0; seed < kFrames; ++seed) {
            tasks.push_back(produce(seed + 1, &(*promises_ptr)[seed]));
            between.push_back(std::make_unique<std::uint64_t>(seed));
        }
        std::uint64_t s = 0;
        for (auto &t : tasks) {
            const Wide w = co_await std::move(t);
            for (auto const x : w.words)
                s += x;
        }
        sum_ptr->store(s);
        done_ptr->store(true);
        co_return;
    });
    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "the spawned coroutine never completed";
    std::uint64_t expect = 0;
    for (std::uint64_t seed = 1; seed <= kFrames; ++seed)
        for (std::uint64_t i = 0; i < 8; ++i)
            expect += seed * 8 + i;
    EXPECT_EQ(sum.load(), expect);
    alignment_census census;
    for (auto const a : promises)
        census.record(a, alignof(wide_promise));
    EXPECT_TRUE(all_aligned(census, kFrames, alignof(wide_promise)));
}

/**
 * @test A `generator<Wide>` -- a generator of any `qb::Event` -- has its promise, and so the value it yields, at the
 *       promise's alignment, frame after frame.
 */
TEST_F(OverAlignedPromise, GeneratorOfAWideValueHasAnAlignedPromise) {
    std::vector<generator<Wide>> gens;
    spacers                      between;
    gens.reserve(kFrames);
    between.reserve(kFrames);
    for (std::uint64_t i = 0; i < kFrames; ++i) {
        gens.push_back(wide_values(i));
        between.push_back(std::make_unique<std::uint64_t>(i));
    }
    alignment_census census;
    for (std::uint64_t i = 0; i < kFrames; ++i) {
        auto it = gens[i].begin();
        ASSERT_NE(it, gens[i].end());
        Wide const &held = *it; // a reference into the promise, not a copy
        EXPECT_EQ(held.words[0], i);
        census.record(opaque_address(&held), alignof(generator<Wide>::promise_type));
    }
    EXPECT_TRUE(all_aligned(census, kFrames, alignof(generator<Wide>::promise_type)));
}

/**
 * @test A generator of an ordinary value whose body keeps an `alignas(64)` local across `co_yield` has that local at
 *       its alignment, at both suspensions, frame after frame.
 */
TEST_F(OverAlignedPromise, GeneratorBodyKeepsAWideLocalAligned) {
    std::vector<generator<std::uintptr_t>> gens;
    spacers                                between;
    gens.reserve(kFrames);
    between.reserve(kFrames);
    for (std::uint64_t i = 0; i < kFrames; ++i) {
        gens.push_back(wide_local_addresses(i));
        between.push_back(std::make_unique<std::uint64_t>(i));
    }
    alignment_census census;
    for (auto &g : gens) {
        // Iterated, not next(): begin() always runs the body to its first co_yield, so what is read here is what the
        // body yielded whatever the promise was built from (generator-sync.cpp holds that contract on its own).
        std::vector<std::uintptr_t> yielded;
        for (auto const address : g)
            yielded.push_back(address);
        ASSERT_EQ(yielded.size(), 2u);
        EXPECT_EQ(yielded[0], yielded[1]) << "one frame yielded two different addresses for one local";
        census.record(yielded[0], alignof(Wide));
        census.record(yielded[1], alignof(Wide));
    }
    EXPECT_TRUE(all_aligned(census, 2 * kFrames, alignof(Wide)));
}

/**
 * @test An `async_generator<Wide>` has its promise at the promise's alignment, frame after frame, and yields its value.
 */
TEST_F(OverAlignedPromise, AsyncGeneratorOfAWideValueHasAnAlignedPromise) {
    std::atomic<bool>           done{false};
    std::atomic<std::size_t>    values{0};
    std::vector<std::uintptr_t> promises(kFrames, 0);
    auto                        done_ptr     = &done;
    auto                        values_ptr   = &values;
    auto                        promises_ptr = &promises;
    coro_scheduler().spawn([done_ptr, values_ptr, promises_ptr]() -> task<void> {
        std::vector<async_generator<Wide>> gens;
        spacers                            between;
        gens.reserve(kFrames);
        between.reserve(kFrames);
        for (std::uint64_t i = 0; i < kFrames; ++i) {
            gens.push_back(wide_values_async(i, &(*promises_ptr)[i]));
            between.push_back(std::make_unique<std::uint64_t>(i));
        }
        std::size_t n = 0;
        for (std::uint64_t i = 0; i < kFrames; ++i) {
            std::optional<Wide> const v = co_await gens[i].next();
            if (v && v->words[0] == i)
                ++n;
        }
        values_ptr->store(n);
        done_ptr->store(true);
        co_return;
    });
    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "the spawned coroutine never completed";
    EXPECT_EQ(values.load(), kFrames);
    alignment_census census;
    for (auto const a : promises)
        census.record(a, alignof(async_generator<Wide>::promise_type));
    EXPECT_TRUE(all_aligned(census, kFrames, alignof(async_generator<Wide>::promise_type)));
}

/**
 * @test An async generator whose body keeps an `alignas(64)` local across `co_yield` has that local at its alignment,
 *       at both suspensions, frame after frame.
 */
TEST_F(OverAlignedPromise, AsyncGeneratorBodyKeepsAWideLocalAligned) {
    std::atomic<bool>           done{false};
    std::atomic<std::size_t>    moved{0};
    std::vector<std::uintptr_t> locals;
    auto                        done_ptr   = &done;
    auto                        moved_ptr  = &moved;
    auto                        locals_ptr = &locals;
    coro_scheduler().spawn([done_ptr, moved_ptr, locals_ptr]() -> task<void> {
        std::vector<async_generator<std::uintptr_t>> gens;
        spacers                                      between;
        gens.reserve(kFrames);
        between.reserve(kFrames);
        for (std::uint64_t i = 0; i < kFrames; ++i) {
            gens.push_back(wide_local_addresses_async(i));
            between.push_back(std::make_unique<std::uint64_t>(i));
        }
        std::size_t m = 0;
        for (auto &g : gens) {
            std::optional<std::uintptr_t> const first  = co_await g.next();
            std::optional<std::uintptr_t> const second = co_await g.next();
            if (!first || !second || *first != *second)
                ++m;
            if (first)
                locals_ptr->push_back(*first);
            if (second)
                locals_ptr->push_back(*second);
        }
        moved_ptr->store(m);
        done_ptr->store(true);
        co_return;
    });
    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "the spawned coroutine never completed";
    EXPECT_EQ(moved.load(), 0u) << "a frame yielded two different addresses for one local, or fewer than two values";
    alignment_census census;
    for (auto const a : locals)
        census.record(a, alignof(Wide));
    EXPECT_TRUE(all_aligned(census, 2 * kFrames, alignof(Wide)));
}
