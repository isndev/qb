/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/container/ring-buffer.cpp
 * @brief `qb::ring_buffer` — the fixed-capacity circular FIFO, and what a full ring does when it evicts.
 *
 * The container had no test at all until 3.3. What is pinned:
 *
 *   - **A full ring builds the replacement before it evicts (Huly QB-278).** It used to destroy the oldest
 *     element first and construct the new one in its slot. Two defects followed: a constructor that threw
 *     left a dead slot counted live — destroyed a SECOND time by `clear()` or the destructor — and
 *     `push_back(front())`, or any value naming the oldest element, was read after its destructor had run.
 *     `FullRingSurvivesAThrowingConstructor` and `PushingTheOldestElementCopiesItWhileAlive` are those two;
 *     both fail on the 3.2 code (a live count of N - 1 and a double destruction; a copy from a destroyed
 *     object, which ASan reports as heap-use-after-free on the `std::string` case).
 *   - **A move that may throw evicts first** and a throw leaves `capacity() - 1` live elements — the documented
 *     fallback, held by `ThrowingMoveEvictsFirstAndLeavesNMinusOneLive`.
 *   - The contract around it: a push into a ring that is not full is untouched by a throwing constructor,
 *     `Overwrite = false` discards at capacity, a trivially copyable window keeps the newest N in order across
 *     the wrap (the example's shape), `clear()` destroys exactly what is held, and an immovable `T` still builds
 *     in place.
 *
 * Liveness is tracked by ADDRESS in a registry, not by a sentinel the destructor writes: a store into an object
 * whose lifetime ends is dead to the optimiser (GCC's lifetime DSE removes it), so a sentinel would make the
 * witness pass on the broken code in a release build.
 */

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <qb/system/container/ring_buffer.h>

namespace ring_buffer_test {

// --- the liveness registry -----------------------------------------------------------------------------------

std::vector<void const *> g_live;             ///< every tracked object alive right now, by address
int                       g_double_destroy{}; ///< a destructor run on an address that was not alive
int                       g_read_dead{};      ///< a copy or move whose SOURCE was not alive
bool                      g_throw_from_int{}; ///< arm the value constructor to throw
bool                      g_throw_on_move{};  ///< arm MayThrowMove's move constructor to throw

bool
is_live(void const *p) {
    return std::find(g_live.begin(), g_live.end(), p) != g_live.end();
}
void
born(void const *p) {
    g_live.push_back(p);
}
void
died(void const *p) {
    const auto it = std::find(g_live.begin(), g_live.end(), p);
    if (it == g_live.end())
        ++g_double_destroy;
    else
        g_live.erase(it);
}
void
reset_registry() {
    g_live.clear();
    g_double_destroy = 0;
    g_read_dead      = 0;
    g_throw_from_int = false;
    g_throw_on_move  = false;
}

/// Copyable, nothrow-movable: the ordinary element type.
struct Tracked {
    int value{0};

    explicit Tracked(int v)
        : value(v) {
        if (g_throw_from_int)
            throw std::runtime_error("Tracked(int) armed to throw");
        born(this);
    }
    Tracked(Tracked const &o)
        : value(o.value) {
        if (!is_live(&o))
            ++g_read_dead;
        born(this);
    }
    Tracked(Tracked &&o) noexcept
        : value(o.value) {
        if (!is_live(&o))
            ++g_read_dead;
        born(this);
    }
    Tracked &operator=(Tracked const &) = delete;
    Tracked &operator=(Tracked &&)      = delete;
    ~Tracked() {
        died(this);
    }
};

/// Copyable, with a move constructor that may throw (not noexcept): the evict-first fallback.
struct MayThrowMove {
    int value{0};

    explicit MayThrowMove(int v)
        : value(v) {
        born(this);
    }
    MayThrowMove(MayThrowMove const &o)
        : value(o.value) {
        born(this);
    }
    MayThrowMove(MayThrowMove &&o) // deliberately not noexcept
        : value(o.value) {
        if (g_throw_on_move)
            throw std::runtime_error("MayThrowMove(&&) armed to throw");
        born(this);
    }
    MayThrowMove &operator=(MayThrowMove const &) = delete;
    MayThrowMove &operator=(MayThrowMove &&)      = delete;
    ~MayThrowMove() {
        died(this);
    }
};

/// Neither copyable nor movable: can only be built in place.
struct Immovable {
    int value{0};

    explicit Immovable(int v)
        : value(v) {
        born(this);
    }
    Immovable(Immovable const &)            = delete;
    Immovable &operator=(Immovable const &) = delete;
    ~Immovable() {
        died(this);
    }
};

template <typename Ring>
std::vector<int>
values_of(Ring const &ring) {
    std::vector<int> out;
    for (auto it = ring.cbegin(); it != ring.cend(); ++it)
        out.push_back(it->value);
    return out;
}

// --- the eviction (Huly QB-278) --------------------------------------------------------------------------------

TEST(RingBuffer, FullRingSurvivesAThrowingConstructor) {
    reset_registry();
    {
        qb::ring_buffer<Tracked, 4> ring;
        for (int i = 0; i < 4; ++i)
            ring.push_back(i);
        ASSERT_TRUE(ring.full());

        g_throw_from_int = true;
        EXPECT_THROW(ring.push_back(99), std::runtime_error);
        g_throw_from_int = false;

        EXPECT_EQ(ring.size(), 4u);
        EXPECT_EQ(g_live.size(), 4u) << "every slot the ring counts must hold a live element after the throw";
        EXPECT_EQ(values_of(ring), (std::vector<int>{0, 1, 2, 3}))
            << "a throwing constructor must leave the full ring untouched — the oldest element included";

        ring.push_back(4); // the ring still works: 0 is evicted now
        EXPECT_EQ(values_of(ring), (std::vector<int>{1, 2, 3, 4}));
    }
    EXPECT_EQ(g_double_destroy, 0) << "a slot destroyed by the eviction was destroyed again by the ring's teardown";
    EXPECT_TRUE(g_live.empty()) << g_live.size() << " element(s) never destroyed";
}

TEST(RingBuffer, PushingTheOldestElementCopiesItWhileAlive) {
    reset_registry();
    {
        qb::ring_buffer<Tracked, 3> ring;
        for (int i = 10; i < 13; ++i)
            ring.push_back(i);
        ASSERT_TRUE(ring.full());

        ring.push_back(ring.front()); // the value names the very element the push evicts
        EXPECT_EQ(g_read_dead, 0) << "push_back(front()) copied the oldest element after its destructor had run";
        EXPECT_EQ(values_of(ring), (std::vector<int>{11, 12, 10}));
    }
    EXPECT_EQ(g_double_destroy, 0);
    EXPECT_TRUE(g_live.empty());

    // The realistic shape: a heap-backed element. On the 3.2 code ASan reports heap-use-after-free here.
    qb::ring_buffer<std::string, 3> words;
    const std::string               oldest(64, 'a'); // longer than any small-string buffer
    words.push_back(oldest);
    words.push_back(std::string(64, 'b'));
    words.push_back(std::string(64, 'c'));
    words.push_back(words.front());
    EXPECT_EQ(words.back(), oldest);
    EXPECT_EQ(words.front(), std::string(64, 'b'));
    EXPECT_EQ(words.size(), 3u);
}

TEST(RingBuffer, ThrowingMoveEvictsFirstAndLeavesNMinusOneLive) {
    reset_registry();
    {
        qb::ring_buffer<MayThrowMove, 3> ring;
        for (int i = 0; i < 3; ++i)
            ring.push_back(i);
        ASSERT_TRUE(ring.full());

        g_throw_on_move = true;
        EXPECT_THROW(ring.push_back(7), std::runtime_error);
        g_throw_on_move = false;

        EXPECT_EQ(ring.size(), 2u) << "the documented fallback: the oldest element went first";
        EXPECT_EQ(g_live.size(), 2u) << "every counted slot holds a live element";
        EXPECT_EQ(values_of(ring), (std::vector<int>{1, 2}));

        ring.push_back(8);
        EXPECT_EQ(values_of(ring), (std::vector<int>{1, 2, 8}));
    }
    EXPECT_EQ(g_double_destroy, 0);
    EXPECT_TRUE(g_live.empty());
}

// --- the contract around it ------------------------------------------------------------------------------------

TEST(RingBuffer, NotFullPushIsUntouchedByAThrowingConstructor) {
    reset_registry();
    {
        qb::ring_buffer<Tracked, 4> ring;
        ring.push_back(1);
        ring.push_back(2);
        g_throw_from_int = true;
        EXPECT_THROW(ring.push_back(3), std::runtime_error);
        g_throw_from_int = false;
        EXPECT_EQ(ring.size(), 2u);
        EXPECT_EQ(values_of(ring), (std::vector<int>{1, 2}));
        EXPECT_EQ(g_live.size(), 2u);
    }
    EXPECT_EQ(g_double_destroy, 0);
    EXPECT_TRUE(g_live.empty());
}

TEST(RingBuffer, OverwriteFalseDiscardsAtCapacity) {
    reset_registry();
    {
        qb::ring_buffer<Tracked, 2, false> ring;
        ring.push_back(1);
        ring.push_back(2);
        ring.push_back(3); // discarded: the ring keeps what it has
        EXPECT_EQ(values_of(ring), (std::vector<int>{1, 2}));
        EXPECT_EQ(g_live.size(), 2u);
    }
    EXPECT_TRUE(g_live.empty());
}

TEST(RingBuffer, TrivialWindowKeepsTheNewestNInOrderAcrossTheWrap) {
    qb::ring_buffer<std::uint64_t, 4> window; // the example's shape: a sliding window of samples
    for (std::uint64_t i = 1; i <= 11; ++i) {
        window.push_back(i);
        EXPECT_EQ(window.back(), i);
        EXPECT_EQ(window.size(), std::min<std::uint64_t>(i, 4));
    }
    std::vector<std::uint64_t> held(window.cbegin(), window.cend());
    EXPECT_EQ(held, (std::vector<std::uint64_t>{8, 9, 10, 11}));
    EXPECT_EQ(window.front(), 8u);

    window.push_back(window.front()); // aliasing the oldest slot: trivially copyable, still well-defined
    held.assign(window.cbegin(), window.cend());
    EXPECT_EQ(held, (std::vector<std::uint64_t>{9, 10, 11, 8}));

    window.pop_front();
    EXPECT_EQ(window.size(), 3u);
    EXPECT_EQ(window.front(), 10u);
}

TEST(RingBuffer, ClearDestroysExactlyWhatIsHeld) {
    reset_registry();
    qb::ring_buffer<Tracked, 3> ring;
    for (int i = 0; i < 7; ++i) // wraps twice
        ring.push_back(i);
    EXPECT_EQ(g_live.size(), 3u);
    ring.clear();
    EXPECT_TRUE(ring.empty());
    EXPECT_TRUE(g_live.empty());
    EXPECT_EQ(g_double_destroy, 0);
    ring.push_back(42);
    EXPECT_EQ(values_of(ring), (std::vector<int>{42}));
}

TEST(RingBuffer, ImmovableTypeStillBuildsInPlace) {
    reset_registry();
    {
        qb::ring_buffer<Immovable, 2> ring;
        for (int i = 0; i < 5; ++i)
            ring.push_back(i);
        EXPECT_EQ(values_of(ring), (std::vector<int>{3, 4}));
        EXPECT_EQ(g_live.size(), 2u);
    }
    EXPECT_EQ(g_double_destroy, 0);
    EXPECT_TRUE(g_live.empty());
}

} // namespace ring_buffer_test
