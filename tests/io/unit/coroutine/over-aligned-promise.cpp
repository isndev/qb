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
 * `detail::CoroutineFrameAllocator`, `generator<T>` an `::operator new` block it realigns (to its promise's alignment
 * when that is past the cache line). The cases below create many
 * frames at once, a small block between each two, so that an allocator that lays consecutive blocks out in order (glibc's
 * and the Windows heap, measured) cannot hand all of them out 64-aligned by luck -- a size-class allocator may -- and
 * read every address through a volatile, which the optimiser cannot fold to "aligned" the way it may fold the residue
 * of a typed pointer.
 *
 * The third part is the OPERAND of `co_yield` / `co_return` when it is aligned past the cache line. The three promise
 * types take it as a by-value parameter takes it (promise-operands.cpp holds what that means), and MSVC refuses a
 * by-value parameter aligned past 64 (error C2719): there -- only there -- they take such an operand by reference, so
 * that a generator or a task of it compiles at all. The cases read, on every compiler, an lvalue copied once and an
 * rvalue never, the same lvalue yielded twice copied then move-assigned, a move-only value and a braced aggregate
 * taken; and what a program observes differently on MSVC, each asserted per compiler: a forwarding constructor a
 * non-const lvalue no longer reaches, an explicit move constructor used (a copy fewer), a move-assignment that swaps
 * handing the previous value back to the operand. (A `T` whose copy constructor takes `T &`, and an rvalue of a type
 * converting to `T` through conversion functions qualified by value category, do not compile there.) They read through
 * a consumer coroutine whose own frame is aligned past the cache line, driven to its end without a scheduler: a
 * pool-block frame is aligned to 64 only, and a compiler may keep a local in the frame whatever its lifetime.
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
#include <string>
#include <type_traits>
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

/// Past the cache line: a value aligned to two of them. `generator<T>`'s frame takes the promise's alignment past the
/// cache line, and so do the pooled promise types' (a realigned block instead of a pool block). MSVC compiles all three:
/// it refuses a by-value parameter aligned past 64 (C2719), and there -- only there -- their yield_value and
/// return_value take the operand by reference (detail::operand_by_reference); none of these cases compiled there before.
struct alignas(128) Wider {
    std::uint64_t words[16]{};
};
static_assert(alignof(generator<Wider>::promise_type) == 128);

generator<Wider>
wider_values(std::uint64_t const seed) {
    Wider w;
    w.words[0] = seed;
    co_yield w;
}

// The pooled promise types past the cache line: their frames take a realigned block, not a pool block.
static_assert(alignof(task<Wider>::promise_type) == 128);
static_assert(alignof(async_generator<Wider>::promise_type) == 128);

task<Wider>
produce_wider(std::uint64_t const seed, std::uintptr_t *promise_at) {
    co_await promise_address{promise_at};
    Wider w;
    w.words[0] = seed;
    co_return w;
}

async_generator<Wider>
wider_values_async(std::uint64_t const seed, std::uintptr_t *promise_at) {
    co_await promise_address{promise_at};
    Wider w;
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

// ---- operands aligned past the cache line -------------------------------------------------------------------------

/// Counts its copies; aligned to two cache lines.
struct alignas(128) Counted128 {
    static inline int copies = 0;
    std::uint64_t     value  = 0;

    Counted128() = default;
    explicit Counted128(std::uint64_t v)
        : value(v) {}
    Counted128(Counted128 const &o)
        : value(o.value) {
        ++copies;
    }
    Counted128(Counted128 &&o) noexcept
        : value(o.value) {
        o.value = 0;
    }
    Counted128 &
    operator=(Counted128 const &o) {
        value = o.value;
        ++copies;
        return *this;
    }
    Counted128 &
    operator=(Counted128 &&o) noexcept {
        value   = o.value;
        o.value = 0;
        return *this;
    }
};

/// A move-only value aligned to two cache lines.
struct alignas(128) MoveOnly128 {
    std::unique_ptr<int> p;
};

/// An aggregate aligned to two cache lines, for a braced operand.
struct alignas(128) Pair128 {
    int         a = 0;
    std::string b;
};

/// An unconstrained forwarding constructor, aligned to two cache lines: a by-value parameter initialised from a
/// non-const lvalue runs it (an exact match); a `T const &` operand -- MSVC's -- is copied.
struct alignas(128) Forwarding128 {
    int v = 0;
    explicit Forwarding128(int x)
        : v(x) {}
    Forwarding128(Forwarding128 const &o)
        : v(o.v) {}
    Forwarding128(Forwarding128 &&o) noexcept
        : v(o.v) {}
    template <typename U, typename = std::enable_if_t<!std::is_integral_v<std::remove_cvref_t<U>>>>
    Forwarding128(U &&u)
        : v(u.v + 1000) {}
    Forwarding128 &
    operator=(Forwarding128 &&o) noexcept {
        v = o.v;
        return *this;
    }
    Forwarding128 &
    operator=(Forwarding128 const &o) {
        v = o.v;
        return *this;
    }
};
#if defined(_MSC_VER) && !defined(__clang__)
constexpr bool kByReference = true; ///< MSVC: the operand of a `T` aligned past 64 is a reference
#else
constexpr bool kByReference = false; ///< a by-value parameter
#endif
constexpr int kForwarded = kByReference ? 0 : 1000; ///< the copy constructor, or the forwarding one

/// What the special members of the logged shapes below did, in order: `C` a copy, `M` a move, `c` a copy-assignment,
/// `m` a move-assignment.
std::string log128;

/// Logs its special members; aligned to two cache lines.
struct alignas(128) Logged128 {
    int v = 0;
    explicit Logged128(int x)
        : v(x) {}
    Logged128(Logged128 const &o)
        : v(o.v) {
        log128 += "C";
    }
    Logged128(Logged128 &&o) noexcept
        : v(o.v) {
        log128 += "M";
    }
    Logged128 &
    operator=(Logged128 const &o) {
        v = o.v;
        log128 += "c";
        return *this;
    }
    Logged128 &
    operator=(Logged128 &&o) noexcept {
        v = o.v;
        log128 += "m";
        return *this;
    }
};

/// An explicit move constructor, aligned to two cache lines: a by-value parameter is copy-initialised, which never
/// calls it (a copy); MSVC's `T &&` operand is moved from directly.
struct alignas(128) ExplicitMove128 {
    int v = 0;
    explicit ExplicitMove128(int x)
        : v(x) {}
    ExplicitMove128(ExplicitMove128 const &o)
        : v(o.v) {
        log128 += "C";
    }
    explicit ExplicitMove128(ExplicitMove128 &&o) noexcept
        : v(o.v) {
        log128 += "M";
    }
    ExplicitMove128 &
    operator=(ExplicitMove128 &&o) noexcept {
        v = o.v;
        log128 += "m";
        return *this;
    }
    ExplicitMove128 &
    operator=(ExplicitMove128 const &o) {
        v = o.v;
        log128 += "c";
        return *this;
    }
};

/// A move-assignment that swaps, aligned to two cache lines: a by-value operand is the parameter, which takes the
/// previous value away with it; MSVC's `T &&` operand is the yielded object itself, which gets the previous value back.
struct alignas(128) Swapping128 {
    std::vector<int> d;
    Swapping128() = default;
    explicit Swapping128(int x)
        : d{x} {}
    Swapping128(Swapping128 const &)            = default;
    Swapping128(Swapping128 &&) noexcept        = default;
    Swapping128 &operator=(Swapping128 const &) = default;
    Swapping128 &
    operator=(Swapping128 &&o) noexcept {
        d.swap(o.d);
        return *this;
    }
};

/// A copy that cannot throw and a move that can, aligned to two cache lines.
struct alignas(128) ThrowingMove128 {
    ThrowingMove128() = default;
    ThrowingMove128(ThrowingMove128 const &) noexcept {}
    ThrowingMove128(ThrowingMove128 &&) noexcept(false) {}
    ThrowingMove128 &operator=(ThrowingMove128 const &) = default;
};

// The noexcept of each call is the one a by-value parameter gives -- its initialisation is part of the call -- on MSVC,
// where the operand is a reference, as everywhere else: an lvalue call throws where the copy or the move can, an
// rvalue call where the move can.
using counted128_promise       = task<Counted128>::promise_type;
using throwing_move128_promise = task<ThrowingMove128>::promise_type;
static_assert(noexcept(std::declval<counted128_promise &>().return_value(std::declval<Counted128 &&>())));
static_assert(!noexcept(std::declval<counted128_promise &>().return_value(std::declval<Counted128 const &>())));
static_assert(!noexcept(std::declval<counted128_promise &>().return_value(std::declval<Counted128 &>())));
static_assert(!noexcept(std::declval<throwing_move128_promise &>().return_value(std::declval<ThrowingMove128 &&>())));
static_assert(!noexcept(std::declval<throwing_move128_promise &>().return_value(std::declval<ThrowingMove128 const &>())));
static_assert(!noexcept(std::declval<throwing_move128_promise &>().return_value(std::declval<ThrowingMove128 &>())));

generator<Counted128>
g128_lvalue(Counted128 const *src) {
    co_yield *src;
}
generator<Counted128>
g128_rvalues() {
    Counted128 c{2};
    co_yield std::move(c);
    co_yield Counted128{3};
}
async_generator<Counted128>
a128_lvalue(Counted128 const *src) {
    co_yield *src;
}
async_generator<Counted128>
a128_rvalues() {
    Counted128 c{2};
    co_yield std::move(c);
    co_yield Counted128{3};
}
task<Counted128>
t128_lvalue(Counted128 const *src) {
    co_return *src;
}
task<Counted128>
t128_local() {
    Counted128 c{4};
    co_return c; // implicitly moved
}
task<Counted128>
t128_prvalue() {
    co_return Counted128{5};
}

generator<MoveOnly128>
g128_move_only() {
    MoveOnly128 m{std::make_unique<int>(6)};
    co_yield std::move(m);
    co_yield MoveOnly128{std::make_unique<int>(7)};
}
async_generator<MoveOnly128>
a128_move_only() {
    MoveOnly128 m{std::make_unique<int>(6)};
    co_yield std::move(m);
    co_yield MoveOnly128{std::make_unique<int>(7)};
}
task<MoveOnly128>
t128_move_only() {
    MoveOnly128 m{std::make_unique<int>(8)};
    co_return m; // a move-only local, implicitly moved
}

generator<Pair128>
g128_braced() {
    co_yield {1, "one"};
}
async_generator<Pair128>
a128_braced() {
    co_yield {2, "two"};
}
task<Pair128>
t128_braced() {
    co_return {3, "three"};
}

generator<Forwarding128>
g128_forwarding(Forwarding128 *p) {
    co_yield *p; // a non-const lvalue
}
async_generator<Forwarding128>
a128_forwarding(Forwarding128 *p) {
    co_yield *p;
}
task<Forwarding128>
t128_forwarding(Forwarding128 &r) {
    co_return r; // a reference parameter: an lvalue
}

generator<Logged128>
g128_logged_twice(Logged128 const *src) {
    co_yield *src;
    co_yield *src; // the held value is assigned
}
async_generator<Logged128>
a128_logged_twice(Logged128 const *src) {
    co_yield *src;
    co_yield *src;
}
generator<ExplicitMove128>
g128_explicit_move() {
    ExplicitMove128 x{11};
    co_yield std::move(x);
}
task<ExplicitMove128>
t128_explicit_move() {
    ExplicitMove128 x{13};
    co_return x; // implicitly moved
}
generator<Swapping128>
g128_swapping(std::vector<int> *after) {
    Swapping128 x{1};
    co_yield std::move(x);
    x.d = {2};
    co_yield std::move(x);
    *after = x.d;
}

/// What a consumer read; aligned to two cache lines, so the frame of a coroutine returning one is too, and the values
/// aligned past the cache line it holds while it reads sit at their alignment.
struct alignas(128) Observed128 {
    std::vector<long long>   values;
    std::vector<int>         copies;
    std::vector<std::string> strings;
};
static_assert(alignof(task<Observed128>::promise_type) == 128);

task<Observed128>
consume_counted128(Counted128 const *src) {
    Observed128 o;
    Counted128::copies = 0;
    {
        auto g = a128_lvalue(src);
        while (auto v = co_await g.next())
            o.values.push_back(static_cast<long long>(v->value));
    }
    o.copies.push_back(Counted128::copies);
    Counted128::copies = 0;
    {
        auto g = a128_rvalues();
        while (auto v = co_await g.next())
            o.values.push_back(static_cast<long long>(v->value));
    }
    o.copies.push_back(Counted128::copies);
    Counted128::copies = 0;
    o.values.push_back(static_cast<long long>((co_await t128_lvalue(src)).value));
    o.copies.push_back(Counted128::copies);
    Counted128::copies = 0;
    o.values.push_back(static_cast<long long>((co_await t128_local()).value));
    o.values.push_back(static_cast<long long>((co_await t128_prvalue()).value));
    o.copies.push_back(Counted128::copies);
    co_return o;
}

task<Observed128>
consume_move_only_and_braced128() {
    Observed128 o;
    {
        auto g = a128_move_only();
        while (auto v = co_await g.next())
            o.values.push_back(v->p ? *v->p : -1);
    }
    {
        MoveOnly128 const m = co_await t128_move_only();
        o.values.push_back(m.p ? *m.p : -1);
    }
    {
        auto g = a128_braced();
        while (auto v = co_await g.next()) {
            o.values.push_back(v->a);
            o.strings.push_back(v->b);
        }
    }
    {
        Pair128 const r = co_await t128_braced();
        o.values.push_back(r.a);
        o.strings.push_back(r.b);
    }
    co_return o;
}

task<Observed128>
consume_forwarding128(Forwarding128 *p) {
    Observed128 o;
    {
        auto g = a128_forwarding(p);
        while (auto v = co_await g.next())
            o.values.push_back(v->v);
    }
    o.values.push_back((co_await t128_forwarding(*p)).v);
    co_return o;
}

task<Observed128>
consume_logged128(Logged128 const *src) {
    Observed128 o;
    log128.clear();
    {
        auto g = a128_logged_twice(src);
        while (auto v = co_await g.next()) {
            o.values.push_back(v->v);
            o.strings.push_back(log128);
            log128.clear();
        }
    }
    co_return o;
}

task<Observed128>
consume_explicit_move128() {
    Observed128 o;
    log128.clear();
    o.values.push_back((co_await t128_explicit_move()).v);
    o.strings.push_back(log128);
    co_return o;
}

/// Runs a task that never waits on the loop to its end, without a scheduler, and reads its value where the promise
/// keeps it; null when it did not end with a value.
template <typename T>
T const *
run_to_end(task<T> const &t) {
    auto const h = t.handle();
    if (!h)
        return nullptr;
    h.resume();
    if (!h.done() || detail::promise_of(h).has_exception())
        return nullptr;
    return &detail::promise_of(h).value();
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
 * @test A `generator<Wider>` -- a promise aligned past the cache line -- has its promise at that alignment, frame
 *       after frame: `generator<T>`'s frame takes the promise's alignment when it exceeds the cache line.
 */
TEST_F(OverAlignedPromise, GeneratorOfAValueAlignedPastTheCacheLineHasAnAlignedPromise) {
    std::vector<generator<Wider>> gens;
    spacers                       between;
    gens.reserve(kFrames);
    between.reserve(kFrames);
    for (std::uint64_t i = 0; i < kFrames; ++i) {
        gens.push_back(wider_values(i));
        between.push_back(std::make_unique<std::uint64_t>(i));
    }
    alignment_census census;
    for (std::uint64_t i = 0; i < kFrames; ++i) {
        auto it = gens[i].begin();
        ASSERT_NE(it, gens[i].end());
        Wider const &held = *it; // a reference into the promise, not a copy
        EXPECT_EQ(held.words[0], i);
        census.record(opaque_address(&held), alignof(generator<Wider>::promise_type));
    }
    EXPECT_TRUE(all_aligned(census, kFrames, alignof(generator<Wider>::promise_type)));
}

/**
 * @test A `task<Wider>` has its promise at the promise's alignment, frame after frame: past the cache line the pooled
 *       promise types take a realigned block instead of a pool block.
 */
TEST_F(OverAlignedPromise, TaskOfAValueAlignedPastTheCacheLineHasAnAlignedPromise) {
    std::atomic<bool>           done{false};
    std::atomic<std::size_t>    values{0};
    std::vector<std::uintptr_t> promises(kFrames, 0);
    auto                        done_ptr     = &done;
    auto                        values_ptr   = &values;
    auto                        promises_ptr = &promises;
    std::atomic<long>           counted_alive{-1};
    std::atomic<long>           counted_after{-1};
    auto                        alive_ptr = &counted_alive;
    auto                        after_ptr = &counted_after;
    coro_scheduler().spawn([done_ptr, values_ptr, promises_ptr, alive_ptr, after_ptr]() -> task<void> {
        using detail::CoroutineFrameAllocator;
        long const               before = CoroutineFrameAllocator::live_frames;
        std::vector<task<Wider>> tasks;
        spacers                  between;
        tasks.reserve(kFrames);
        between.reserve(kFrames);
        for (std::uint64_t i = 0; i < kFrames; ++i) {
            tasks.push_back(produce_wider(i, &(*promises_ptr)[i]));
            between.push_back(std::make_unique<std::uint64_t>(i));
        }
        alive_ptr->store(CoroutineFrameAllocator::live_frames - before);
        std::size_t n = 0;
        for (std::uint64_t i = 0; i < kFrames; ++i) {
            const Wider w = co_await std::move(tasks[i]);
            if (w.words[0] == i)
                ++n;
        }
        tasks.clear(); // every frame destroyed
        after_ptr->store(CoroutineFrameAllocator::live_frames - before);
        values_ptr->store(n);
        done_ptr->store(true);
        co_return;
    });
    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "the spawned coroutine never completed";
    EXPECT_EQ(values.load(), kFrames);
    alignment_census census;
    for (auto const a : promises)
        census.record(a, alignof(task<Wider>::promise_type));
    EXPECT_TRUE(all_aligned(census, kFrames, alignof(task<Wider>::promise_type)));
    // Past the line the frames are not pool blocks, and the pool's per-thread count still sees them -- the leak check
    // qb's tests rely on: kFrames while they live, back where it was once they are gone.
    EXPECT_EQ(counted_alive.load(), static_cast<long>(kFrames));
    EXPECT_EQ(counted_after.load(), 0);
}

/**
 * @test An `async_generator<Wider>` has its promise at the promise's alignment, frame after frame.
 */
TEST_F(OverAlignedPromise, AsyncGeneratorOfAValueAlignedPastTheCacheLineHasAnAlignedPromise) {
    std::atomic<bool>           done{false};
    std::atomic<std::size_t>    values{0};
    std::vector<std::uintptr_t> promises(kFrames, 0);
    auto                        done_ptr     = &done;
    auto                        values_ptr   = &values;
    auto                        promises_ptr = &promises;
    coro_scheduler().spawn([done_ptr, values_ptr, promises_ptr]() -> task<void> {
        std::vector<async_generator<Wider>> gens;
        spacers                             between;
        gens.reserve(kFrames);
        between.reserve(kFrames);
        for (std::uint64_t i = 0; i < kFrames; ++i) {
            gens.push_back(wider_values_async(i, &(*promises_ptr)[i]));
            between.push_back(std::make_unique<std::uint64_t>(i));
        }
        std::size_t n = 0;
        for (std::uint64_t i = 0; i < kFrames; ++i) {
            std::optional<Wider> const v = co_await gens[i].next();
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
        census.record(a, alignof(async_generator<Wider>::promise_type));
    EXPECT_TRUE(all_aligned(census, kFrames, alignof(async_generator<Wider>::promise_type)));
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

// ---- operands aligned past the cache line -------------------------------------------------------------------------

/**
 * @test An operand aligned past the cache line is copied once from an lvalue and never from an rvalue, by the three
 *       promise types: by value, or on MSVC by reference.
 */
TEST_F(OverAlignedPromise, AnOperandAlignedPastTheCacheLineIsCopiedOnceFromAnLvalueNeverFromAnRvalue) {
    Counted128 const           src{1};
    std::vector<std::uint64_t> seen;
    Counted128::copies = 0;
    for (auto const &x : g128_lvalue(&src))
        seen.push_back(x.value);
    EXPECT_EQ(Counted128::copies, 1);
    Counted128::copies = 0;
    for (auto const &x : g128_rvalues())
        seen.push_back(x.value);
    EXPECT_EQ(Counted128::copies, 0);
    EXPECT_EQ(seen, (std::vector<std::uint64_t>{1, 2, 3}));
    EXPECT_EQ(src.value, 1u);

    auto const               consumer = consume_counted128(&src);
    Observed128 const *const o        = run_to_end(consumer);
    ASSERT_NE(o, nullptr) << "the consumer did not end with a value";
    EXPECT_EQ(o->values, (std::vector<long long>{1, 2, 3, 1, 4, 5}));
    EXPECT_EQ(o->copies, (std::vector<int>{1, 0, 1, 0})) << "async lvalue, async rvalues, task lvalue, task rvalues";
    EXPECT_EQ(src.value, 1u);
}

/**
 * @test A move-only operand and a braced aggregate aligned past the cache line are taken by the three promise types.
 */
TEST_F(OverAlignedPromise, AMoveOnlyOrBracedOperandAlignedPastTheCacheLineIsTaken) {
    std::vector<int> moved;
    for (auto const &x : g128_move_only())
        moved.push_back(x.p ? *x.p : -1);
    EXPECT_EQ(moved, (std::vector<int>{6, 7}));
    std::vector<std::string> braced;
    for (auto const &x : g128_braced())
        braced.push_back(std::to_string(x.a) + x.b);
    EXPECT_EQ(braced, (std::vector<std::string>{"1one"}));

    auto const               consumer = consume_move_only_and_braced128();
    Observed128 const *const o        = run_to_end(consumer);
    ASSERT_NE(o, nullptr) << "the consumer did not end with a value";
    EXPECT_EQ(o->values, (std::vector<long long>{6, 7, 8, 2, 3}));
    EXPECT_EQ(o->strings, (std::vector<std::string>{"two", "three"}));
}

/**
 * @test What MSVC's reference changes, one: a non-const lvalue of a `T` aligned past the cache line with an
 *       unconstrained forwarding constructor runs that constructor where the operand is a by-value parameter, and is
 *       copied on MSVC, where it is a `T const &`.
 */
TEST_F(OverAlignedPromise, AnLvalueAlignedPastTheCacheLineIsABindingOnlyOnMsvc) {
    Forwarding128 src{8};
    int           seen = 0;
    for (auto const &x : g128_forwarding(&src))
        seen = x.v;
    EXPECT_EQ(seen, 8 + kForwarded);

    auto const               consumer = consume_forwarding128(&src);
    Observed128 const *const o        = run_to_end(consumer);
    ASSERT_NE(o, nullptr) << "the consumer did not end with a value";
    EXPECT_EQ(o->values, (std::vector<long long>{8 + kForwarded, 8 + kForwarded}));
}

/**
 * @test The same lvalue aligned past the cache line, yielded twice, is copied then moved into the held value, then
 *       copied then move-assigned to it -- what a by-value parameter does, and MSVC's `T const &` overload too (it
 *       copies into a temporary first: assigned from the lvalue, the held value would be copy-assigned).
 */
TEST_F(OverAlignedPromise, TheSameLvalueAlignedPastTheCacheLineYieldedTwiceIsCopiedThenMoved) {
    Logged128 const          src{5};
    std::vector<std::string> logs;
    log128.clear();
    for (auto const &x : g128_logged_twice(&src)) {
        EXPECT_EQ(x.v, 5);
        logs.push_back(log128);
        log128.clear();
    }
    EXPECT_EQ(logs, (std::vector<std::string>{"CM", "Cm"}));

    auto const               consumer = consume_logged128(&src);
    Observed128 const *const o        = run_to_end(consumer);
    ASSERT_NE(o, nullptr) << "the consumer did not end with a value";
    EXPECT_EQ(o->values, (std::vector<long long>{5, 5}));
    // each yield, then next(), which moves the held value out
    EXPECT_EQ(o->strings, (std::vector<std::string>{"CMM", "CmM"}));
}

/**
 * @test What MSVC's reference changes, two: an explicit move constructor of a `T` aligned past the cache line is never
 *       called for a by-value parameter (copy-initialised: a copy), and is on MSVC, where the operand is a `T &&` -- a
 *       copy fewer.
 */
TEST_F(OverAlignedPromise, AnExplicitMoveConstructorAlignedPastTheCacheLineIsUsedOnlyOnMsvc) {
    log128.clear();
    for (auto const &x : g128_explicit_move())
        EXPECT_EQ(x.v, 11);
    EXPECT_EQ(log128, kByReference ? "M" : "CM");

    auto const               consumer = consume_explicit_move128();
    Observed128 const *const o        = run_to_end(consumer);
    ASSERT_NE(o, nullptr) << "the consumer did not end with a value";
    EXPECT_EQ(o->values, (std::vector<long long>{13}));
    // into the parameter (a copy, by value only), into the promise, out by await_resume (copy-initialised: a copy)
    EXPECT_EQ(o->strings, (std::vector<std::string>{kByReference ? "MC" : "CMC"}));
}

/**
 * @test What MSVC's reference changes, three: a move-assignment that swaps, of a `T` aligned past the cache line, hands
 *       the previous value to the parameter that dies with it where the operand is a by-value parameter, and back to the
 *       operand itself on MSVC, where it is a `T &&`.
 */
TEST_F(OverAlignedPromise, ASwappingMoveAssignmentAlignedPastTheCacheLineSwapsWithTheOperandOnlyOnMsvc) {
    std::vector<int>              after{-9};
    std::vector<std::vector<int>> seen;
    for (auto const &x : g128_swapping(&after))
        seen.push_back(x.d);
    EXPECT_EQ(seen, (std::vector<std::vector<int>>{{1}, {2}}));
    if (kByReference)
        EXPECT_EQ(after, (std::vector<int>{1})) << "the operand did not get the previous value back";
    else
        EXPECT_NE(after, (std::vector<int>{1})) << "the operand of the second co_yield got the previous value back";
}
