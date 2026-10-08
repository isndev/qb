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
 * `__drive_init__` resumes the first `onInit()` directly. If that first suspension completes
 * inline, `await_suspend` queues the continuation before `__begin_activation__` can run.
 * Previously no listener scheduler was bound yet: the continuation landed on a thread-local
 * fallback that the actor loop never drains. A positive timer/connect may mask the defect
 * because its callback arrives after the listener scheduler is installed.
 *
 * WHAT WAS WRONG
 * --------------
 * Bind the listener scheduler before that first resume. Keep `onInit()` owned by the activation
 * record until a later listener turn drains its continuation; resuming inline would re-enter
 * actor initialization and change the coroutine scheduling contract.
 *
 * WHAT IS ASSERTED
 * ----------------
 *   1. `ConnectInInit`      — `co_await async::tcp::connect<>(uri, timeout)` as the FIRST
 *                             suspension of `onInit()` resumes, and hands back an open socket.
 *   2. `SleepInInit`        — positive delay, and zero/negative delay on a fresh core. The
 *                             latter two queue during the first `await_suspend`, before
 *                             `__begin_activation__` has a chance to run.
 *   3. `InlineCallbackInFirstInit` — an `async_awaiter<int>` whose callback completes inline
 *                             must resume on the loop's ready drain, not re-enter `onInit`.
 *   4. `ConnectInSpawn`     — the control: the identical expression inside `spawn()` still works.
 *   5. `ConnectInDynamicInit` — the `addRefActor` path, which reaches the same funnel through
 *                             `initActor()` rather than `__init__actors__()`.
 *   6. Synchronous first init and standalone immediate awaiters remain working controls.
 *
 * Actor assertions are mirrored to post-`join()` atomics and read after `join()`.
 *
 * HOW A REGRESSION REPORTS, MEASURED RATHER THAN ASSUMED
 * -----------------------------------------------------
 * A lost continuation hangs in `Main::join()` because the actor remains Activating. The
 * watchdog bounds wrong results and partial regressions; the explicit CTest TIMEOUT 120
 * bounds a fully wedged init. The pre-fix zero, negative and inline cases each timed out
 * under a separate three-second process watchdog; the positive/sync/standalone controls passed.
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
std::atomic<int>  g_resumed{0};
std::atomic<int>  g_socket_open{0};
std::atomic<int>  g_slept{0};
std::atomic<bool> g_inline_callback_returned{false};
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
    const qb::duration _delay;

public:
    explicit SleepInInitActor(qb::duration delay = 20ms)
        : _delay(delay) {}

    qb::io::async::task<bool>
    onInit() final {
        co_await qb::io::async::sleep(_delay);
        g_slept.fetch_add(1, std::memory_order_relaxed);
        g_done.store(true, std::memory_order_release);
        kill();
        co_return true;
    }
};

class InlineCallbackInInitActor final : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() final {
        const int value = co_await qb::io::async::async_awaiter<int>([](auto callback) {
            callback(17);
            g_inline_callback_returned.store(true, std::memory_order_release);
        });
        if (value == 17 && g_inline_callback_returned.load(std::memory_order_acquire))
            g_resumed.fetch_add(1, std::memory_order_relaxed);
        g_done.store(true, std::memory_order_release);
        kill();
        co_return true;
    }
};

class SynchronousInitActor final : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() final {
        g_resumed.fetch_add(1, std::memory_order_relaxed);
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
    g_inline_callback_returned.store(false, std::memory_order_relaxed);
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

TEST(InitIoAwaiters, ZeroSleepInFirstInitResumes) {
    reset_counters();

    qb::Main main;
    main.addActor<SleepInInitActor>(0, qb::duration::zero());
    main.addActor<WatchdogActor>(1, qb::duration{15s});
    main.start();
    main.join();

    EXPECT_EQ(g_slept.load(std::memory_order_relaxed), 1) << "zero sleep queued on an unpumped scheduler";
}

TEST(InitIoAwaiters, NegativeSleepInFirstInitResumes) {
    reset_counters();

    qb::Main main;
    main.addActor<SleepInInitActor>(0, qb::duration{-1ms});
    main.addActor<WatchdogActor>(1, qb::duration{15s});
    main.start();
    main.join();

    EXPECT_EQ(g_slept.load(std::memory_order_relaxed), 1) << "negative sleep queued on an unpumped scheduler";
}

TEST(InitIoAwaiters, InlineCallbackInFirstInitResumes) {
    reset_counters();

    qb::Main main;
    main.addActor<InlineCallbackInInitActor>(0);
    main.addActor<WatchdogActor>(1, qb::duration{15s});
    main.start();
    main.join();

    EXPECT_EQ(g_resumed.load(std::memory_order_relaxed), 1) << "inline callback queued on an unpumped scheduler";
}

TEST(InitIoAwaiters, SynchronousFirstInitStillCompletes) {
    reset_counters();

    qb::Main main;
    main.addActor<SynchronousInitActor>(0);
    main.addActor<WatchdogActor>(1, qb::duration{15s});
    main.start();
    main.join();

    EXPECT_EQ(g_resumed.load(std::memory_order_relaxed), 1);
}

TEST(InitIoAwaiters, StandaloneImmediateAwaitersStillResume) {
    auto operation = []() -> qb::io::async::task<int> {
        co_await qb::io::async::sleep(qb::duration::zero());
        co_return co_await qb::io::async::async_awaiter<int>([](auto callback) { callback(17); });
    };
    EXPECT_EQ(qb::io::async::run_sync(operation()), 17);
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
