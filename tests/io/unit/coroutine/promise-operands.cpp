/**
 * @file unit/coroutine/promise-operands.cpp
 * @brief What `co_yield` and `co_return` accept, and what they do with it, for `generator<T>`,
 *        `async_generator<T>` and `task<T>` (Huly QB-1246).
 *
 * The three promise types take the operand of `co_yield` / `co_return` as a by-value parameter of type `T` takes it,
 * on every compiler and for every `T` a compiler passes by value. Only where MSVC refuses such a parameter -- a `T`
 * aligned past 64 (C2719), for which nothing compiled there before -- do they take it by reference instead, through a
 * `T const &` and a `T &&` overload (over-aligned-promise.cpp holds those operands). This file holds the by-value
 * meaning. Every operand shape -- an lvalue, a moved lvalue, a prvalue, a const lvalue, a derived lvalue, a type
 * converting to `T const &`, a value converted to `T`, a braced list, an empty brace, a move-only `T`, a `T` whose
 * rvalues are copied (no move constructor), a `T` that is not copy-assignable (a generator's second yield assigns), an
 * implicitly moved local, a returned coroutine parameter -- compiles and produces the value it does, with the copies it
 * does: an lvalue is copied once, an rvalue never. So do the shapes a `T const &` / `T &&` pair would REFUSE (a volatile
 * scalar, a copy constructor taking a non-const reference, conversion functions to `T const &` and to `T &&` qualified
 * by the operand's value category) or give ANOTHER MEANING (a forwarding constructor, an explicit move constructor, a
 * move-assignment that swaps, a one-element brace of a `T` with an initializer_list constructor, the `noexcept` of a
 * call whose move can throw) -- each asserted with what the by-value parameter makes of it (the brace, on which the
 * compilers disagree, with what a by-value parameter of `T` is on the compiler that builds it). The `noexcept` of every call shape is asserted
 * at compile time: a by-value parameter's initialisation is part of the call expression. (A `task<T &>` or a `generator<T &>` cannot be
 * instantiated: their results are a `std::variant` and a `std::optional`.)
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

#include <any>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <qb/io/async/coroutine.h>

#include "../../shared/coroutine_test_support.h"

using namespace qb::io::async;
using namespace std::chrono_literals;
using qb::io::test::pump_until;
using qb::io::test::reset_async_context;

namespace promise_operands_test {

/// A value that counts its copies and its moves; a moved-from one reads -1.
struct Counted {
    static inline int copies = 0;
    static inline int moves  = 0;
    int               value  = 0;

    Counted() = default;
    explicit Counted(int v)
        : value(v) {}
    Counted(Counted const &o)
        : value(o.value) {
        ++copies;
    }
    Counted(Counted &&o) noexcept
        : value(o.value) {
        o.value = -1;
        ++moves;
    }
    Counted &
    operator=(Counted const &o) {
        value = o.value;
        ++copies;
        return *this;
    }
    Counted &
    operator=(Counted &&o) noexcept {
        value   = o.value;
        o.value = -1;
        ++moves;
        return *this;
    }
    static void
    reset() noexcept {
        copies = 0;
        moves  = 0;
    }
};

/// A class derived from the yielded type: an lvalue of it is sliced into a copy, as it was.
struct Derived : Counted {
    int extra = 0;
};

/// A type converting to `Counted const &`: a conversion function, then a copy.
struct Holder {
    Counted c{7};
            operator Counted const &() const noexcept {
        return c;
    }
};

/// A value whose rvalues are copied: a declared copy constructor, so no move constructor.
struct CopyOnly {
    int v      = 0;
    CopyOnly() = default;
    explicit CopyOnly(int x)
        : v(x) {}
    CopyOnly(CopyOnly const &)            = default;
    CopyOnly &operator=(CopyOnly const &) = default;
};

/// Copy-constructible and move-assignable, NOT copy-assignable: what the by-value parameter accepted for an lvalue (a
/// copy into the parameter, then a move-assignment), and what a copy-assignment of the lvalue would refuse.
struct MoveAssignOnly {
    int v            = 0;
    MoveAssignOnly() = default;
    explicit MoveAssignOnly(int x)
        : v(x) {}
    MoveAssignOnly(MoveAssignOnly const &)            = default;
    MoveAssignOnly(MoveAssignOnly &&)                 = default;
    MoveAssignOnly &operator=(MoveAssignOnly const &) = delete;
    MoveAssignOnly &operator=(MoveAssignOnly &&)      = default;
};

// ---- generator<T> -----------------------------------------------------------------------------------------------

generator<Counted>
g_lvalue(int *source_after) {
    Counted c{7};
    co_yield c;
    *source_after = c.value;
}
generator<Counted>
g_moved(int *source_after) {
    Counted c{7};
    co_yield std::move(c);
    *source_after = c.value;
}
generator<Counted>
g_prvalue() {
    co_yield Counted{7};
}
generator<Counted>
g_const_lvalue() {
    Counted const c{7};
    co_yield c;
}
generator<Counted>
g_derived() {
    Derived d;
    d.value = 7;
    co_yield d;
}
generator<Counted>
g_converted_by_operator(Holder const *h) {
    co_yield *h;
}
generator<std::string>
g_converted() {
    co_yield "abc";
}
generator<long>
g_promoted() {
    co_yield 5;
}
generator<std::pair<int, std::string>>
g_braced_pair() {
    co_yield {1, "a"};
}
generator<std::vector<int>>
g_braced_vector() {
    co_yield {1, 2, 3};
    co_yield {};
}
generator<std::unique_ptr<int>>
g_move_only() {
    co_yield std::make_unique<int>(5);
    auto p = std::make_unique<int>(6);
    co_yield std::move(p);
}
generator<CopyOnly>
g_copy_only() {
    CopyOnly x{3};
    co_yield x;
    co_yield CopyOnly{4};
}
generator<MoveAssignOnly>
g_move_assign_only() {
    MoveAssignOnly x{5};
    co_yield x; // the first yield constructs the held value
    x.v = 6;
    co_yield x; // the second assigns it: a move-assignment, as it was
}

template <typename T>
std::vector<T>
drain(generator<T> g) {
    std::vector<T> out;
    for (auto const &v : g)
        out.push_back(v);
    return out;
}

// ---- async_generator<T> -----------------------------------------------------------------------------------------

async_generator<Counted>
a_lvalue(int *source_after) {
    Counted c{7};
    co_yield c;
    *source_after = c.value;
}
async_generator<Counted>
a_moved(int *source_after) {
    Counted c{7};
    co_yield std::move(c);
    *source_after = c.value;
}
async_generator<std::string>
a_converted() {
    co_yield "abc";
}
async_generator<std::vector<int>>
a_braced_vector() {
    co_yield {1, 2, 3};
}
async_generator<std::unique_ptr<int>>
a_move_only() {
    co_yield std::make_unique<int>(5);
}
async_generator<CopyOnly>
a_copy_only() {
    CopyOnly x{3};
    co_yield x;
}
async_generator<MoveAssignOnly>
a_move_assign_only() {
    MoveAssignOnly x{5};
    co_yield x;
    x.v = 6;
    co_yield x;
}

// ---- task<T> ----------------------------------------------------------------------------------------------------

task<Counted>
t_local() {
    Counted c{7};
    co_return c; // implicitly moved
}
task<Counted>
t_lvalue(Counted const *source) {
    co_return *source; // an lvalue nothing moves
}
task<Counted>
t_prvalue() {
    co_return Counted{7};
}
task<Counted>
t_const_local() {
    Counted const c{7};
    co_return c; // const: copied
}
task<Counted>
t_parameter(Counted c) {
    co_return c; // the frame's copy of the parameter, implicitly moved
}
task<std::string>
t_converted() {
    co_return "abc";
}
task<std::string>
t_converted_local() {
    char const *s = "abc";
    co_return s;
}
task<long>
t_promoted() {
    co_return 5;
}
task<std::pair<int, std::string>>
t_braced_pair() {
    co_return {1, "a"};
}
task<std::vector<int>>
t_braced_vector() {
    co_return {1, 2, 3};
}
task<std::vector<int>>
t_empty_brace() {
    co_return {};
}
task<std::optional<int>>
t_nullopt() {
    co_return std::nullopt;
}
task<std::unique_ptr<int>>
t_move_only_prvalue() {
    co_return std::make_unique<int>(5);
}
task<std::unique_ptr<int>>
t_move_only_local() {
    auto p = std::make_unique<int>(6);
    co_return p;
}
task<CopyOnly>
t_copy_only() {
    CopyOnly x{3};
    co_return x;
}

// The noexcept of each call shape: a by-value parameter's initialisation is part of the call, so an lvalue call can
// throw where the copy can, and an rvalue call where the move can.
using counted_promise = task<Counted>::promise_type;
using string_promise  = task<std::string>::promise_type;
using int_promise     = task<int>::promise_type;
static_assert(noexcept(std::declval<counted_promise &>().return_value(std::declval<Counted &&>())));
static_assert(!noexcept(std::declval<counted_promise &>().return_value(std::declval<Counted const &>())));
static_assert(!noexcept(std::declval<counted_promise &>().return_value(std::declval<Counted &>())));
static_assert(noexcept(std::declval<string_promise &>().return_value(std::declval<std::string &&>())));
static_assert(!noexcept(std::declval<string_promise &>().return_value(std::declval<std::string const &>())));
static_assert(noexcept(std::declval<int_promise &>().return_value(std::declval<int &&>())));
static_assert(noexcept(std::declval<int_promise &>().return_value(std::declval<int const &>())));

/// What the special members of the shapes below did, in order: `C` a copy, `C&` a copy from a non-const lvalue, `M` a
/// move, `T` the forwarding constructor, `m` a move-assignment, `<&>` / `<&&>` a conversion function.
std::string shape_log;

// ---- operands a reference parameter refuses ---------------------------------------------------------------------
// A `T const &` / `T &&` pair would refuse these three; a by-value parameter accepts each.

/// A volatile scalar: neither `int const &` nor `int &&` binds a volatile lvalue; a by-value `int` reads it.
generator<int>
g_volatile() {
    volatile int v = 3;
    co_yield v;
}
async_generator<int>
a_volatile() {
    volatile int v = 4;
    co_yield v;
}
task<int>
t_volatile() {
    volatile int v = 5;
    co_return v; // a volatile local is never implicitly moved
}

/// A copy constructor taking a NON-const reference: a by-value parameter copies a non-const lvalue with it, which a
/// `T const &` operand cannot.
struct NonConstCopy {
    int v = 0;
    explicit NonConstCopy(int x)
        : v(x) {}
    NonConstCopy(NonConstCopy &o)
        : v(o.v) {
        shape_log += "C&";
    }
    NonConstCopy(NonConstCopy &&o) noexcept
        : v(o.v) {
        o.v = -1;
        shape_log += "M";
    }
    NonConstCopy &
    operator=(NonConstCopy &&o) noexcept {
        v   = o.v;
        o.v = -1;
        shape_log += "m";
        return *this;
    }
};
generator<NonConstCopy>
g_non_const_copy(NonConstCopy *p) {
    co_yield *p;
}
async_generator<NonConstCopy>
a_non_const_copy(NonConstCopy *p) {
    co_yield *p;
}
task<NonConstCopy>
t_non_const_copy(NonConstCopy *p) {
    co_return *p;
}

/// The yielded type of `Converting`.
struct Converted {
    int v = 0;
    explicit Converted(int x)
        : v(x) {}
    Converted(Converted const &o)
        : v(o.v) {
        shape_log += "C";
    }
    Converted(Converted &&o) noexcept
        : v(o.v) {
        shape_log += "M";
    }
    Converted &
    operator=(Converted &&o) noexcept {
        v = o.v;
        shape_log += "m";
        return *this;
    }
};
/// Converts to `Converted const &` from an lvalue and to `Converted &&` from an rvalue: a by-value parameter takes the
/// conversion the operand's value category selects; for a `T const &` / `T &&` pair both bindings are viable through
/// the same conversion function, an ambiguous call.
struct Converting {
    Converted held{23};
              operator Converted const &() const           &{
        shape_log += "<&>";
        return held;
    }
    operator Converted &&() && {
        shape_log += "<&&>";
        return std::move(held);
    }
};
generator<Converted>
g_converting(Converting *c) {
    co_yield *c;
    co_yield std::move(*c);
}
async_generator<Converted>
a_converting(Converting *c) {
    co_yield *c;
    co_yield std::move(*c);
}
task<Converted>
t_converting_lvalue(Converting *c) {
    co_return *c;
}
task<Converted>
t_converting_prvalue() {
    co_return Converting{};
}

// ---- operands a reference parameter gives another meaning -------------------------------------------------------
// A `T const &` / `T &&` pair would accept these, and do something else with them.

/// An unconstrained forwarding constructor: a by-value parameter initialised from a non-const lvalue, or from a const
/// local `co_return` moves, runs it (an exact match, where the copy constructor needs a const binding); a `T const &`
/// operand would be copied. Aligned to `Align`: a `T` aligned to the cache line itself is taken by value on every
/// compiler (MSVC passes a by-value parameter aligned to 64), so it means the same.
template <std::size_t Align>
struct alignas(Align) Forwarding {
    int v = 0;
    explicit Forwarding(int x)
        : v(x) {}
    Forwarding(Forwarding const &o)
        : v(o.v) {
        shape_log += "C";
    }
    Forwarding(Forwarding &&o) noexcept
        : v(o.v) {
        shape_log += "M";
    }
    template <typename U, typename = std::enable_if_t<!std::is_integral_v<std::remove_cvref_t<U>>>>
    Forwarding(U &&u)
        : v(u.v + 1000) {
        shape_log += "T";
    }
    Forwarding &
    operator=(Forwarding &&o) noexcept {
        v = o.v;
        shape_log += "m";
        return *this;
    }
    Forwarding &
    operator=(Forwarding const &o) {
        v = o.v;
        shape_log += "c";
        return *this;
    }
};
static_assert(alignof(Forwarding<64>) == 64);

template <std::size_t Align>
generator<Forwarding<Align>>
g_forwarding(Forwarding<Align> *p) {
    co_yield *p; // a non-const lvalue
}
template <std::size_t Align>
async_generator<Forwarding<Align>>
a_forwarding(Forwarding<Align> *p) {
    co_yield *p;
}
template <std::size_t Align>
task<Forwarding<Align>>
t_forwarding_reference(Forwarding<Align> &r) {
    co_return r; // a reference parameter: an lvalue, never implicitly moved
}
template <std::size_t Align>
task<Forwarding<Align>>
t_forwarding_const_local() {
    Forwarding<Align> const c{10};
    co_return c; // a const local, implicitly moved: a const rvalue
}

/// An explicit move constructor: a by-value parameter is copy-initialised, which never calls an explicit constructor,
/// so an rvalue operand is copied into it, then moved into the promise.
struct ExplicitMove {
    int v = 0;
    explicit ExplicitMove(int x)
        : v(x) {}
    ExplicitMove(ExplicitMove const &o)
        : v(o.v) {
        shape_log += "C";
    }
    explicit ExplicitMove(ExplicitMove &&o) noexcept
        : v(o.v) {
        shape_log += "M";
    }
    ExplicitMove &
    operator=(ExplicitMove &&o) noexcept {
        v = o.v;
        shape_log += "m";
        return *this;
    }
    ExplicitMove &
    operator=(ExplicitMove const &o) {
        v = o.v;
        shape_log += "c";
        return *this;
    }
};
generator<ExplicitMove>
g_explicit_move() {
    ExplicitMove x{11};
    co_yield std::move(x);
}
async_generator<ExplicitMove>
a_explicit_move() {
    ExplicitMove x{12};
    co_yield std::move(x);
}
task<ExplicitMove>
t_explicit_move_local() {
    ExplicitMove x{13};
    co_return x;
}
task<ExplicitMove>
t_explicit_move_rvalue() {
    ExplicitMove x{14};
    co_return std::move(x);
}

/// A move-assignment that swaps: `co_yield std::move(x)` moves x into the parameter, and the generator's second yield
/// move-assigns the parameter into the value it holds -- the value swapped out dies with the parameter, and x is left
/// moved-from. (A reference operand would be the one swapped: x would get the previous value back.)
struct Swapping {
    std::vector<int> d;
    Swapping() = default;
    explicit Swapping(int x)
        : d{x} {}
    Swapping(Swapping const &)            = default;
    Swapping(Swapping &&) noexcept        = default;
    Swapping &operator=(Swapping const &) = default;
    Swapping &
    operator=(Swapping &&o) noexcept {
        d.swap(o.d);
        return *this;
    }
};
generator<Swapping>
g_swapping(std::vector<int> *after) {
    Swapping x{1};
    co_yield std::move(x);
    x.d = {2};
    co_yield std::move(x);
    *after = x.d;
}

/// A one-element brace whose element is a `T`, `T` having an initializer_list constructor its element converts to: the
/// by-value parameter is copy-list-initialised from it, and the compilers build different things (measured: g++ 14
/// and clang-cl 22 the initializer_list's vector of one `std::any`, clang 19 and MSVC 19.51 a copy of the element, a
/// vector of three). So the operand is held to what a by-value parameter of `T` is on the compiler that builds it,
/// whatever that is. A reference operand would bind the element itself, a vector of three, on all of them.
using anys = std::vector<std::any>;
generator<anys>
g_brace_of_one(anys const *p) {
    co_yield {*p};
}
async_generator<anys>
a_brace_of_one(anys const *p) {
    co_yield {*p};
}
task<anys>
t_brace_of_one(anys const *p) {
    co_return {*p};
}
task<anys>
t_brace_of_one_local() {
    anys v{1, 2, 3};
    co_return {v};
}
/// The size of a by-value parameter copy-list-initialised from `{*p}`, and from `{v}` for a non-const `v`.
std::size_t
brace_of_one_as_a_parameter(anys const *p) {
    auto const size_of = [](anys parameter) {
        return parameter.size();
    };
    return size_of({*p});
}
std::size_t
brace_of_one_local_as_a_parameter() {
    auto const size_of = [](anys parameter) {
        return parameter.size();
    };
    anys v{1, 2, 3};
    return size_of({v});
}

// ---- the noexcept a by-value parameter gives ----------------------------------------------------------------------

/// A copy that cannot throw and a move that can: a call is noexcept only when the parameter's initialisation and the
/// function both are, so an lvalue call is not, though the copy cannot throw.
struct ThrowingMove {
    ThrowingMove() = default;
    ThrowingMove(ThrowingMove const &) noexcept {}
    ThrowingMove(ThrowingMove &&) noexcept(false) {}
    ThrowingMove &operator=(ThrowingMove const &) = default;
};
using throwing_move_promise = task<ThrowingMove>::promise_type;
static_assert(!noexcept(std::declval<throwing_move_promise &>().return_value(std::declval<ThrowingMove &>())));
static_assert(!noexcept(std::declval<throwing_move_promise &>().return_value(std::declval<ThrowingMove const &>())));
static_assert(!noexcept(std::declval<throwing_move_promise &>().return_value(std::declval<ThrowingMove &&>())));

// ---- the fixture --------------------------------------------------------------------------------------------------

class PromiseOperands : public ::testing::Test {
protected:
    void
    SetUp() override {
        reset_async_context();
        Counted::reset();
        shape_log.clear();
    }
    void
    TearDown() override {
        if (listener::current.has_coro_scheduler()) {
            run_for(5ms);
            listener::current.reset_coro_scheduler();
        }
        listener::current.clear();
    }

public:
    /// Run `body` as a spawned coroutine to its end.
    template <typename F>
    void
    run(F body) {
        std::atomic<bool> done{false};
        auto              done_ptr = &done;
        coro_scheduler().spawn([body, done_ptr]() -> task<void> {
            co_await body();
            done_ptr->store(true);
        });
        ASSERT_TRUE(pump_until([&] { return done.load(); })) << "the coroutine never completed";
    }
};

} // namespace promise_operands_test

using namespace promise_operands_test;

// ---- generator<T> -----------------------------------------------------------------------------------------------

TEST_F(PromiseOperands, GeneratorYieldsACopyOfAnLvalue) {
    int after = 0, seen = 0;
    {
        auto g = g_lvalue(&after);
        for (auto const &v : g)
            seen = v.value;
    }
    EXPECT_EQ(seen, 7);
    EXPECT_EQ(after, 7) << "the yielded lvalue was moved from";
    EXPECT_EQ(Counted::copies, 1);
}

TEST_F(PromiseOperands, GeneratorMovesAnRvalueNeverCopiesIt) {
    int after = 0;
    {
        auto g = g_moved(&after);
        for (auto const &v : g)
            EXPECT_EQ(v.value, 7);
    }
    EXPECT_EQ(after, -1) << "std::move(c) was not moved from";
    EXPECT_EQ(Counted::copies, 0);
    {
        auto g = g_prvalue();
        for (auto const &v : g)
            EXPECT_EQ(v.value, 7);
    }
    EXPECT_EQ(Counted::copies, 0);
}

TEST_F(PromiseOperands, GeneratorCopiesAConstAndADerivedLvalueAndAConversion) {
    for (auto const &v : g_const_lvalue())
        EXPECT_EQ(v.value, 7);
    EXPECT_EQ(Counted::copies, 1);
    Counted::reset();
    for (auto const &v : g_derived())
        EXPECT_EQ(v.value, 7);
    EXPECT_EQ(Counted::copies, 1);
    Counted::reset();
    Holder const h;
    for (auto const &v : g_converted_by_operator(&h))
        EXPECT_EQ(v.value, 7);
    EXPECT_EQ(Counted::copies, 1);
}

TEST_F(PromiseOperands, GeneratorConvertsBracesAndPromotes) {
    EXPECT_EQ(drain(g_converted()), (std::vector<std::string>{"abc"}));
    EXPECT_EQ(drain(g_promoted()), (std::vector<long>{5L}));
    EXPECT_EQ(drain(g_braced_pair()), (std::vector<std::pair<int, std::string>>{{1, "a"}}));
    EXPECT_EQ(drain(g_braced_vector()), (std::vector<std::vector<int>>{{1, 2, 3}, {}}));
}

TEST_F(PromiseOperands, GeneratorTakesMoveOnlyAndCopyOnlyValues) {
    std::vector<int> seen;
    for (auto const &p : g_move_only())
        seen.push_back(*p);
    EXPECT_EQ(seen, (std::vector<int>{5, 6}));
    std::vector<int> copied;
    for (auto const &c : g_copy_only())
        copied.push_back(c.v);
    EXPECT_EQ(copied, (std::vector<int>{3, 4}));
    std::vector<int> assigned;
    for (auto const &c : g_move_assign_only())
        assigned.push_back(c.v);
    EXPECT_EQ(assigned, (std::vector<int>{5, 6}));
}

// ---- async_generator<T> -----------------------------------------------------------------------------------------

TEST_F(PromiseOperands, AsyncGeneratorCopiesAnLvalueAndMovesAnRvalue) {
    int lvalue_after = 0, rvalue_after = 0, lvalue_copies = -1, rvalue_copies = -1;
    run([&]() -> task<void> {
        {
            auto g = a_lvalue(&lvalue_after);
            auto v = co_await g.next();
            EXPECT_EQ(v ? v->value : 0, 7);
            co_await g.next();
        }
        lvalue_copies = Counted::copies;
        Counted::reset();
        {
            auto g = a_moved(&rvalue_after);
            auto v = co_await g.next();
            EXPECT_EQ(v ? v->value : 0, 7);
            co_await g.next();
        }
        rvalue_copies = Counted::copies;
    });
    EXPECT_EQ(lvalue_after, 7);
    EXPECT_EQ(lvalue_copies, 1);
    EXPECT_EQ(rvalue_after, -1);
    EXPECT_EQ(rvalue_copies, 0);
}

TEST_F(PromiseOperands, AsyncGeneratorConvertsBracesMoveOnlyAndCopyOnly) {
    std::string      s;
    std::vector<int> braced, assigned;
    int              moved_only = 0, copy_only = 0;
    run([&]() -> task<void> {
        s          = (co_await collect_to_vector(a_converted())).at(0);
        braced     = (co_await collect_to_vector(a_braced_vector())).at(0);
        auto g     = a_move_only();
        auto p     = co_await g.next();
        moved_only = (p && *p) ? **p : 0;
        auto c     = co_await collect_to_vector(a_copy_only());
        copy_only  = c.empty() ? 0 : c[0].v;
        auto m     = a_move_assign_only();
        while (auto v = co_await m.next())
            assigned.push_back(v->v);
    });
    EXPECT_EQ(s, "abc");
    EXPECT_EQ(braced, (std::vector<int>{1, 2, 3}));
    EXPECT_EQ(moved_only, 5);
    EXPECT_EQ(copy_only, 3);
    EXPECT_EQ(assigned, (std::vector<int>{5, 6}));
}

// ---- task<T> ----------------------------------------------------------------------------------------------------

TEST_F(PromiseOperands, TaskMovesALocalAParameterAndAPrvalue) {
    int local = 0, parameter = 0, prvalue = 0, copies = -1;
    run([&]() -> task<void> {
        local     = (co_await t_local()).value;
        parameter = (co_await t_parameter(Counted{8})).value;
        prvalue   = (co_await t_prvalue()).value;
        copies    = Counted::copies;
    });
    EXPECT_EQ(local, 7);
    EXPECT_EQ(parameter, 8);
    EXPECT_EQ(prvalue, 7);
    EXPECT_EQ(copies, 0) << "a moved operand was copied";
}

TEST_F(PromiseOperands, TaskCopiesAnLvalueNothingMovesAndAConstLocal) {
    Counted const source{9};
    int           lvalue = 0, const_local = 0, after_lvalue = -1, after_const = -1;
    run([&]() -> task<void> {
        lvalue       = (co_await t_lvalue(&source)).value;
        after_lvalue = Counted::copies;
        Counted::reset();
        const_local = (co_await t_const_local()).value;
        after_const = Counted::copies;
    });
    EXPECT_EQ(lvalue, 9);
    EXPECT_EQ(source.value, 9);
    EXPECT_EQ(after_lvalue, 1);
    EXPECT_EQ(const_local, 7);
    EXPECT_EQ(after_const, 1);
}

TEST_F(PromiseOperands, TaskConvertsBracesAndPromotes) {
    std::string                 converted, converted_local;
    long                        promoted = 0;
    std::pair<int, std::string> pair;
    std::vector<int>            braced{9}, empty{9};
    std::optional<int>          none{9};
    run([&]() -> task<void> {
        converted       = co_await t_converted();
        converted_local = co_await t_converted_local();
        promoted        = co_await t_promoted();
        pair            = co_await t_braced_pair();
        braced          = co_await t_braced_vector();
        empty           = co_await t_empty_brace();
        none            = co_await t_nullopt();
    });
    EXPECT_EQ(converted, "abc");
    EXPECT_EQ(converted_local, "abc");
    EXPECT_EQ(promoted, 5L);
    EXPECT_EQ(pair, (std::pair<int, std::string>{1, "a"}));
    EXPECT_EQ(braced, (std::vector<int>{1, 2, 3}));
    EXPECT_TRUE(empty.empty());
    EXPECT_FALSE(none.has_value());
}

TEST_F(PromiseOperands, TaskTakesMoveOnlyAndCopyOnlyValues) {
    int prvalue = 0, local = 0, copy_only = 0;
    run([&]() -> task<void> {
        prvalue   = *(co_await t_move_only_prvalue());
        local     = *(co_await t_move_only_local());
        copy_only = (co_await t_copy_only()).v;
    });
    EXPECT_EQ(prvalue, 5);
    EXPECT_EQ(local, 6);
    EXPECT_EQ(copy_only, 3);
}

// ---- operands a reference parameter refuses ---------------------------------------------------------------------

TEST_F(PromiseOperands, AVolatileOperandIsRead) {
    EXPECT_EQ(drain(g_volatile()), (std::vector<int>{3}));
    int async_value = 0, task_value = 0;
    run([&]() -> task<void> {
        {
            auto g      = a_volatile();
            auto v      = co_await g.next();
            async_value = v ? *v : 0;
        }
        task_value = co_await t_volatile();
    });
    EXPECT_EQ(async_value, 4);
    EXPECT_EQ(task_value, 5);
}

TEST_F(PromiseOperands, ACopyConstructorTakingANonConstReferenceCopiesAnLvalue) {
    NonConstCopy src{6};
    int          seen = 0;
    for (auto const &x : g_non_const_copy(&src))
        seen = x.v;
    EXPECT_EQ(seen, 6);
    EXPECT_EQ(src.v, 6) << "the yielded lvalue was moved from";
    EXPECT_EQ(shape_log, "C&M");
    std::string async_log, task_log;
    int         async_value = 0, task_value = 0;
    run([&]() -> task<void> {
        shape_log.clear();
        {
            auto g      = a_non_const_copy(&src);
            auto v      = co_await g.next();
            async_value = v ? v->v : 0;
        }
        async_log = shape_log;
        shape_log.clear();
        task_value = (co_await t_non_const_copy(&src)).v;
        task_log   = shape_log;
    });
    EXPECT_EQ(async_value, 6);
    EXPECT_EQ(async_log, "C&MM");
    EXPECT_EQ(task_value, 6);
    EXPECT_EQ(task_log, "C&MM");
    EXPECT_EQ(src.v, 6);
}

TEST_F(PromiseOperands, AConversionFunctionIsTheOneTheOperandsValueCategorySelects) {
    Converting               c;
    std::vector<std::string> logs;
    for (auto const &x : g_converting(&c)) {
        EXPECT_EQ(x.v, 23);
        logs.push_back(shape_log);
        shape_log.clear();
    }
    EXPECT_EQ(logs, (std::vector<std::string>{"<&>CM", "<&&>Mm"}));
    std::vector<std::string> async_logs;
    std::string              lvalue_log, prvalue_log;
    int                      lvalue = 0, prvalue = 0;
    Converting               d, e;
    run([&]() -> task<void> {
        shape_log.clear();
        {
            auto g = a_converting(&d);
            while (auto v = co_await g.next()) {
                async_logs.push_back(shape_log);
                shape_log.clear();
            }
        }
        shape_log.clear();
        lvalue     = (co_await t_converting_lvalue(&e)).v;
        lvalue_log = shape_log;
        shape_log.clear();
        prvalue     = (co_await t_converting_prvalue()).v;
        prvalue_log = shape_log;
    });
    EXPECT_EQ(async_logs, (std::vector<std::string>{"<&>CMM", "<&&>MmM"}));
    EXPECT_EQ(lvalue, 23);
    EXPECT_EQ(lvalue_log, "<&>CMM");
    EXPECT_EQ(prvalue, 23);
    EXPECT_EQ(prvalue_log, "<&&>MMM");
}

// ---- operands a reference parameter gives another meaning -------------------------------------------------------

template <std::size_t Align>
void
expect_the_forwarding_constructor(PromiseOperands &fixture) {
    Forwarding<Align> src{8};
    int               seen = 0;
    for (auto const &x : g_forwarding(&src))
        seen = x.v;
    EXPECT_EQ(seen, 1008) << "aligned to " << Align;
    EXPECT_EQ(shape_log, "TM") << "aligned to " << Align;
    int               async_value = 0, reference = 0, const_local = 0;
    Forwarding<Align> r{9};
    fixture.run([&]() -> task<void> {
        {
            auto g      = a_forwarding(&src);
            auto v      = co_await g.next();
            async_value = v ? v->v : 0;
        }
        // Named first: g++ 14 cannot type `(co_await e).v` when `e` depends on a template parameter.
        auto const returned_reference = co_await t_forwarding_reference(r);
        reference                     = returned_reference.v;
        auto const returned_local     = co_await t_forwarding_const_local<Align>();
        const_local                   = returned_local.v;
    });
    EXPECT_EQ(async_value, 1008) << "aligned to " << Align;
    EXPECT_EQ(reference, 1009) << "aligned to " << Align;
    EXPECT_EQ(const_local, 1010) << "aligned to " << Align;
}

TEST_F(PromiseOperands, AForwardingConstructorInitialisesTheOperand) {
    expect_the_forwarding_constructor<alignof(int)>(*this);
}

TEST_F(PromiseOperands, AForwardingConstructorInitialisesAnOperandAlignedToTheCacheLine) {
    expect_the_forwarding_constructor<64>(*this);
}

TEST_F(PromiseOperands, AnExplicitMoveConstructorNeverInitialisesTheOperand) {
    for (auto const &x : g_explicit_move())
        EXPECT_EQ(x.v, 11);
    EXPECT_EQ(shape_log, "CM");
    std::string async_log, local_log, rvalue_log;
    int         async_value = 0, local = 0, rvalue = 0;
    run([&]() -> task<void> {
        shape_log.clear();
        {
            auto g      = a_explicit_move();
            auto v      = co_await g.next();
            async_value = v ? v->v : 0;
        }
        async_log = shape_log;
        shape_log.clear();
        local     = (co_await t_explicit_move_local()).v;
        local_log = shape_log;
        shape_log.clear();
        rvalue     = (co_await t_explicit_move_rvalue()).v;
        rvalue_log = shape_log;
    });
    EXPECT_EQ(async_value, 12);
    EXPECT_EQ(async_log, "CMM");
    EXPECT_EQ(local, 13);
    EXPECT_EQ(local_log, "CMC") << "copied into the parameter, moved into the promise, copied out by await_resume";
    EXPECT_EQ(rvalue, 14);
    EXPECT_EQ(rvalue_log, "CMC");
}

TEST_F(PromiseOperands, ASwappingMoveAssignmentLeavesTheOperandMovedFrom) {
    std::vector<int>              after{-9};
    std::vector<std::vector<int>> seen;
    for (auto const &x : g_swapping(&after))
        seen.push_back(x.d);
    EXPECT_EQ(seen, (std::vector<std::vector<int>>{{1}, {2}}));
    // what a moved-from vector holds is unspecified; that it is not the previous value is what the parameter makes sure
    EXPECT_NE(after, (std::vector<int>{1})) << "the operand of the second co_yield std::move(x) got the first value back";
}

TEST_F(PromiseOperands, AOneElementBraceIsWhatEachCompilerBuildsForAParameter) {
    anys const        three{1, 2, 3};
    std::size_t const as_a_parameter       = brace_of_one_as_a_parameter(&three);
    std::size_t const local_as_a_parameter = brace_of_one_local_as_a_parameter();
    // the two things a compiler may build: the initializer_list's vector of one, or a copy of the element
    EXPECT_TRUE(as_a_parameter == 1 || as_a_parameter == 3) << as_a_parameter;
    EXPECT_TRUE(local_as_a_parameter == 1 || local_as_a_parameter == 3) << local_as_a_parameter;
    for (auto const &x : g_brace_of_one(&three))
        EXPECT_EQ(x.size(), as_a_parameter);
    std::size_t async_size = 0, task_size = 0, local_size = 0;
    run([&]() -> task<void> {
        {
            auto g     = a_brace_of_one(&three);
            auto v     = co_await g.next();
            async_size = v ? v->size() : 0;
        }
        task_size  = (co_await t_brace_of_one(&three)).size();
        local_size = (co_await t_brace_of_one_local()).size();
    });
    EXPECT_EQ(async_size, as_a_parameter);
    EXPECT_EQ(task_size, as_a_parameter);
    EXPECT_EQ(local_size, local_as_a_parameter);
}
