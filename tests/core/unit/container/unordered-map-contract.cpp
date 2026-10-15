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
 *
 * 5. **The case-folding wrappers (`icase_unordered_map`, `icase_map`) erase through the folded
 *    key, whatever form the key arrives in** (Huly QB-279), and their iterator and range `erase`
 *    forms erase positions. qbm-http's `Headers::remove_header` is the caller that reached it.
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

// --- 5. The case-folding wrappers erase through the folded key, whatever the key form ----------
// `icase_basic_map` stores every key folded to lower case, so each lookup must fold its argument
// first. Its key `erase` did, but the wrapper also imported the base's whole `erase` overload set
// to expose the iterator forms, and that set includes the base's NON-folding `erase(const Key &)`.
// For a `const std::string` lvalue the folding template deduces the very same parameter type, the
// non-template wins the tie, and the lookup ran case-sensitively against the folded storage:
// `remove_header(name)` with a `const std::string` silently kept the header (Huly QB-279). The
// same import gave the iterator forms to the folding template on the ska map, where `erase(it)`
// did not compile at all.

namespace {

template <typename Map>
[[nodiscard]] Map
icase_fixture() {
    Map map;
    map.emplace("Content-Length", 1);
    map.emplace("X-Other", 2);
    return map;
}

} // namespace

template <typename Map>
class IcaseMapErase : public ::testing::Test {};
using IcaseMaps = ::testing::Types<qb::icase_unordered_map<int>, qb::icase_map<int>>;
TYPED_TEST_SUITE(IcaseMapErase, IcaseMaps);

TYPED_TEST(IcaseMapErase, EveryKeyFormErasesThroughTheFoldedKey) {
    using Map                = TypeParam;
    const auto expect_erased = [](Map &map, std::size_t erased, const char *form) {
        EXPECT_EQ(erased, 1u) << form << ": the key was not found through its folded form";
        EXPECT_FALSE(map.has("content-length")) << form;
        EXPECT_EQ(map.size(), 1u) << form << ": the wrong entry went";
        EXPECT_TRUE(map.has("x-other")) << form;
    };

    {
        Map               map = icase_fixture<Map>();
        const std::string key = "CONTENT-length";
        expect_erased(map, map.erase(key), "const std::string lvalue");
    }
    {
        Map         map = icase_fixture<Map>();
        std::string key = "content-LENGTH";
        expect_erased(map, map.erase(key), "std::string lvalue");
    }
    {
        Map map = icase_fixture<Map>();
        expect_erased(map, map.erase(std::string("Content-Length")), "std::string rvalue");
    }
    {
        Map                    map = icase_fixture<Map>();
        const std::string_view key = "CONTENT-LENGTH";
        expect_erased(map, map.erase(key), "std::string_view");
    }
    {
        Map map = icase_fixture<Map>();
        expect_erased(map, map.erase("Content-length"), "string literal");
    }
    {
        Map               map    = icase_fixture<Map>();
        const std::string absent = "Content-Type";
        EXPECT_EQ(map.erase(absent), 0u) << "an absent key erased something";
        EXPECT_EQ(map.size(), 2u);
    }
}

TYPED_TEST(IcaseMapErase, IteratorAndRangeFormsErasePositionsNotKeys) {
    using Map = TypeParam;
    {
        Map  map = icase_fixture<Map>();
        auto it  = map.find("X-OTHER");
        ASSERT_TRUE(it != map.end());
        map.erase(it); // iterator lvalue
        EXPECT_EQ(map.size(), 1u);
        EXPECT_FALSE(map.has("x-other"));
        map.erase(map.begin()); // iterator rvalue
        EXPECT_TRUE(map.empty());
    }
    {
        Map        map = icase_fixture<Map>();
        const auto it  = std::as_const(map).find("content-length");
        ASSERT_TRUE(it != map.cend());
        map.erase(it); // const_iterator lvalue
        EXPECT_EQ(map.size(), 1u);
        EXPECT_TRUE(map.has("x-other"));
    }
    {
        Map map = icase_fixture<Map>();
        map.erase(map.cbegin(), map.cend()); // range
        EXPECT_TRUE(map.empty());
    }
}

// --- 6. A lookup folds its key on the stack; only an insert builds the folded std::string -------
// `find`, `has`, `at` and `erase(key)` used to call `_Trait::convert` -- a std::string of the folded
// key, heap-allocated past the small-string capacity -- for every lookup. They now fold the key into
// a stack buffer and look that view up through the base map's transparent hash and equality
// (`qb::string_hash` / `std::equal_to<>`, or `std::less<>` for icase_map); only a key longer than the
// buffer, and every INSERT (the stored key must be the folded std::string), still convert. The hash
// values are std::hash<std::string>'s, so buckets and iteration order are what they were.

namespace {

/// `string_to_lower` with counters: how many keys were converted (a std::string built) and how many
/// were folded in place.
struct counting_fold_trait {
    inline static int converts = 0;
    inline static int folds    = 0;

    template <std::size_t N>
    static std::string
    convert(const char (&str)[N]) {
        ++converts;
        return qb::string_to_lower::convert(str);
    }
    static std::string
    convert(std::string const &str) {
        ++converts;
        return qb::string_to_lower::convert(str);
    }
    static std::string
    convert(std::string_view const &str) {
        ++converts;
        return qb::string_to_lower::convert(str);
    }
    static void
    fold(const char *src, char *dst, std::size_t n) noexcept {
        ++folds;
        qb::string_to_lower::fold(src, dst, n);
    }
    static void
    reset() noexcept {
        converts = folds = 0;
    }
};

/// The byte-wise reference the eight-byte fold must agree with.
char
reference_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

} // namespace

template <typename Map>
class IcaseMapLookup : public ::testing::Test {};
using CountingIcaseMaps =
    ::testing::Types<qb::icase_basic_map<qb::unordered_map<std::string, int, qb::string_hash, std::equal_to<>>, counting_fold_trait>,
                     qb::icase_basic_map<std::map<std::string, int, std::less<>>, counting_fold_trait>>;
TYPED_TEST_SUITE(IcaseMapLookup, CountingIcaseMaps);

TYPED_TEST(IcaseMapLookup, LookupsFoldOnTheStackAndInsertsConvert) {
    using Map = TypeParam;
    counting_fold_trait::reset();
    Map map;
    map["Content-Length"] = 1;
    map.emplace("X-Other", 2);
    map.try_emplace(std::string("Transfer-Encoding"), 3);
    EXPECT_EQ(counting_fold_trait::converts, 3) << "every insert builds the folded key";
    EXPECT_EQ(counting_fold_trait::folds, 0);

    const std::string      lvalue = "CONTENT-length";
    const std::string_view view   = "x-OTHER";
    const char            *ptr    = "transfer-ENCODING";
    counting_fold_trait::reset();
    EXPECT_EQ(map.find("content-LENGTH")->second, 1) << "string literal";
    EXPECT_EQ(map.find(lvalue)->second, 1) << "const std::string";
    EXPECT_EQ(map.find(view)->second, 2) << "std::string_view";
    EXPECT_EQ(map.find(ptr)->second, 3) << "const char *";
    EXPECT_TRUE(map.has("CONTENT-LENGTH") && map.has(lvalue) && map.has(view) && map.has(ptr));
    EXPECT_EQ(map.at(lvalue), 1);
    EXPECT_EQ(std::as_const(map).at(ptr), 3);
    EXPECT_TRUE(std::as_const(map).find(view) != map.cend());
    EXPECT_EQ(counting_fold_trait::converts, 0) << "a lookup never builds a std::string";
    EXPECT_EQ(counting_fold_trait::folds, 11) << "one stack fold per lookup";

    counting_fold_trait::reset();
    EXPECT_THROW((void) map.at("absent"), std::out_of_range);
    EXPECT_THROW((void) std::as_const(map).at(std::string("absent")), std::out_of_range);
    EXPECT_EQ(counting_fold_trait::converts, 0);

    counting_fold_trait::reset();
    EXPECT_EQ(map.erase(lvalue), 1u) << "erase folds the const std::string on the stack too";
    EXPECT_EQ(map.erase("absent"), 0u);
    EXPECT_FALSE(map.has("content-length"));
    EXPECT_EQ(map.size(), 2u);
    EXPECT_EQ(counting_fold_trait::converts, 0);
    EXPECT_EQ(counting_fold_trait::folds, 3);
}

TYPED_TEST(IcaseMapLookup, AKeyLongerThanTheStackBufferIsConvertedAndStillFound) {
    using Map = TypeParam;
    Map               map;
    const std::string stored(200, 'K');
    map[stored] = 7;
    counting_fold_trait::reset();
    EXPECT_EQ(map.find(std::string(200, 'k'))->second, 7);
    EXPECT_TRUE(map.has(stored));
    EXPECT_EQ(map.erase(std::string_view(stored)), 1u);
    EXPECT_TRUE(map.empty());
    EXPECT_EQ(counting_fold_trait::converts, 3) << "a 200-byte key does not fit the stack buffer";
    EXPECT_EQ(counting_fold_trait::folds, 0);
}

TEST(StringToLower, FoldAgreesWithTheByteWiseReferenceAtEveryLengthAndWordBoundary) {
    // Every byte value, letters placed on both sides of every eight-byte boundary.
    std::string source;
    for (int b = 0; b < 256; ++b)
        source.push_back(static_cast<char>(b));
    source += "aZ-AzZ@[`{Az";
    for (std::size_t n = 0; n <= 40; ++n) {
        for (std::size_t start = 0; start + n <= source.size() && start < 24; ++start) {
            const std::string in = source.substr(start, n);
            std::string       expected(in);
            for (auto &c : expected)
                c = reference_lower(c);
            std::string out(n, '\0');
            qb::string_to_lower::fold(in.data(), out.data(), n);
            EXPECT_EQ(out, expected) << "into a buffer, n=" << n << " start=" << start;
            std::string in_place(in);
            qb::string_to_lower::fold(in_place.data(), in_place.data(), n);
            EXPECT_EQ(in_place, expected) << "in place, n=" << n << " start=" << start;
        }
    }
    EXPECT_EQ(qb::string_to_lower::convert("Content-Type"), "content-type");
    EXPECT_EQ(qb::string_to_lower::convert(std::string("ABCDEFGHIJKLMNOPQRSTUVWXYZ[\]^_`")), "abcdefghijklmnopqrstuvwxyz[\]^_`");
    EXPECT_EQ(qb::string_to_lower::convert(std::string_view("\x80\xC1\xDA\xFF"
                                                            "AZ")),
              std::string("\x80\xC1\xDA\xFF"
                          "az"));
}

TEST(StringHash, EqualsStdHashOfTheSameBytesSoBucketsAndOrderAreUnchanged) {
    for (const char *key : {"", "a", "content-type", "transfer-encoding", "x-a-rather-long-header-name-past-the-small-string"}) {
        const std::string s(key);
        EXPECT_EQ(qb::string_hash{}(std::string_view(s)), std::hash<std::string>{}(s)) << key;
    }
}

// --- 7. lookup_fold_capacity names the bound of the stack fold -------------------------------------
// A caller that must not allocate -- qbm-http's noexcept Headers::has_header / remove_header -- has
// to know which keys a lookup folds on the stack (Huly QB-1068): exactly those of up to
// lookup_fold_capacity bytes; one byte more converts. A map that converts every key says 0.
static_assert(qb::icase_unordered_map<int>::lookup_fold_capacity == 128);
static_assert(qb::icase_map<int>::lookup_fold_capacity == 128);
static_assert(qb::icase_basic_map<std::map<std::string, int>>::lookup_fold_capacity == 0,
              "std::less<std::string> is not transparent: every lookup converts");

TYPED_TEST(IcaseMapLookup, TheLookupFoldCapacityIsTheLongestKeyFoldedOnTheStack) {
    using Map                 = TypeParam;
    constexpr std::size_t cap = Map::lookup_fold_capacity;
    static_assert(cap > 0);
    Map map;
    map[std::string(cap, 'K')]     = 1;
    map[std::string(cap + 1, 'K')] = 2;

    counting_fold_trait::reset();
    EXPECT_EQ(map.find(std::string(cap, 'k'))->second, 1);
    EXPECT_EQ(counting_fold_trait::converts, 0) << "a key of exactly the capacity folds on the stack";
    EXPECT_EQ(counting_fold_trait::folds, 1);

    counting_fold_trait::reset();
    EXPECT_EQ(map.find(std::string(cap + 1, 'k'))->second, 2);
    EXPECT_EQ(counting_fold_trait::converts, 1) << "one byte more converts";
    EXPECT_EQ(counting_fold_trait::folds, 0);
}
