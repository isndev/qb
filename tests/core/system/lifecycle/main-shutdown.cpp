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
 */

#include <chrono>
#include <thread>

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/main.h>

namespace {

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
        qb::Main main;
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
        qb::Main main;
        main.core(0).addActor<LongLivedActor>();
        main.core(1).addActor<LongLivedActor>();
        main.start(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        // fall off the scope — ~Main() issues request_stop() + join()
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t_start);
    EXPECT_LT(elapsed.count(), 10) << "~Main() must shut down promptly via std::stop_source/jthread";
}
