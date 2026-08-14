/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * See the License for the specific terms.
 */

/**
 * @file system/coroutine/coroutine-escaped-exception.cpp
 * @brief An exception escaping a `spawn` / `spawn_detached` body is REPORTED, not swallowed.
 *
 * A spawned coroutine has no continuation and no `task<>` owner. `unhandled_exception()` parks the
 * `exception_ptr` in a promise the scheduler then destroys on its frame drain (`task.h:846`,
 * `:817-828`), and nobody ever reads it. Measured before the fix: a `throw std::runtime_error(...)`
 * after a `co_await` in a `spawn()` body produced NO output at any log level, left
 * `Main::hasError()` false, and the engine carried on. That was the last silent failure path in
 * the actor surface — an exception out of `onInit()` has been reported since 2.x
 * (`VirtualCore.cpp:505`), and the two are now in line.
 *
 * The fix is a report, not a behaviour change, and this file pins both halves:
 *   - the diagnostic reaches `std::cerr` (via `qb::io::cerr`, which is always compiled — the
 *     structured `QB_LOG_CRIT` channel is a no-op unless the build defines QB_WITH_LOGGING or
 *     QB_STDOUT_LOGGING, `qb/io.h:262-265`), naming the actor, the API and the `what()`;
 *   - control flow is unchanged: the frame still unwinds, RAII in the body still runs, the
 *     coroutine counter still drops, the engine still keeps going, `hasError()` stays false.
 *
 * And the negative half, which is what stops the fix becoming noise: `cancelled_error` is the
 * teardown protocol for a scoped coroutine, not a failure, and must stay silent.
 */

#include <atomic>
#include <chrono>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/main.h>

using namespace std::chrono_literals;

namespace coroutine_escaped_exception_test {

std::atomic<bool> g_body_raii_ran{false};
std::atomic<bool> g_engine_kept_going{false};

/// Captures everything written to `std::cerr` for its lifetime.
class CerrCapture {
    std::stringstream _buf;
    std::streambuf   *_saved;

public:
    CerrCapture()
        : _saved(std::cerr.rdbuf(_buf.rdbuf())) {}
    ~CerrCapture() {
        std::cerr.rdbuf(_saved);
    }
    std::string
    str() const {
        return _buf.str();
    }
};

struct RaiiWitness {
    ~RaiiWitness() {
        g_body_raii_ran.store(true, std::memory_order_release);
    }
};

class Thrower : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        spawn([](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            RaiiWitness witness;
            co_await ctx.sleep(2ms);
            throw std::runtime_error("scoped-body-boom");
        });
        spawn_detached([](qb::CoroContext) -> qb::io::async::task<void> {
            co_await qb::io::async::sleep(2ms);
            throw std::runtime_error("detached-body-boom");
        });
        // Runs after both have thrown: proves the core is still turning.
        spawn([](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            co_await ctx.sleep(120ms);
            g_engine_kept_going.store(true, std::memory_order_release);
            ctx.push<qb::KillEvent>();
        });
        co_return true;
    }
};

/// A scoped coroutine parked on a cancellation-aware await when its actor dies. The wrapper takes
/// `cancelled_error` first, on purpose — reporting it would fire on every ordinary kill.
class CancelledSleeper : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        spawn([](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> { co_await ctx.sleep(30s); });
        kill();
        co_return true;
    }
};

TEST(CoroutineEscapedException, BothSpawnPathsReportInsteadOfSwallowing) {
    std::string captured;
    bool        had_error = true;
    {
        CerrCapture capture;
        {
            qb::Main main;
            main.addActor<Thrower>(0);
            main.start(false);
            main.join();
            had_error = main.hasError();
        }
        captured = capture.str();
    }

    EXPECT_NE(captured.find("scoped-body-boom"), std::string::npos) << "spawn()'s escaped exception was not reported. Got:\n" << captured;
    EXPECT_NE(captured.find("detached-body-boom"), std::string::npos) << "spawn_detached()'s escaped exception was not reported. Got:\n"
                                                                      << captured;
    // The diagnostic has to name which call the user wrote, or it sends them looking.
    EXPECT_NE(captured.find("spawn()"), std::string::npos) << captured;
    EXPECT_NE(captured.find("spawn_detached()"), std::string::npos) << captured;
    EXPECT_NE(captured.find("DISCARDED"), std::string::npos) << captured;

    // Reporting must not have changed anything else.
    EXPECT_TRUE(g_body_raii_ran.load(std::memory_order_acquire)) << "the body's RAII did not run — the frame no longer unwinds";
    EXPECT_TRUE(g_engine_kept_going.load(std::memory_order_acquire)) << "the engine stopped after a coroutine threw";
    EXPECT_FALSE(had_error) << "a thrown-and-reported coroutine body must not flag the engine";
}

TEST(CoroutineEscapedException, ScopeCancellationStaysSilent) {
    std::string captured;
    {
        CerrCapture capture;
        {
            qb::Main main;
            main.addActor<CancelledSleeper>(0);
            main.start(false);
            main.join();
        }
        captured = capture.str();
    }
    EXPECT_EQ(captured.find("coroutine body let an exception escape"), std::string::npos)
        << "an actor being killed while its coroutine sleeps is the teardown protocol, not a failure. Got:\n"
        << captured;
}

} // namespace coroutine_escaped_exception_test
