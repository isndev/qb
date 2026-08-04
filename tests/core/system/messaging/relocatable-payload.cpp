/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/messaging/relocatable-payload.cpp
 * @brief Cross-core delivery relocates events with memcpy — pin what that requires of a payload.
 *
 * The cross-core transport is byte-based end to end and never runs a destructor on the source:
 *
 *   1. `push`/`send` placement-new the event inside the sender's outbound pipe (address A);
 *   2. `try_send` → `spsc::enqueue<_All=true>` **memcpy**s the whole bucket run into the peer's
 *      MPSC ring (address B), splitting it in two segments when it straddles the ring wrap;
 *   3. the sender rewinds its pipe cursor (`pipe::reset()` / `pipe::free()`) — **no destructor**;
 *   4. the consumer **memcpy**s the batch out of the ring into `_event_buffer` (address C);
 *   5. the handler runs on the copy at C, and `router::dispose` destroys it there.
 *
 * So every cross-core event is relocated twice and destroyed at an address it was never
 * constructed at. That is only correct for a **trivially relocatable** payload — one holding no
 * pointer into itself. `std::vector`, `std::unique_ptr`, `std::shared_ptr`, `qb::string<N>` and
 * every POD qualify. A libstdc++ **short** `std::string` does NOT: its `_M_p` points at its own
 * inline `_M_local_buf`, so after the memcpy it still addresses the *sender's* pipe buffer —
 * the handler reads freed/reused memory, and `~basic_string()` then calls `operator delete` on a
 * pointer that never came from the heap.
 *
 * libc++ recomputes a short string's `data()` from `this`, so macOS cannot observe any of this:
 * the trap is structurally invisible on the development platform and fires only on the primary
 * deployment one. Hence this file asserts the *behaviour* (bytes survive the hop) rather than a
 * platform-specific layout, so it fails loudly wherever relocation is unsound.
 *
 * Same-core delivery is NOT affected: `__receive__` swaps the mono pipe and routes the event in
 * place, so nothing is ever relocated. Both are exercised.
 *
 * COVERAGE OF THE DEBUG GUARD (`SharedCoreCommunication::send`, `src/Main.cpp`) — what the death
 * tests at the bottom do and do not prove, because a guard nothing exercises is indistinguishable
 * from a guard that does not work:
 *
 *   - positive control, EVERY stdlib: `SelfReferentialEventIsRejectedCrossCoreInDebug` pushes an
 *     event with an explicit interior cursor and requires the process to die *with the guard's own
 *     diagnostic* — not merely to die, which any unrelated crash would satisfy;
 *   - negative control, EVERY stdlib: `CrossCorePayloadSurvivesMemcpyRelocation` drives the
 *     sanctioned payload set through the same guarded path in-process, so a guard that fired on a
 *     safe payload would abort the runner;
 *   - the REAL-WORLD shape (a by-value short `std::string`) can only be rejected where the stdlib
 *     actually makes it self-referential. On libstdc++ the death test covers it; on libc++ there is
 *     nothing to reject, and `ShortStdStringEventCrossesCoresWithoutTrippingTheGuard` asserts the
 *     complement instead — that the guard does not false-positive on it.
 *
 * All of that is Debug-only: the guard is `#ifndef NDEBUG`, so a Release build runs the five
 * relocation tests and none of the guard tests.
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <future>
#include <gtest/gtest.h>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>
#include <qb/actor.h>
#include <qb/main.h>
#include <qb/string.h>

namespace {

constexpr const char *kShort = "short-sso"; ///< <= 15 chars: inside every SSO buffer.

std::atomic<int> g_ok{0};
std::atomic<int> g_bad{0};
std::atomic<int> g_seen{0};

/// Every member here must survive a raw byte relocation.
struct RelocEvent : public qb::Event {
    qb::string<32>               inline_str; ///< the framework's own heap-free string
    std::vector<int>             vec;        ///< heap-owned: no self-pointer
    std::shared_ptr<std::string> boxed;      ///< heap-owned: no self-pointer

    RelocEvent()
        : inline_str(kShort)
        , vec{1, 2, 3, 4, 5}
        , boxed(std::make_shared<std::string>(kShort)) {}
};

class RelocRecv final : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<RelocEvent>(*this);
        co_return true;
    }
    void
    on(RelocEvent const &e) {
        g_seen.fetch_add(1, std::memory_order_relaxed);
        const bool ok =
            std::strcmp(e.inline_str.c_str(), kShort) == 0 && e.vec.size() == 5u && e.vec[4] == 5 && e.boxed != nullptr && *e.boxed == kShort;
        (ok ? g_ok : g_bad).fetch_add(1, std::memory_order_relaxed);
        kill();
    }
};

class RelocSend final : public qb::Actor {
    const qb::ActorId _to;

public:
    explicit RelocSend(qb::ActorId to)
        : _to(to) {}
    qb::io::async::task<bool>
    onInit() override {
        push<RelocEvent>(_to);
        kill();
        co_return true;
    }
};

[[nodiscard]] bool
run(qb::CoreId sender_core, qb::CoreId receiver_core, std::chrono::seconds budget) {
    g_ok = g_bad = g_seen = 0;
    auto done             = std::make_shared<std::promise<void>>();
    auto future           = done->get_future();
    std::thread([sender_core, receiver_core, done] {
        qb::Main main;
        auto     rcv = main.addActor<RelocRecv>(receiver_core);
        main.addActor<RelocSend>(sender_core, rcv);
        main.start();
        main.join();
        done->set_value();
    }).detach();
    return future.wait_for(budget) == std::future_status::ready;
}

} // namespace

// Control: same-core routes in place, so relocation is not involved at all.
TEST(RelocatablePayload, SameCorePayloadArrivesIntact) {
    ASSERT_TRUE(run(0, 0, std::chrono::seconds(30)));
    EXPECT_EQ(g_seen.load(), 1);
    EXPECT_EQ(g_bad.load(), 0);
    EXPECT_EQ(g_ok.load(), 1);
}

// The real subject: two memcpy relocations and a destructor at a third address. In a Debug build
// this is also the debug guard's negative control — the sanctioned payload goes through the guarded
// path in THIS process, so a guard that fired on a safe event would abort the runner here.
TEST(RelocatablePayload, CrossCorePayloadSurvivesMemcpyRelocation) {
    if (std::thread::hardware_concurrency() < 2u)
        GTEST_SKIP() << "requires-multicore: relocation only happens on the cross-core path";

    ASSERT_TRUE(run(0, 1, std::chrono::seconds(30)));
    EXPECT_EQ(g_seen.load(), 1);
    EXPECT_EQ(g_bad.load(), 0) << "a payload member did not survive the cross-core memcpy relocation: it holds a "
                                  "pointer into itself, so after the hop it still addresses the sender's pipe";
    EXPECT_EQ(g_ok.load(), 1);
}

// --- Self-reference detector -----------------------------------------------------------------
// The property the transport actually requires, checked directly on a value so it holds on every
// platform and in Release too. This is a TYPE-level probe, not the shipped guard: it decides the
// same question by the same rule, but the guard itself lives in an anonymous namespace in
// `src/Main.cpp` and is exercised end to end by the death tests at the bottom of this file.
//
// Deliberately NOT routed through a live `qb::Main` in Release: with the debug guard compiled out,
// delivering a self-referential payload cross-core makes the receiver's destructor free a pointer
// that never came from the heap, which aborts the process under ASan.
namespace {

/// True iff a freshly built `T` contains a pointer-sized word addressing its own storage — i.e.
/// the value would dangle after the engine memcpy-relocates it to another address.
template <typename T, typename Factory>
[[nodiscard]] bool
is_self_referential(Factory make) {
    // Zeroed: any padding byte the constructor does not write would otherwise hold whatever this
    // stack frame held before, and a stale word landing inside `storage` reads as a false positive.
    // `make()` returns a `T` prvalue, so guaranteed elision builds the object directly in here —
    // no copy constructor runs, hence nothing can drag indeterminate padding back in.
    alignas(T) unsigned char storage[sizeof(T)]{};
    auto *const              obj  = new (storage) T(make());
    const auto *const        base = reinterpret_cast<const unsigned char *>(obj);
    const auto               lo   = reinterpret_cast<std::uintptr_t>(base);
    const auto               hi   = lo + sizeof(T);

    bool found = false;
    // Same stride and same read-into-an-integer as the shipped guard: words are copied into a
    // `uintptr_t`, so no pointer is ever *formed* from padding bytes.
    for (std::size_t off = 0; off + sizeof(std::uintptr_t) <= sizeof(T); off += alignof(std::uintptr_t)) {
        // Read through `volatile unsigned char` rather than memcpy'ing the word directly. The
        // placement-new above starts T's lifetime, which makes every byte the constructor does not
        // write INDETERMINATE again -- the zero-init before it no longer counts. For a
        // `qb::string<32>` that is the whole unused tail, and gcc-14 rightly reports
        // `-Wmaybe-uninitialized` on a `uintptr_t` pulled straight out of it. Byte-wise access
        // through a volatile lvalue of narrow character type is the well-defined way to inspect
        // such storage, and scanning the padding is the POINT here: the shipped guard in
        // `SharedCoreCommunication::send` looks at the same bytes.
        std::uintptr_t word = 0;
        for (std::size_t b = 0; b < sizeof(word); ++b) {
            const auto byte = static_cast<const volatile unsigned char *>(base)[off + b];
            word |= static_cast<std::uintptr_t>(byte) << (b * 8u); // little-endian, matching memcpy
        }
        if (word >= lo && word < hi) {
            found = true;
            break;
        }
    }
    obj->~T();
    return found;
}

} // namespace

// Every payload type the framework sanctions for events must survive a raw byte relocation.
TEST(RelocatablePayload, SanctionedPayloadTypesHoldNoPointerIntoThemselves) {
    EXPECT_FALSE(is_self_referential<qb::string<32>>([] { return qb::string<32>(kShort); }))
        << "qb::string<N> is the sanctioned inline string for events precisely because it stores "
           "its characters with no pointer at all";
    EXPECT_FALSE(is_self_referential<std::vector<int>>([] { return std::vector<int>{1, 2, 3}; }));
    EXPECT_FALSE(is_self_referential<std::shared_ptr<std::string>>([] { return std::make_shared<std::string>(kShort); }));
    EXPECT_FALSE(is_self_referential<std::unique_ptr<int>>([] { return std::make_unique<int>(7); }));
}

// The trap, characterised rather than asserted: a SHORT std::string is self-referential on
// libstdc++ (its `_M_p` addresses its own `_M_local_buf`) and not on libc++ (which recomputes
// `data()` from `this`). Putting one in an event and pushing it CROSS-CORE therefore corrupts
// the heap on Linux while passing every macOS test — which is exactly why it went unnoticed.
// The answer is `qb::string<N>` or heap-owned data behind a pointer; never a by-value
// std::string. This test records the platform's answer instead of asserting one, so it stays
// green everywhere while keeping the fact visible in the test log.
TEST(RelocatablePayload, ShortStdStringSelfReferenceIsPlatformDependent) {
    const bool self_ref = is_self_referential<std::string>([] { return std::string(kShort); });
    RecordProperty("short_std_string_is_self_referential", self_ref ? "yes" : "no");
    std::printf("[relocation] short std::string is self-referential on this stdlib: %s\n", self_ref ? "YES (unsafe in an event)" : "no");
    // A LONG string is heap-backed on every implementation, hence always relocatable.
    EXPECT_FALSE(is_self_referential<std::string>([] { return std::string(512, 'L'); }))
        << "a heap-backed string must never be self-referential";
}

#ifndef NDEBUG
// --- The debug guard ---------------------------------------------------------------------------
// Debug builds refuse to relocate a self-referential event instead of silently corrupting the
// peer's heap. This is what restores visibility on macOS, where libc++ makes the underlying
// defect unobservable. Death-tested so the abort is exercised without killing this process.
namespace {

/// Substring of the guard's own `assert` expression (`SharedCoreCommunication::send`, `src/Main.cpp`).
/// `assert` stringises its argument onto **stderr**, which is exactly what `EXPECT_DEATH` matches;
/// the guard's companion `QB_LOG_CRIT` cannot serve here — it writes to stdout and compiles to a no-op
/// unless logging is enabled. Matching this text instead of `""` is the whole point of the death
/// test: `""` accepts a death from ANY cause, so it cannot tell the guard firing apart from an
/// unrelated crash on the same path. If the guard ever stops asserting, or stops saying why, these
/// tests fail loudly instead of passing on someone else's abort.
constexpr const char *kGuardDiagnostic = "not trivially relocatable";

/// Drive one whole `qb::Main` to completion on a detached thread and report whether it finished
/// inside @p budget — same shape as `run()` above, so a wedged engine fails the test instead of
/// hanging the ctest slot.
[[nodiscard]] bool
run_engine(void (*body)(), std::chrono::seconds budget) {
    auto done   = std::make_shared<std::promise<void>>();
    auto future = done->get_future();
    std::thread([body, done] {
        body();
        done->set_value();
    }).detach();
    return future.wait_for(budget) == std::future_status::ready;
}

struct SelfRefEvent : public qb::Event {
    char  buf[32]{};
    char *cursor; ///< points INTO this object: exactly what memcpy relocation cannot preserve
    SelfRefEvent()
        : cursor(buf) {
        std::strcpy(buf, kShort);
    }
};

class SelfRefRecv final : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<SelfRefEvent>(*this);
        co_return true;
    }
    void
    on(SelfRefEvent const &) {
        kill();
    }
};

class SelfRefSend final : public qb::Actor {
    const qb::ActorId _to;

public:
    explicit SelfRefSend(qb::ActorId to)
        : _to(to) {}
    qb::io::async::task<bool>
    onInit() override {
        push<SelfRefEvent>(_to);
        kill();
        co_return true;
    }
};

void
push_self_referential_cross_core() {
    qb::Main main;
    auto     rcv = main.addActor<SelfRefRecv>(1);
    main.addActor<SelfRefSend>(0, rcv);
    main.start();
    main.join();
}

} // namespace

// The guard's positive control, and the only one that runs on EVERY platform: an explicit interior
// cursor is self-referential whatever the standard library does. Requiring the guard's own
// diagnostic — not merely a death — is what makes this a test of the guard rather than a test that
// something, somewhere, crashed.
TEST(RelocatablePayloadDeathTest, SelfReferentialEventIsRejectedCrossCoreInDebug) {
    if (std::thread::hardware_concurrency() < 2u)
        GTEST_SKIP() << "requires-multicore: the guard sits on the cross-core relocation path";
    // The statement starts a whole engine, so the child must re-exec rather than fork a process
    // that already holds core threads.
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    EXPECT_DEATH(push_self_referential_cross_core(), kGuardDiagnostic)
        << "the debug relocation guard did not reject an event holding a cursor into its own storage: "
           "either it no longer runs on the cross-core path, or it no longer reports why";
}

// The real-world shape: a by-value short std::string. Whether the guard must reject it depends on
// the standard library, so the two possible answers are BOTH asserted, one test each — it is
// rejected where it is genuinely self-referential (libstdc++, exactly where it used to corrupt the
// peer's heap in silence), and it is delivered untouched where it is not (libc++). Neither platform
// is left with nothing to check.
namespace {

struct StdStringEvent : public qb::Event {
    std::string text{kShort}; // <= 15 chars: lives in the SSO buffer
};

class StrRecv final : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<StdStringEvent>(*this);
        co_return true;
    }
    void
    on(StdStringEvent const &) {
        kill();
    }
};

class StrSend final : public qb::Actor {
    const qb::ActorId _to;

public:
    explicit StrSend(qb::ActorId to)
        : _to(to) {}
    qb::io::async::task<bool>
    onInit() override {
        push<StdStringEvent>(_to);
        kill();
        co_return true;
    }
};

void
push_short_std_string_cross_core() {
    qb::Main main;
    auto     rcv = main.addActor<StrRecv>(1);
    main.addActor<StrSend>(0, rcv);
    main.start();
    main.join();
}

} // namespace

TEST(RelocatablePayloadDeathTest, ShortStdStringEventIsRejectedWhereItIsUnsafe) {
    if (std::thread::hardware_concurrency() < 2u)
        GTEST_SKIP() << "requires-multicore: the guard sits on the cross-core relocation path";
    if (!is_self_referential<std::string>([] { return std::string(kShort); }))
        GTEST_SKIP() << "this stdlib's short std::string is not self-referential (libc++ recomputes "
                        "data() from this), so there is nothing for the guard to reject here. The guard "
                        "is still covered on this platform: SelfReferentialEventIsRejectedCrossCoreInDebug "
                        "proves it fires, and ShortStdStringEventCrossesCoresWithoutTrippingTheGuard "
                        "proves it does not false-positive on this very payload";

    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    EXPECT_DEATH(push_short_std_string_cross_core(), kGuardDiagnostic)
        << "this stdlib's short std::string IS self-referential, so the guard must refuse to relocate "
           "it — otherwise the peer core frees a pointer that never came from the heap";
}

// The complement, for the platforms where the death test above has nothing to reject: on libc++ a
// short std::string really is trivially relocatable, so pushing one cross-core must complete
// normally. That makes this the guard's negative control for the real-world shape — it runs the
// exact event the death test would have used, and a guard that rejected it would abort this
// process. Deliberately NOT a death test: the expected outcome here is survival.
TEST(RelocatablePayload, ShortStdStringEventCrossesCoresWithoutTrippingTheGuard) {
    if (std::thread::hardware_concurrency() < 2u)
        GTEST_SKIP() << "requires-multicore: relocation only happens on the cross-core path";
    if (is_self_referential<std::string>([] { return std::string(kShort); }))
        GTEST_SKIP() << "this stdlib's short std::string is self-referential, so the guard is SUPPOSED "
                        "to reject it — see ShortStdStringEventIsRejectedWhereItIsUnsafe";

    EXPECT_TRUE(run_engine(&push_short_std_string_cross_core, std::chrono::seconds(30)))
        << "the engine never finished: a short std::string event did not complete its cross-core hop";
}
#endif // !NDEBUG

// A type-level backstop for the ONE sanctioned type that carries its text inline. Trivial
// copyability is strictly STRONGER than relocation needs — std::vector is safely relocatable and is
// not trivially copyable — so this says nothing about the other sanctioned payloads; it only pins
// that qb::string<N> never grows a member that would make a raw byte copy unsound.
TEST(RelocatablePayload, QbStringStaysTriviallyCopyable) {
    EXPECT_TRUE(std::is_trivially_copyable_v<qb::string<32>>)
        << "qb::string<N> must stay memcpy-safe; it is the sanctioned inline string for events";
}
