/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/core/actor-name-lifetime.cpp
 * @brief `ActorProxy::getName<T>()` must hand out storage that outlives static destruction.
 *
 * `Actor::name` is a bare `const char *`, set once from `ActorProxy::getName<T>()` and read
 * back by `Actor::getName()` and by both `operator<<(…, Actor const&)` overloads — which is
 * what `VirtualCore::removeActor()` streams (`QB_LOG_INFO("Delete " << *actor)`). Under
 * `QB_WITH_LOGGING` that read is live: nanolog's default level is 0, so `is_logged(INFO)` is
 * true and the actor really is formatted.
 *
 * On MSVC that pointer is `typeid(T).name()` — static storage, valid for the whole program,
 * the same contract `Event.h` documents for its type-name registry ("a link-time constant
 * address, never a heap string"). The `__GNUC__` branch used to break that contract: it cached
 * the `abi::__cxa_demangle()` buffer in a function-local
 * `static std::unique_ptr<char, void(*)(void*)>`, so the storage was released by an
 * `__cxa_atexit` handler — on the main thread, at static-destruction time, in an order nothing
 * about the engine controls.
 *
 * Two things follow, and this file pins both:
 *
 *   1. **Ordering.** `__cxa_atexit` handlers run LIFO. The cache registers its handler the
 *      first time an actor of that type is named — i.e. at run time, from whichever thread got
 *      there first — so ANY handler registered earlier (a static destructor, another
 *      translation unit's cleanup, `atexit()` from `main`) runs AFTER the name is freed and
 *      sees dangling storage. `NameStorageOutlivesStaticDestruction` registers exactly such a
 *      handler before `main()` and reads the name from it.
 *
 *   2. **Threads.** The engine offers no guarantee that every `VirtualCore` worker is gone
 *      before static destruction begins — `qb::Main` joins its workers in `~Main`, but a `Main`
 *      that outlives `main()`, or an engine parked on a detached thread (the shape four tests
 *      in this suite already use: sigterm-shutdown, kill-during-reap, oversize-event-probe,
 *      relocatable-payload), is still running when the atexit chain fires. A worker formatting
 *      an actor at that moment reads freed heap.
 *      `NameStorageIsNotFreedWhileAnotherThreadStillReadsIt` keeps a detached reader on the
 *      pointer across process exit.
 *
 * The fix is not a lock: it is that the cache must be immortal, exactly like the MSVC branch it
 * mirrors. One allocation per actor type, never released — bounded by the number of actor types
 * in the program, and rooted in a `static const char *` so LeakSanitizer sees it as reachable
 * and does not report it.
 *
 * Oracles, in order of strength:
 *   - under `sanitize` (ASan) both reads are a `heap-use-after-free` and abort deterministically;
 *   - under `sanitize-thread` the second is a `data race` between the atexit `free()` and the
 *     detached reader, which is the shape originally reported from a shutdown-time TSan run;
 *   - everywhere else the byte comparison in the atexit handler is the allocator-independent
 *     signal (weaker: it only fires if the freed block is actually reused or scribbled), and it
 *     forces a non-zero exit itself because gtest has already reported by then.
 *
 * Pure logic: no engine, no daemon.
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <gtest/gtest.h>
#include <qb/actor.h>

namespace {

/// Distinct actor types, one per case, so neither test can be perturbed by the other's cache.
/// Never instantiated — `getName<T>()` only ever needs `typeid(T)` — so `onInit()` is left at
/// `qb::Actor`'s default.
struct NameLifetimeProbeActor : qb::Actor {};
struct NameLifetimeThreadProbeActor : qb::Actor {};

// ── Case 1 state: read from an atexit handler registered before main() ────────────────────
const char *g_name = nullptr; ///< The cached pointer, captured by the test body.
std::string g_name_bytes;     ///< What it must still read as at the very end of the process.

void
verify_name_survives_static_destruction() {
    if (g_name == nullptr)
        return; // test body never ran (e.g. --gtest_filter excluded it): nothing to verify

    // THE READ. Registered before main(), so this handler runs LAST — after the cache's own
    // __cxa_atexit handler has run. With a cache that frees itself this dereferences released
    // heap: ASan aborts right here.
    if (std::strcmp(g_name, g_name_bytes.c_str()) != 0) {
        std::fprintf(stderr,
                     "[actor-name-lifetime] ActorProxy::getName<T>() storage did not survive static "
                     "destruction: expected \"%s\", read \"%s\". Actor::name is a bare pointer read by "
                     "VirtualCore::removeActor(); freeing its storage at exit leaves every still-running "
                     "worker dereferencing freed heap.\n",
                     g_name_bytes.c_str(), g_name);
        std::fflush(stderr);
        // gtest has already printed its verdict; the only way left to red the run is the exit code.
        std::_Exit(1);
    }
}

/// Registers the handler above during dynamic initialisation — i.e. strictly before any test body
/// can call `getName<T>()` and install the cache's own handler. LIFO ordering does the rest.
const struct AtExitRegistrar {
    AtExitRegistrar() {
        (void) std::atexit(&verify_name_survives_static_destruction);
    }
} g_at_exit_registrar;

// ── Case 2 state: a detached reader still holding the pointer at process exit ─────────────
// Deliberately `relaxed`: these flags sequence the handshake without publishing a
// happens-before edge, which is what makes the exit-time free and this thread's reads
// genuinely unordered — the same absence of ordering a detached engine thread has against
// the atexit chain.
std::atomic<bool>         g_reader_started{false};
std::atomic<const char *> g_reader_target{nullptr};

} // namespace

/**
 * @brief The cached name must still read correctly from the last atexit handler in the chain.
 *
 * Captures the pointer and its bytes; @ref verify_name_survives_static_destruction does the
 * checking, at the one moment that distinguishes immortal storage from `__cxa_atexit`-owned
 * storage.
 */
TEST(ActorNameLifetime, NameStorageOutlivesStaticDestruction) {
    const char *name = qb::ActorProxy::getName<NameLifetimeProbeActor>();
    ASSERT_NE(name, nullptr) << "getName<T>() must never hand back nullptr — Actor::getName() "
                                "builds a std::string_view straight from it";
    EXPECT_NE(std::string_view(name).find("NameLifetimeProbeActor"), std::string_view::npos) << "expected a demangled type name, got: " << name;

    // Stable across calls: the whole point of the cache.
    EXPECT_EQ(name, qb::ActorProxy::getName<NameLifetimeProbeActor>());

    g_name_bytes = name;
    g_name       = name;
}

/**
 * @brief The cached name must not be freed while another thread is still reading it.
 *
 * The reader is detached and never joined, so nothing orders its reads against the atexit
 * chain — precisely the position a `VirtualCore` worker is in when the engine outlives
 * `main()`. It keeps reading across process exit; a cache that frees itself at static
 * destruction turns those reads into a use-after-free (ASan) and a data race (TSan).
 */
TEST(ActorNameLifetime, NameStorageIsNotFreedWhileAnotherThreadStillReadsIt) {
    const char *name = qb::ActorProxy::getName<NameLifetimeThreadProbeActor>();
    ASSERT_NE(name, nullptr);

    g_reader_target.store(name, std::memory_order_relaxed);
    std::thread([] {
        const char *target = nullptr;
        while ((target = g_reader_target.load(std::memory_order_relaxed)) == nullptr)
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        g_reader_started.store(true, std::memory_order_relaxed);
        // Never returns: the process exits out from under it, atexit chain and all.
        for (;;) {
            volatile char sink = target[0];
            (void) sink;
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }).detach();

    // Make sure the reader is actually on the pointer before this test returns, so it is
    // demonstrably live for the rest of the binary's run — including exit.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!g_reader_started.load(std::memory_order_relaxed) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    EXPECT_TRUE(g_reader_started.load(std::memory_order_relaxed)) << "reader thread never started; the case would prove nothing";
}
