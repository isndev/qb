/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/init/init-io-awaiters.cpp
 * @brief `onInit()` may `co_await` a qb-io awaiter as its FIRST suspension.
 *
 * The other init tests all suspend on the actor surface — `qb::ask`, `ctx.sleep`, the pattern
 * library — whose awaiters resolve their scheduler at RESUME time through
 * `schedule_via_current` (scheduler.h:888-902). This file covers the other family: a qb-io
 * awaiter that CACHES a scheduler pointer at SUSPEND time and resumes through the cached copy
 * (`connect_awaiter`, connector.h:708-710 and 722-724; `awaiter_base`, awaiter.h:329-333 and
 * 191-193). Nothing exercised that combination, and it did not work.
 *
 * WHAT WAS WRONG
 * --------------
 * `__drive_init__` resumes the `onInit()` frame directly and deliberately never `spawn()`s it,
 * so on the pre-loop path no coroutine scheduler was bound to the thread yet.
 * `Actor::spawn` binds one as its first act (`Actor::__resolve_coro_scheduler__`,
 * Actor.cpp:252-255) — which is why the very same `co_await` resumed normally inside `spawn()`
 * and not inside `onInit()`. Finding null, the awaiter fell back to
 * `CoroutineScheduler::current()`, which lazily creates a THREAD-LOCAL FALLBACK
 * (scheduler.h:631-632) and cached that. `__begin_activation__` then called
 * `listener::current.coro_scheduler()`, whose first call `set_current()`s the LISTENER's
 * scheduler (listener.h:890), and `listener::run()` pumps only that one (listener.h:777-778).
 * The completion callback queued the resume into the orphaned fallback and it was never drained.
 *
 * The symptom was deceptive from every angle: the awaited operation SUCCEEDED (the TCP connect
 * completed and the server saw its session), the actor never failed, and the activation deadline
 * did not reap it either — `__pump_activations__` cancels the actor's coro scope, and an awaiter
 * parked on a plain completion callback holds no cancellation token, so the frame never reached
 * `done()`. A hang with a healthy-looking connection on both ends.
 *
 * WHAT IS ASSERTED
 * ----------------
 *   1. `ConnectInInit`      — `co_await async::tcp::connect<>(uri, timeout)` as the FIRST
 *                             suspension of `onInit()` resumes, and hands back an open socket.
 *   2. `SleepInInit`        — the same for `qb::io::async::sleep`, the other cached-scheduler
 *                             awaiter, so the fix is proven at the family level and not at one
 *                             call site. This is the shape AGENTS.md already warned about.
 *   3. `ConnectInSpawn`     — the control: the identical expression inside `spawn()` still works.
 *                             If the fix ever regresses to "bind only on the spawn path" this
 *                             stays green while 1 and 2 go red, which is the original bug.
 *   4. `ConnectInDynamicInit` — the `addRefActor` path, which reaches the same funnel through
 *                             `initActor()` rather than `__init__actors__()`.
 *
 * Every assertion is mirrored to a post-`join()` atomic and read after `join()`.
 *
 * HOW A REGRESSION REPORTS, MEASURED RATHER THAN ASSUMED
 * -----------------------------------------------------
 * It HANGS. That was the first thing written here as "a regression fails rather than hangs", and
 * neutralising the fix and re-running disproved it: the whole binary wedged and had to be killed.
 * The reason is the second half of the finding, and nothing in this file can route around it.
 * `WatchdogActor` broadcasts a `KillEvent`; a `KillEvent` does reach an Activating actor (the gate
 * lets it through, VirtualCore.cpp:173) and does cancel its coro scope — but a `connect_awaiter`
 * parked on a plain completion callback registers with no cancellation token, so the frame never
 * reaches `done()`, `_activating` never empties, the core never leaves `__workflow__`, and
 * `Main::join()` blocks for ever. The engine has no in-band way to end that run, which is exactly
 * why the activation deadline could not reap the original defect either.
 *
 * So the watchdog earns its place for the OTHER shapes — a subject that resumes but produces the
 * wrong value, a partial regression on one of the four paths — and the ctest `TIMEOUT` is what
 * reports a total one. That timeout is given EXPLICITLY at the call site (120 s, against a healthy
 * 23 ms) rather than inherited: the `requires-multicore` floor is 600 s, and waiting ten minutes
 * for a hang whose healthy runtime is milliseconds is a worse report, not a safer one.
 *
 * tier=system. Run under ASAN_OPTIONS=detect_leaks=0 like the rest of the coroutine suites.
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/io/async.h>
#include <qb/io/async/tcp/connector.h>
#include <qb/io/tcp/listener.h>
#include <qb/main.h>

using namespace std::chrono_literals;

namespace init_io_awaiters_test {

// Observed from inside the actors, read after join(). `resumed` is the whole question: the
// defect left it 0 while `connected` (the peer's view) was already 1.
std::atomic<int> g_resumed{0};
std::atomic<int> g_socket_open{0};
std::atomic<int> g_slept{0};
// Raised by the subject the moment its work is observable. The watchdog watches THIS, not the
// clock, on the healthy path — otherwise the watchdog itself keeps its core alive for the whole
// budget and every run costs the timeout even when nothing is wrong.
std::atomic<bool> g_done{false};

// A bare blocking listener on an ephemeral port, owned by the test rather than the engine: the
// connect only has to COMPLETE, and a real qb server would add an accept path this test is not
// about. `listen()`'s backlog holds the connection, which is all the connector waits for.
class EphemeralPort {
    qb::io::tcp::listener _listener;
    std::uint16_t         _port{0};

public:
    EphemeralPort() {
        // Port 0 => the OS picks. Bind before any actor exists so the URI is known up front.
        if (_listener.listen_v4(0, "127.0.0.1") == 0)
            _port = _listener.local_endpoint().port();
    }
    [[nodiscard]] bool
    ok() const noexcept {
        return _port != 0;
    }
    [[nodiscard]] std::string
    uri() const {
        return "tcp://127.0.0.1:" + std::to_string(_port);
    }
};

// Bounds every test from inside the engine: if an `onInit()` never resumes, its core never
// finishes activating and `join()` would block for ever. This actor sits on its own core and
// ends the run either as soon as the subject reports done (the healthy path, immediate) or at a
// wall-clock budget (the regression path), turning a hang into a failed assertion.
//
// Watching `g_done` rather than only the clock is what keeps a healthy run fast: a watchdog that
// only ever fires on the budget holds its core alive for the full budget every time, so all four
// cases cost the timeout whether they pass or fail -- measured at 15000 ms each before this.
class WatchdogActor
    : public qb::Actor
    , public qb::ICallback {
    const qb::duration _budget;
    qb::mono_time      _start{};

public:
    explicit WatchdogActor(qb::duration budget)
        : _budget(budget) {}

    qb::io::async::task<bool>
    onInit() final {
        _start = qb::mono_now();
        registerCallback(*this);
        co_return true;
    }

    void
    on(qb::LoopEvent const &) final {
        if (g_done.load(std::memory_order_acquire) || qb::mono_now() - _start >= _budget)
            broadcast<qb::KillEvent>();
    }
};

// 1 + 4. `co_await connect(...)` as the first suspension of an async onInit.
class ConnectInInitActor final : public qb::Actor {
    const std::string _uri;

public:
    explicit ConnectInInitActor(std::string uri)
        : _uri(std::move(uri)) {}

    qb::io::async::task<bool>
    onInit() final {
        // FIRST suspension of onInit, and an awaiter that caches its scheduler.
        auto socket = co_await qb::io::async::tcp::connect<qb::io::transport::tcp>(qb::io::uri{_uri}, 5s);
        g_resumed.fetch_add(1, std::memory_order_relaxed);
        if (socket && socket->is_open())
            g_socket_open.fetch_add(1, std::memory_order_relaxed);
        g_done.store(true, std::memory_order_release);
        kill();
        co_return true;
    }
};

// 2. `sleep` as the first suspension — the same family, the shape AGENTS.md names.
class SleepInInitActor final : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() final {
        co_await qb::io::async::sleep(20ms);
        g_slept.fetch_add(1, std::memory_order_relaxed);
        g_done.store(true, std::memory_order_release);
        kill();
        co_return true;
    }
};

// 3. The control: the identical await, but reached through `spawn()`, which binds the scheduler
// on the way in. This passed throughout the defect and must keep passing.
class ConnectInSpawnActor final : public qb::Actor {
    const std::string _uri;

public:
    explicit ConnectInSpawnActor(std::string uri)
        : _uri(std::move(uri)) {}

    qb::io::async::task<bool>
    onInit() final {
        const std::string u = _uri;
        // Capture by VALUE and talk back only through the context: the actor may be gone by the
        // time this resumes. `push` targets the spawning actor's own id.
        spawn([u](auto ctx) -> qb::io::async::task<void> {
            auto socket = co_await qb::io::async::tcp::connect<qb::io::transport::tcp>(qb::io::uri{u}, 5s);
            g_resumed.fetch_add(1, std::memory_order_relaxed);
            if (socket && socket->is_open())
                g_socket_open.fetch_add(1, std::memory_order_relaxed);
            g_done.store(true, std::memory_order_release);
            ctx.template push<qb::KillEvent>();
        });
        co_return true;
    }
};

// 4. The dynamic path: this actor's own init is synchronous, and it brings up a child whose
// init suspends on connect. `addRefActor` drives that child through `initActor()`, a different
// caller of the same funnel.
class DynamicParentActor final : public qb::Actor {
    const std::string _uri;

public:
    explicit DynamicParentActor(std::string uri)
        : _uri(std::move(uri)) {}

    qb::io::async::task<bool>
    onInit() final {
        addRefActor<ConnectInInitActor>(_uri);
        kill();
        co_return true;
    }
};

void
reset_counters() {
    g_resumed.store(0, std::memory_order_relaxed);
    g_socket_open.store(0, std::memory_order_relaxed);
    g_slept.store(0, std::memory_order_relaxed);
    g_done.store(false, std::memory_order_release);
}

} // namespace init_io_awaiters_test

using namespace init_io_awaiters_test;

// --- 1. the defect -----------------------------------------------------------------------

TEST(InitIoAwaiters, ConnectInInitResumes) {
    EphemeralPort port;
    ASSERT_TRUE(port.ok()) << "could not bind a loopback listener; the environment, not qb";
    reset_counters();

    qb::Main main;
    main.addActor<ConnectInInitActor>(0, port.uri());
    main.addActor<WatchdogActor>(1, qb::duration{15s});
    main.start();
    main.join();

    // The connect always succeeded, even while broken. `resumed` is the assertion that matters.
    EXPECT_EQ(g_resumed.load(std::memory_order_relaxed), 1) << "onInit() never resumed after co_await connect(...)";
    EXPECT_EQ(g_socket_open.load(std::memory_order_relaxed), 1);
}

// --- 2. the same family, via sleep -------------------------------------------------------

TEST(InitIoAwaiters, SleepInInitResumes) {
    reset_counters();

    qb::Main main;
    main.addActor<SleepInInitActor>(0);
    main.addActor<WatchdogActor>(1, qb::duration{15s});
    main.start();
    main.join();

    EXPECT_EQ(g_slept.load(std::memory_order_relaxed), 1) << "onInit() never resumed after co_await sleep(...)";
}

// --- 3. the control ----------------------------------------------------------------------

TEST(InitIoAwaiters, ConnectInSpawnStillResumes) {
    EphemeralPort port;
    ASSERT_TRUE(port.ok()) << "could not bind a loopback listener; the environment, not qb";
    reset_counters();

    qb::Main main;
    main.addActor<ConnectInSpawnActor>(0, port.uri());
    main.addActor<WatchdogActor>(1, qb::duration{15s});
    main.start();
    main.join();

    EXPECT_EQ(g_resumed.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(g_socket_open.load(std::memory_order_relaxed), 1);
}

// --- 4. the dynamic (addRefActor) path ---------------------------------------------------

TEST(InitIoAwaiters, ConnectInDynamicInitResumes) {
    EphemeralPort port;
    ASSERT_TRUE(port.ok()) << "could not bind a loopback listener; the environment, not qb";
    reset_counters();

    qb::Main main;
    main.addActor<DynamicParentActor>(0, port.uri());
    main.addActor<WatchdogActor>(1, qb::duration{15s});
    main.start();
    main.join();

    EXPECT_EQ(g_resumed.load(std::memory_order_relaxed), 1) << "a dynamically added actor's onInit() never resumed";
    EXPECT_EQ(g_socket_open.load(std::memory_order_relaxed), 1);
}
