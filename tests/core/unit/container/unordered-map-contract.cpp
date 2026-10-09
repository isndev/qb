/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/container/unordered-map-contract.cpp
 * @brief The `qb::unordered_map` properties the framework actually leans on, plus the one
 *        place its "drop-in for std::unordered_map" promise used to fail.
 *
 * `qb::unordered_map` is `ska::unordered_map`, unconditionally. Until 3.0.0 the alias resolved
 * to a *different container per build* — ska under `NDEBUG`, `std::unordered_map` otherwise —
 * which made the identity and layout of a public type depend on a build macro and aborted at
 * runtime when a Debug consumer linked a Release libqb. That switch is gone and
 * `qbmModuleConfig.cmake.in` carries a configure-time tripwire against its return, so these
 * tests now pin one container rather than two. (This file's own header claimed the switch was
 * still live until 3.0 — a per-build container is exactly the kind of claim that keeps reading
 * as true long after it stops being.)
 *
 * Three properties are load-bearing, and none was pinned anywhere:
 *
 * 1. **`it = map.erase(it)` while iterating visits every element exactly once.** The HTTP/2 server
 *    walks `_server_streams` and erases as it goes on the GOAWAY path
 *    (`qbm/http/2/protocol/server.h`), and the pattern recurs in the idle-stream sweep and the
 *    pending-flush loops. If the flat map's backward-shift deletion moved a not-yet-visited entry
 *    behind the cursor, streams would be silently skipped — no crash, just a stream that never
 *    gets its error event and never gets reclaimed.
 *
 * 2. **A reference into the map stays valid across a rehash** (node stability). `VirtualCore`
 *    holds `Actor` objects by `unique_ptr` inside the map and hands out raw pointers; the HTTP/2
 *    server takes `Http2ServerStream &` references and keeps using them across calls that can
 *    insert. Iterators are NOT stable across a rehash — only the pointed-to values are — so this
 *    test states both halves explicitly rather than leaving the distinction to folklore.
 *
 * 3. **`contains()` exists and agrees with `find()` and `count()`.** A qb addition to the fork:
 *    C++20 gave every standard associative container a `contains()`, upstream ska predates
 *    C++20, and the alias advertises itself as a drop-in — so its absence was the single point
 *    where that promise failed, and it failed as a compile error naming a template deep inside
 *    the vendored header. The test is a three-way agreement rather than a bare existence check,
 *    because a `contains()` that disagreed with `find()` would be worse than none at all, and it
 *    covers all four aliases: a member added to a shared base reaches map and set together, and
 *    only checking one is how the other silently misses it.
 *
 * 4. **The standard members the fork shipped broken behave as the standard says** (Huly QB-369 to
 *    QB-373). `try_emplace` builds the mapped value from all of its arguments, zero or several,
 *    and constructs nothing on a hit. `merge` moves the absent keys and leaves the present ones in
 *    the source. `bucket(key)` compiles and names a bucket of the table. With an allocator that
 *    is not equal to the source's, the allocator-extended move constructor and the move
 *    assignment re-allocate every element and free nothing through an allocator that did not
 *    allocate it; the move assignment also installs the source's hasher before it re-inserts.
 *    Upstream stole the storage regardless of the allocators, and re-inserted with the old hasher.
 *    qb's own callers (the event router, qbm-http's status handlers) only ever passed one argument
 *    to `try_emplace`, the only form that compiled. Nothing in qb calls `merge` or `bucket`, or uses
 *    a stateful allocator, so none of these defects was reached until now.
 */

#include <cstdint>
#include <gtest/gtest.h>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#include <qb/system/container/unordered_map.h>
#include <qb/system/container/unordered_set.h>

namespace {

/// Reported on a failure so the message names the container under test.
constexpr const char *
config_name() noexcept {
    return "qb::unordered_map -> ska::unordered_map (unconditional since 3.0.0)";
}

} // namespace

TEST(UnorderedMapContract, EraseWhileIteratingVisitsEveryElementExactlyOnce) {
    // Sized to cross several growth/shrink thresholds, since the hazard (if any) lives in the
    // rehash / backward-shift machinery rather than in any single erase.
    for (const int n : {8, 64, 512, 4096}) {
        qb::unordered_map<std::uint32_t, std::uint32_t> m;
        for (int i = 0; i < n; ++i)
            m[static_cast<std::uint32_t>(i)] = static_cast<std::uint32_t>(i);

        std::set<std::uint32_t> visited;
        for (auto it = m.begin(); it != m.end();) {
            ASSERT_TRUE(visited.insert(it->first).second)
                << config_name() << ", n=" << n << ": key " << it->first << " was visited twice — erase() moved an already-seen entry "
                << "back in front of the cursor";
            if (it->first % 2u == 0u)
                it = m.erase(it);
            else
                ++it;
        }

        EXPECT_EQ(visited.size(), static_cast<std::size_t>(n))
            << config_name() << ", n=" << n << ": the walk skipped " << (static_cast<std::size_t>(n) - visited.size())
            << " element(s) — erase() moved an unvisited entry behind the cursor";
        EXPECT_EQ(m.size(), static_cast<std::size_t>(n / 2)) << config_name() << ", n=" << n;
        for (int i = 0; i < n; ++i) {
            const bool present = m.find(static_cast<std::uint32_t>(i)) != m.end();
            EXPECT_EQ(present, (i % 2) != 0) << config_name() << ", n=" << n << ": key " << i << " survived the wrong way";
        }
    }
}

TEST(UnorderedMapContract, ValuesStayPutAcrossARehashEvenThoughIteratorsDoNot) {
    qb::unordered_map<std::uint32_t, std::uint64_t> m;
    m[1] = 0xAAAA;

    // A raw pointer to the mapped value — the shape VirtualCore and the HTTP/2 server both use.
    std::uint64_t *pinned = &m[1];
    *pinned               = 0xBBBB;

    // Force many rehashes.
    for (std::uint32_t i = 2; i < 4096; ++i)
        m[i] = i;

    ASSERT_NE(m.find(1), m.end());
    EXPECT_EQ(m[1], 0xBBBBu);
    EXPECT_EQ(*pinned, 0xBBBBu) << config_name()
                                << ": a reference into the map did NOT survive a rehash. VirtualCore hands out raw "
                                   "Actor* from its actor map and the HTTP/2 server keeps stream references across "
                                   "inserts — both would become use-after-free.";
    EXPECT_EQ(pinned, &m[1]) << config_name() << ": the mapped value moved during rehash";
}

// ---------------------------------------------------------------------------
// contains() — the drop-in promise, on all four aliases
// ---------------------------------------------------------------------------

namespace {

/// Assert the three lookups agree, on any of the four qb aliases. A `contains()` that
/// disagreed with `find()` would be worse than no `contains()` at all, so the property under
/// test is agreement, not existence.
template <typename Map>
void
expect_map_lookups_agree(const char *which) {
    Map m;
    for (int i = 0; i < 64; ++i)
        m[i] = i * 10;

    for (int i = 0; i < 128; ++i) {
        const bool by_find     = m.find(i) != m.end();
        const bool by_count    = m.count(i) != 0;
        const bool by_contains = m.contains(i);
        ASSERT_EQ(by_contains, by_find) << which << ": contains() disagrees with find() for key " << i;
        ASSERT_EQ(by_contains, by_count) << which << ": contains() disagrees with count() for key " << i;
        ASSERT_EQ(by_contains, i < 64) << which << ": wrong answer for key " << i;
    }

    // Erase must be observed by contains() too — a stale positive is the failure that would
    // matter, since it is the spelling most callers will reach for on a liveness check.
    m.erase(7);
    EXPECT_FALSE(m.contains(7)) << which << ": contains() still reports an erased key";
    EXPECT_EQ(m.find(7), m.end()) << which << ": find() still reports an erased key";

    // And on an empty container, where a bad end() comparison would show.
    Map empty;
    EXPECT_FALSE(empty.contains(0)) << which << ": contains() on an empty map";
}

template <typename Set>
void
expect_set_lookups_agree(const char *which) {
    Set s;
    for (int i = 0; i < 64; ++i)
        s.insert(i);

    for (int i = 0; i < 128; ++i) {
        const bool by_find     = s.find(i) != s.end();
        const bool by_contains = s.contains(i);
        ASSERT_EQ(by_contains, by_find) << which << ": contains() disagrees with find() for key " << i;
        ASSERT_EQ(by_contains, i < 64) << which << ": wrong answer for key " << i;
    }

    s.erase(7);
    EXPECT_FALSE(s.contains(7)) << which << ": contains() still reports an erased key";

    Set empty;
    EXPECT_FALSE(empty.contains(0)) << which << ": contains() on an empty set";
}

} // namespace

TEST(UnorderedMapContract, ContainsAgreesWithFindAndCountOnEveryAlias) {
    // Both node-based and flat variants: the member was added once to each vendored base
    // (sherwood_v10_table and sherwood_v3_table), and each base serves a map and a set.
    expect_map_lookups_agree<qb::unordered_map<int, int>>("qb::unordered_map");
    expect_map_lookups_agree<qb::unordered_flat_map<int, int>>("qb::unordered_flat_map");
    expect_set_lookups_agree<qb::unordered_set<int>>("qb::unordered_set");
    expect_set_lookups_agree<qb::unordered_flat_set<int>>("qb::unordered_flat_set");
}

TEST(UnorderedMapContract, ContainsWorksForANonTrivialKey) {
    // std::string keys exercise the hashing/equality path rather than the identity-hash one,
    // which is what most callers actually use (header names, actor service names, route paths).
    qb::unordered_map<std::string, int> m;
    m["alpha"] = 1;
    m["beta"]  = 2;

    EXPECT_TRUE(m.contains("alpha"));
    EXPECT_TRUE(m.contains("beta"));
    EXPECT_FALSE(m.contains("gamma"));
    EXPECT_EQ(m.contains("alpha"), m.find("alpha") != m.end());
    EXPECT_EQ(m.contains("gamma"), m.find("gamma") != m.end());

    qb::unordered_set<std::string> s{"one", "two"};
    EXPECT_TRUE(s.contains("one"));
    EXPECT_FALSE(s.contains("three"));
}

// ---------------------------------------------------------------------------
// try_emplace -- the mapped value from every argument, nothing on a hit (Huly QB-369)
// ---------------------------------------------------------------------------

namespace {

/// A mapped type that needs two constructor arguments, and has no default constructor.
struct two_args {
    int a;
    int b;
    two_args(int x, int y)
        : a(x)
        , b(y) {}
};

/// Counts every construction, copies and moves included.
int g_counted_made = 0;

struct counted {
    int v;
    explicit counted(int x)
        : v(x) {
        ++g_counted_made;
    }
    counted(const counted &o)
        : v(o.v) {
        ++g_counted_made;
    }
    counted(counted &&o) noexcept
        : v(o.v) {
        ++g_counted_made;
    }
    counted &operator=(const counted &) = default;
    counted &operator=(counted &&)      = default;
};

/// Neither copyable nor movable: only a table that builds the pair where it stays can hold it.
struct immovable {
    int v;
    explicit immovable(int x)
        : v(x) {}
    immovable(const immovable &)            = delete;
    immovable &operator=(const immovable &) = delete;
};

/// The two table families, as a type: `Tables::map<K, V>`.
struct node_tables {
    template <typename K, typename V>
    using map = qb::unordered_map<K, V>;
};
struct flat_tables {
    template <typename K, typename V>
    using map = qb::unordered_flat_map<K, V>;
};

template <typename Tables>
void
expect_try_emplace_contract(const char *which) {
    typename Tables::template map<int, int> zero;
    const auto [z, z_inserted] = zero.try_emplace(1);
    EXPECT_TRUE(z_inserted) << which;
    EXPECT_EQ(z->second, 0) << which << ": try_emplace(k) value-initialises the mapped value";

    typename Tables::template map<int, two_args> several;
    const auto [s, s_inserted] = several.try_emplace(2, 3, 4);
    EXPECT_TRUE(s_inserted) << which;
    EXPECT_EQ(s->second.a, 3) << which << ": try_emplace(k, a, b) builds the mapped value from (a, b)";
    EXPECT_EQ(s->second.b, 4) << which;

    typename Tables::template map<int, std::unique_ptr<int>> owning;
    EXPECT_TRUE(owning.try_emplace(5, std::make_unique<int>(50)).second) << which;
    auto keep                  = std::make_unique<int>(51);
    const auto [o, o_inserted] = owning.try_emplace(5, std::move(keep));
    EXPECT_FALSE(o_inserted) << which;
    EXPECT_EQ(*o->second, 50) << which << ": a hit leaves the mapped value alone";
    ASSERT_NE(keep.get(), nullptr) << which << ": a hit must not move from its arguments";
    EXPECT_EQ(*keep, 51) << which;

    typename Tables::template map<int, counted> hits;
    hits.try_emplace(7, 70);
    g_counted_made             = 0;
    const auto [h, h_inserted] = hits.try_emplace(7, 71);
    EXPECT_FALSE(h_inserted) << which;
    EXPECT_EQ(h->second.v, 70) << which;
    EXPECT_EQ(g_counted_made, 0) << which << ": a hit constructed a mapped value";
}

} // namespace

TEST(UnorderedMapContract, TryEmplaceBuildsTheMappedValueFromEveryArgument) {
    // Upstream forwarded `key, args...` to the PAIR's constructor: try_emplace(k) and
    // try_emplace(k, a, b) did not compile, on either table.
    expect_try_emplace_contract<node_tables>("qb::unordered_map");
    expect_try_emplace_contract<flat_tables>("qb::unordered_flat_map");
}

TEST(UnorderedMapContract, TryEmplaceOnTheNodeMapBuildsInPlace) {
    // The node table constructs the pair once, in the node it keeps for the element's lifetime;
    // the flat table builds a pair and moves it into its slot, because robin-hood displacement
    // relocates its elements anyway.
    qb::unordered_map<int, counted> m;
    g_counted_made = 0;
    EXPECT_TRUE(m.try_emplace(1, 10).second);
    EXPECT_EQ(g_counted_made, 1) << "a miss on the node map built the mapped value more than once";

    qb::unordered_map<int, immovable> pinned;
    const auto [it, inserted] = pinned.try_emplace(3, 30);
    EXPECT_TRUE(inserted);
    EXPECT_EQ(it->second.v, 30);
    for (int i = 4; i < 200; ++i) // growth relinks the nodes; it never moves an element
        pinned.try_emplace(i, i * 10);
    ASSERT_EQ(pinned.size(), 197u);
    EXPECT_EQ(pinned.find(3)->second.v, 30);
    EXPECT_EQ(pinned.find(199)->second.v, 1990);
}

// ---------------------------------------------------------------------------
// merge -- absent keys move, present keys stay in the source (Huly QB-372)
// ---------------------------------------------------------------------------

TEST(UnorderedMapContract, MergeMovesTheAbsentKeysAndLeavesThePresentOnes) {
    qb::unordered_map<int, std::string> dst{{1, "one"}};
    qb::unordered_map<int, std::string> src{{1, "uno"}, {2, "two"}, {3, "three"}};

    dst.merge(src);
    EXPECT_EQ(dst.size(), 3u);
    EXPECT_EQ(dst.at(1), "one") << "merge overwrote a key the destination already held";
    EXPECT_EQ(dst.at(2), "two");
    EXPECT_EQ(dst.at(3), "three");
    ASSERT_EQ(src.size(), 1u) << "the merged elements must LEAVE the source (upstream copied them)";
    EXPECT_EQ(src.at(1), "uno") << "the element whose key was already present stays in the source";

    // The rvalue overload has the same contract: the source keeps what was not taken.
    qb::unordered_map<int, std::string> more{{1, "eins"}, {4, "four"}};
    dst.merge(std::move(more));
    EXPECT_EQ(dst.size(), 4u);
    EXPECT_EQ(dst.at(4), "four");
    ASSERT_EQ(more.size(), 1u); // merge(&&) leaves the source valid, holding what it kept
    EXPECT_EQ(more.at(1), "eins");

    // Merging a map into itself changes nothing.
    dst.merge(dst);
    EXPECT_EQ(dst.size(), 4u);
    EXPECT_EQ(dst.at(1), "one");
}

TEST(UnorderedMapContract, MergeMovesAMoveOnlyMappedValue) {
    // Upstream copied through const iterators, in both overloads: neither compiled for a
    // move-only mapped type.
    qb::unordered_map<int, std::unique_ptr<int>> dst;
    dst.emplace(1, std::make_unique<int>(1));
    qb::unordered_map<int, std::unique_ptr<int>> src;
    src.emplace(1, std::make_unique<int>(-1));
    src.emplace(2, std::make_unique<int>(2));
    const int *moved = src.at(2).get();

    dst.merge(src);
    ASSERT_EQ(dst.size(), 2u);
    EXPECT_EQ(dst.at(2).get(), moved) << "the owned object moved, it was not rebuilt";
    EXPECT_EQ(*dst.at(1), 1);
    ASSERT_EQ(src.size(), 1u);
    EXPECT_EQ(*src.at(1), -1);

    qb::unordered_map<int, std::unique_ptr<int>> tail;
    tail.emplace(3, std::make_unique<int>(3));
    dst.merge(std::move(tail));
    EXPECT_EQ(*dst.at(3), 3);
}

// ---------------------------------------------------------------------------
// bucket(key) -- compiles, and names a bucket of the table (Huly QB-373)
// ---------------------------------------------------------------------------

namespace {

template <typename Container>
void
expect_bucket_names_a_bucket(Container &c, const char *which) {
    for (int k = 0; k < 256; ++k) {
        const auto b = c.bucket(k);
        ASSERT_LT(b, c.bucket_count()) << which << ": bucket(" << k << ") is past the table";
        ASSERT_EQ(b, c.bucket(k)) << which << ": bucket(" << k << ") is not a function of the key";
    }
}

} // namespace

TEST(UnorderedMapContract, BucketOfAKeyNamesABucketOfTheTable) {
    // Upstream's node table spelled `.template index_for_hash<0>(...)` against a member that is
    // not a template: the first call to bucket() on a node map or set did not compile. An EMPTY node
    // table counts one bucket while its fibonacci policy (shift 63) spans the sentinel's two slots:
    // bucket(k) answered 1 for half the keys until it answered the empty case itself.
    qb::unordered_map<int, int> node_map;
    expect_bucket_names_a_bucket(node_map, "qb::unordered_map (empty)");
    qb::unordered_set<int> node_set;
    expect_bucket_names_a_bucket(node_set, "qb::unordered_set (empty)");
    for (int i = 0; i < 1000; ++i) {
        node_map[i] = i;
        node_set.insert(i);
    }
    expect_bucket_names_a_bucket(node_map, "qb::unordered_map");
    expect_bucket_names_a_bucket(node_set, "qb::unordered_set");

    // The flat tables were never broken: they are the control.
    qb::unordered_flat_map<int, int> flat_map;
    qb::unordered_flat_set<int>      flat_set;
    for (int i = 0; i < 1000; ++i) {
        flat_map[i] = i;
        flat_set.insert(i);
    }
    expect_bucket_names_a_bucket(flat_map, "qb::unordered_flat_map");
    expect_bucket_names_a_bucket(flat_set, "qb::unordered_flat_set");
}

// ---------------------------------------------------------------------------
// Moves across unequal allocators (Huly QB-370, QB-371)
// ---------------------------------------------------------------------------

namespace {

/// One allocator identity: the allocators of a type compare equal exactly when they share a
/// ledger, so two ledgers are two allocators neither of which may free the other's storage.
struct alloc_ledger {
    std::size_t live = 0;
};

/// Which ledger allocated each live block. A block freed through any other ledger is a foreign
/// free: storage released by an allocator that did not allocate it.
struct alloc_registry {
    std::map<const void *, alloc_ledger *> owner;
    std::size_t                            foreign_frees = 0;
    std::size_t                            unknown_frees = 0;
};

alloc_registry &
registry() {
    static alloc_registry r;
    return r;
}

template <typename T, typename Propagate = std::false_type>
struct ledger_alloc {
    using value_type                             = T;
    using propagate_on_container_move_assignment = Propagate;
    template <typename U>
    struct rebind {
        using other = ledger_alloc<U, Propagate>;
    };

    alloc_ledger *ledger;

    explicit ledger_alloc(alloc_ledger &l) noexcept
        : ledger(&l) {}
    template <typename U>
    ledger_alloc(const ledger_alloc<U, Propagate> &o) noexcept
        : ledger(o.ledger) {}

    T *
    allocate(std::size_t n) {
        auto *p             = static_cast<T *>(::operator new(n * sizeof(T)));
        registry().owner[p] = ledger;
        ++ledger->live;
        return p;
    }
    void
    deallocate(T *p, std::size_t) noexcept {
        auto &reg = registry();
        auto  it  = reg.owner.find(p);
        if (it == reg.owner.end()) {
            ++reg.unknown_frees;
        } else {
            if (it->second != ledger)
                ++reg.foreign_frees;
            --it->second->live;
            reg.owner.erase(it);
        }
        ::operator delete(p);
    }

    friend bool
    operator==(const ledger_alloc &a, const ledger_alloc &b) noexcept {
        return a.ledger == b.ledger;
    }
    friend bool
    operator!=(const ledger_alloc &a, const ledger_alloc &b) noexcept {
        return a.ledger != b.ledger;
    }
};

/// A hasher with state: two seeds place the same key in different buckets.
struct seeded_hash {
    std::uint64_t seed = 0;
    seeded_hash()      = default;
    explicit seeded_hash(std::uint64_t s)
        : seed(s) {}
    std::size_t
    operator()(int k) const noexcept {
        return static_cast<std::size_t>((static_cast<std::uint64_t>(static_cast<std::uint32_t>(k)) + seed) * 0x9E3779B97F4A7C15ull);
    }
};

template <typename Propagate = std::false_type>
using ledger_pair_alloc = ledger_alloc<std::pair<const int, std::string>, Propagate>;

template <typename Propagate = std::false_type>
using ledger_node_map = qb::unordered_map<int, std::string, seeded_hash, std::equal_to<int>, ledger_pair_alloc<Propagate>>;
template <typename Propagate = std::false_type>
using ledger_flat_map = qb::unordered_flat_map<int, std::string, seeded_hash, std::equal_to<int>, ledger_pair_alloc<Propagate>>;

std::string
value_of(int k) {
    return "value-" + std::to_string(k) + "-long-enough-to-leave-the-small-string-buffer";
}

template <typename Map>
void
fill_with_values(Map &m, int n) {
    for (int k = 0; k < n; ++k)
        m.emplace(k, value_of(k));
}

template <typename Map>
void
expect_holds(Map &m, int n, const char *which) {
    ASSERT_EQ(m.size(), static_cast<std::size_t>(n)) << which;
    for (int k = 0; k < n; ++k) {
        const auto it = m.find(k);
        ASSERT_NE(it, m.end()) << which << ": key " << k << " is in the table and find() misses it";
        EXPECT_EQ(it->second, value_of(k)) << which;
    }
}

/// QB-370: the allocator-extended move constructor with an allocator unequal to the source's.
template <typename Map>
void
expect_alloc_extended_move(const char *which) {
    using A    = ledger_pair_alloc<>;
    registry() = {};
    alloc_ledger la, lb;
    {
        Map src(A{la});
        fill_with_values(src, 300);
        Map dst(std::move(src), A{lb});
        expect_holds(dst, 300, which);
        dst.emplace(1000, value_of(1000)); // grows dst through ITS allocator
        src.emplace(2000, value_of(2000)); // the source stays usable, through its own
        EXPECT_EQ(src.count(2000), 1u) << which;
    }
    EXPECT_EQ(registry().foreign_frees, 0u) << which << ": a block was freed by an allocator that did not allocate it";
    EXPECT_EQ(registry().unknown_frees, 0u) << which;
    EXPECT_EQ(la.live, 0u) << which;
    EXPECT_EQ(lb.live, 0u) << which;

    // Control: with an EQUAL allocator the storage is stolen -- no block allocated, none freed.
    registry() = {};
    {
        Map src(A{la});
        fill_with_values(src, 300);
        const std::size_t before = la.live;
        Map               dst(std::move(src), A{la});
        EXPECT_EQ(la.live, before) << which << ": an equal-allocator move must steal, not rebuild";
        expect_holds(dst, 300, which);
    }
    EXPECT_EQ(registry().foreign_frees, 0u) << which;
    EXPECT_EQ(la.live, 0u) << which;
}

/// QB-371: the move assignment between unequal, non-propagating allocators, with hashers that
/// place the keys differently.
template <template <typename> class MapOf>
void
expect_move_assignment(const char *which) {
    using A    = ledger_pair_alloc<>;
    registry() = {};
    alloc_ledger la, lb;
    {
        MapOf<std::false_type> dst(0, seeded_hash{1}, std::equal_to<int>{}, A{la});
        dst.emplace(9999, value_of(9999));
        MapOf<std::false_type> src(0, seeded_hash{2}, std::equal_to<int>{}, A{lb});
        fill_with_values(src, 300);

        dst = std::move(src);
        EXPECT_EQ(dst.hash_function().seed, 2u) << which << ": the source's hasher was not installed";
        expect_holds(dst, 300, which);
        EXPECT_EQ(dst.count(9999), 0u) << which;
        for (int k = 0; k < 300; ++k)
            EXPECT_EQ(dst.erase(k), 1u) << which << ": erase(" << k << ") missed a key the table holds";
        EXPECT_TRUE(dst.empty()) << which;
    }
    EXPECT_EQ(registry().foreign_frees, 0u) << which;
    EXPECT_EQ(registry().unknown_frees, 0u) << which;
    EXPECT_EQ(la.live, 0u) << which;
    EXPECT_EQ(lb.live, 0u) << which;

    // Control 1: equal allocators -- the storage is swapped, never rebuilt.
    registry() = {};
    {
        MapOf<std::false_type> dst(0, seeded_hash{1}, std::equal_to<int>{}, A{la});
        MapOf<std::false_type> src(0, seeded_hash{2}, std::equal_to<int>{}, A{la});
        fill_with_values(src, 300);
        const std::size_t before = la.live;
        dst                      = std::move(src);
        EXPECT_EQ(la.live, before) << which << ": an equal-allocator move assignment allocated or freed";
        expect_holds(dst, 300, which);
    }
    EXPECT_EQ(registry().foreign_frees, 0u) << which;

    // Control 2: a propagating allocator travels with the storage.
    using P    = ledger_pair_alloc<std::true_type>;
    registry() = {};
    {
        MapOf<std::true_type> dst(0, seeded_hash{1}, std::equal_to<int>{}, P{la});
        dst.emplace(9999, value_of(9999));
        MapOf<std::true_type> src(0, seeded_hash{2}, std::equal_to<int>{}, P{lb});
        fill_with_values(src, 300);
        dst = std::move(src);
        expect_holds(dst, 300, which);
    }
    EXPECT_EQ(registry().foreign_frees, 0u) << which;
    EXPECT_EQ(la.live, 0u) << which;
    EXPECT_EQ(lb.live, 0u) << which;
}

template <typename P>
using node_map_of = ledger_node_map<P>;
template <typename P>
using flat_map_of = ledger_flat_map<P>;

// noexcept tells the truth: the steal cannot throw, the element-wise path allocates.
using std_node  = qb::unordered_map<int, int>;
using std_flat  = qb::unordered_flat_map<int, int>;
using std_alloc = std::allocator<std::pair<const int, int>>;
static_assert(std::is_nothrow_move_constructible_v<std_node> && std::is_nothrow_move_assignable_v<std_node>);
static_assert(std::is_nothrow_move_constructible_v<std_flat> && std::is_nothrow_move_assignable_v<std_flat>);
static_assert(std::is_nothrow_constructible_v<std_node, std_node &&, const std_alloc &>);
static_assert(std::is_nothrow_constructible_v<std_flat, std_flat &&, const std_alloc &>);
static_assert(!std::is_nothrow_move_assignable_v<ledger_node_map<>>, "unequal non-propagating allocators re-allocate");
static_assert(!std::is_nothrow_move_assignable_v<ledger_flat_map<>>, "unequal non-propagating allocators re-allocate");
static_assert(std::is_nothrow_move_assignable_v<ledger_node_map<std::true_type>>, "a propagating allocator only swaps");
static_assert(std::is_nothrow_move_assignable_v<ledger_flat_map<std::true_type>>, "a propagating allocator only swaps");
static_assert(!std::is_nothrow_constructible_v<ledger_node_map<>, ledger_node_map<> &&, const ledger_pair_alloc<> &>);
static_assert(!std::is_nothrow_constructible_v<ledger_flat_map<>, ledger_flat_map<> &&, const ledger_pair_alloc<> &>);

} // namespace

TEST(UnorderedMapContract, AnAllocatorExtendedMoveFreesEveryBlockThroughItsAllocator) {
    // Upstream stole the source's storage whatever the allocators, and the destination later
    // freed, through ITS allocator, blocks the source's allocator had made.
    expect_alloc_extended_move<ledger_node_map<>>("qb::unordered_map");
    expect_alloc_extended_move<ledger_flat_map<>>("qb::unordered_flat_map");
}

TEST(UnorderedMapContract, AMoveAssignmentAcrossUnequalAllocatorsKeepsEveryKeyFindable) {
    // Upstream re-inserted the elements with the destination's OLD hasher and installed the
    // source's afterwards: every key sat in the bucket of a hash the table no longer computed.
    expect_move_assignment<node_map_of>("qb::unordered_map");
    expect_move_assignment<flat_map_of>("qb::unordered_flat_map");
}
