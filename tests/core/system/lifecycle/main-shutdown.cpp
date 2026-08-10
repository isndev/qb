/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/lifecycle/main-shutdown.cpp
 * @brief Engine shutdown via std::stop_source / jthread RAII.
 *
 * Long-lived actors (no self-kill) must come down promptly both ways:
 *   - explicit `Main::stop()` — the signal-handler-safe path that broadcasts a SIGINT to every
 *     core, so each actor's SignalEvent handler calls `kill()`;
 *   - implicit `~Main()` — the destructor must `request_stop()` on the engine's std::stop_source
 *     and join the worker jthreads, achieving the same shutdown with no explicit stop() call.
 *
 * Both cases bound the join with a `ShutdownWatchdog` (see below): the promptness assertion sits
 * AFTER a join that has no timed overload, so without it the one failure each case exists to catch
 * would hang rather than assert.
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/main.h>

namespace {

using namespace std::chrono_literals;

/// The prompt-shutdown bound both cases claim. One constant so the watchdog and the post-join
/// `EXPECT_LT(elapsed.count(), 10)` can never drift apart and disagree about what "promptly" means.
constexpr auto kShutdownBudget = 10s;

/**
 * @brief Bounds an otherwise-unbounded engine shutdown so a hang REPORTS the assertion.
 *
 * `qb::Main::join()` has no timed overload (`Main.h`), and both cases here assert `elapsed < 10 s`
 * AFTER it returns. On the exact regression they target — a shutdown that never completes — the
 * join (explicit in the first case, inside `~Main()` in the second) blocks and that assertion is
 * never reached: the case dies at the tier timeout having asserted nothing, so a real defect reads
 * as infrastructure flake. The bound is the whole subject of the file, and it was the one thing
 * unreachable on failure.
 *
 * This makes the same claim from a thread the engine cannot block. If the budget elapses it records
 * the failure the case was written to make and aborts — a wedged join cannot be unwound, and an
 * immediate abort carrying the message beats a silent timeout. The `fflush(nullptr)` is belt and
 * braces: gtest's own printer flushes stdout after each part result (verified — the message does
 * arrive through a captured pipe), but `abort()` flushes nothing itself, and losing the diagnosis
 * would put this case straight back to being a timeout that names nothing.
 *
 * Declare it BEFORE the `qb::Main` it guards so it is destroyed (disarmed) only after `~Main()` has
 * returned — that destructor is itself the subject of the second case.
 */
class ShutdownWatchdog {
public:
    ShutdownWatchdog(std::chrono::seconds budget, const char *what)
        : _thread([this, budget, what] {
            const auto deadline = std::chrono::steady_clock::now() + budget;
            while (!_done.load(std::memory_order_acquire)) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    ADD_FAILURE() << what << " — the engine was still shutting down after " << budget.count()
                                  << " s; join() is unbounded, so this is the assertion the case could not otherwise reach";
                    std::fflush(nullptr);
                    std::abort();
                }
                std::this_thread::sleep_for(20ms);
            }
        }) {}

    ShutdownWatchdog(const ShutdownWatchdog &)            = delete;
    ShutdownWatchdog &operator=(const ShutdownWatchdog &) = delete;

    ~ShutdownWatchdog() {
        _done.store(true, std::memory_order_release);
        _thread.join();
    }

private:
    std::atomic<bool> _done{false}; // declared first: the thread reads it from its first turn
    std::thread       _thread;
};

class LongLivedActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() final {
        registerEvent<qb::KillEvent>(*this);
        registerEvent<qb::SignalEvent>(*this);
        co_return true; // stays alive — no kill() in onInit
    }
    void
    on(qb::KillEvent const &) {
        kill();
    }
    void
    on(qb::SignalEvent const &) {
        kill();
    }
};

} // namespace

TEST(StopSource, ExplicitStopShutsDownPromptly) {
    const auto t_start = std::chrono::steady_clock::now();
    {
        ShutdownWatchdog watchdog{kShutdownBudget, "Main::stop() must broadcast SIGINT and shut down promptly"};
        qb::Main         main;
        main.core(0).addActor<LongLivedActor>();
        main.core(1).addActor<LongLivedActor>();
        main.start(true); // async: returns immediately
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        main.stop();
        main.join();
        EXPECT_FALSE(main.hasError());
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t_start);
    EXPECT_LT(elapsed.count(), 10) << "Main::stop() must broadcast SIGINT and shut down promptly";
}

TEST(StopSource, MainDestructorJoinsRunningWorkers) {
    // No explicit stop(): ~Main() must still bring the engine down via _stop_source.request_stop()
    // + jthread RAII. The synthesised SIGINT reaches each core, every LongLivedActor gets a
    // SignalEvent, calls kill(), and the workflow loop terminates.
    const auto t_start = std::chrono::steady_clock::now();
    {
        // Declared before `main`, so it is still armed while ~Main() runs — the join under test here
        // is the one inside that destructor.
        ShutdownWatchdog watchdog{kShutdownBudget, "~Main() must shut down promptly via std::stop_source/jthread"};
        qb::Main         main;
        main.core(0).addActor<LongLivedActor>();
        main.core(1).addActor<LongLivedActor>();
        main.start(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        // fall off the scope — ~Main() issues request_stop() + join()
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t_start);
    EXPECT_LT(elapsed.count(), 10) << "~Main() must shut down promptly via std::stop_source/jthread";
}
