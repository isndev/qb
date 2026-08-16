/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file io/unit/coroutine/detached-exception-report.cpp
 * @brief An exception escaping a DETACHED coroutine is reported, not silently dropped.
 *
 * `exception-propagation.cpp` covers the OWNED case: a `task<T>` someone awaits stores the
 * exception in its promise and `await_resume()` rethrows it (task.h:715-716). This file covers
 * the case with no owner — `qb::io::async::coro_scheduler().spawn(t)` — where that rethrow
 * never runs, because nothing awaits the frame.
 *
 * WHAT WAS WRONG
 * --------------
 * `spawn()` detaches the handle (scheduler.h:954-967); `unhandled_exception()` stores the
 * exception in the promise (task.h); the frame later reaches `final_suspend` with no
 * continuation and is handed straight to `defer_frame_destruction`. The exception was destroyed
 * with the promise, unobserved: the program simply stopped in the middle of the coroutine with
 * no message on any stream and no non-zero exit anywhere.
 *
 * `Actor::spawn` and `Actor::spawn_detached` were already fixed, by wrapping the user's body in
 * a coroutine that CATCHES and calls `qb::detail::report_unhandled_coroutine_exception`
 * (VirtualCore.h:1139-1146, 1160-1168). The free-function path has no such wrapper. The report
 * now happens in `final_suspend` itself, on the detached branch only.
 *
 * WHAT IS ASSERTED
 * ----------------
 *   1. `ReportsEscapingException`   — stderr carries a CRITICAL line naming the exception.
 *   2. `ReportsNonStdException`     — a throw of a non-`std::exception` type is reported too,
 *                                     rather than crashing the reporter.
 *   3. `SilentOnClean`              — a detached coroutine that completes normally prints
 *                                     NOTHING. Without this the guard could "pass" by printing
 *                                     on every spawn, which is worse than silence.
 *   4. `SilentOnCancellation`       — `cancelled_error` is teardown, not failure, and is the one
 *                                     exemption; it must not print. This is the false-positive
 *                                     case that would otherwise make the whole thing unusable
 *                                     for `when_any` losers and cancelled scopes.
 *   5. `AwaitedTaskStillRethrows`   — the OWNED path is untouched: an awaited task still
 *                                     propagates by throwing, and does NOT also print.
 *
 * stderr is captured by swapping `std::cerr`'s streambuf, which is what `qb::io::cerr` writes
 * through (logger.cpp).
 */

#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>

#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>

namespace detached_exception_report_test {

// RAII stderr capture. `qb::io::cerr` locks its own mutex and writes to `std::cerr`, so
// redirecting the underlying streambuf sees exactly what a user would.
class CapturedCerr {
    std::ostringstream _sink;
    std::streambuf    *_saved;

public:
    CapturedCerr()
        : _saved(std::cerr.rdbuf(_sink.rdbuf())) {}
    ~CapturedCerr() {
        std::cerr.rdbuf(_saved);
    }
    [[nodiscard]] std::string
    str() const {
        return _sink.str();
    }
};

struct NotAnException {
    int marker = 7;
};

qb::io::async::task<void>
throws_std() {
    co_await qb::io::async::sleep(std::chrono::milliseconds(1));
    throw std::runtime_error("escaping-marker-2f7a");
}

qb::io::async::task<void>
throws_non_std() {
    co_await qb::io::async::sleep(std::chrono::milliseconds(1));
    throw NotAnException{};
}

qb::io::async::task<void>
throws_cancelled() {
    co_await qb::io::async::sleep(std::chrono::milliseconds(1));
    throw qb::io::async::cancelled_error{};
}

qb::io::async::task<void>
completes_cleanly() {
    co_await qb::io::async::sleep(std::chrono::milliseconds(1));
    co_return;
}

qb::io::async::task<int>
throws_with_value() {
    co_await qb::io::async::sleep(std::chrono::milliseconds(1));
    throw std::runtime_error("owned-marker-91c3");
    co_return 1;
}

// Pump the loop long enough for the spawned frame to run, throw and reach final_suspend.
void
pump() {
    for (int i = 0; i < 200; ++i) {
        qb::io::async::run(EVRUN_NOWAIT);
        if (qb::io::async::listener::current.coro_scheduler().active_count() == 0)
            break;
    }
    qb::io::async::run(EVRUN_NOWAIT);
}

class DetachedExceptionReport : public ::testing::Test {
protected:
    void
    SetUp() override {
        qb::io::async::init();
    }
};

} // namespace detached_exception_report_test

using namespace detached_exception_report_test;

TEST_F(DetachedExceptionReport, ReportsEscapingException) {
    std::string out;
    {
        CapturedCerr cap;
        qb::io::async::coro_scheduler().spawn(throws_std());
        pump();
        out = cap.str();
    }
    EXPECT_NE(out.find("escaping-marker-2f7a"), std::string::npos) << "the exception's message never reached stderr; got: " << out;
    EXPECT_NE(out.find("DISCARDED"), std::string::npos) << "the report did not say the exception was discarded; got: " << out;
}

TEST_F(DetachedExceptionReport, ReportsNonStdException) {
    std::string out;
    {
        CapturedCerr cap;
        qb::io::async::coro_scheduler().spawn(throws_non_std());
        pump();
        out = cap.str();
    }
    EXPECT_NE(out.find("not derived from std::exception"), std::string::npos) << "a non-std throw was not reported; got: " << out;
}

TEST_F(DetachedExceptionReport, SilentOnClean) {
    std::string out;
    {
        CapturedCerr cap;
        qb::io::async::coro_scheduler().spawn(completes_cleanly());
        pump();
        out = cap.str();
    }
    EXPECT_TRUE(out.empty()) << "a clean detached coroutine printed something; got: " << out;
}

TEST_F(DetachedExceptionReport, SilentOnCancellation) {
    std::string out;
    {
        CapturedCerr cap;
        qb::io::async::coro_scheduler().spawn(throws_cancelled());
        pump();
        out = cap.str();
    }
    EXPECT_TRUE(out.empty()) << "cancellation is teardown, not a failure, and must not be reported; got: " << out;
}

TEST_F(DetachedExceptionReport, AwaitedTaskStillRethrows) {
    std::string out;
    bool        threw = false;
    {
        CapturedCerr cap;
        auto         driver = [&]() -> qb::io::async::task<void> {
            try {
                (void) co_await throws_with_value();
            } catch (std::runtime_error const &e) {
                threw = (std::string{e.what()} == "owned-marker-91c3");
            }
        };
        qb::io::async::coro_scheduler().spawn(driver());
        pump();
        out = cap.str();
    }
    EXPECT_TRUE(threw) << "an AWAITED task must still propagate by throwing";
    // The driver caught it, so nothing escaped: the owned path must not also print.
    EXPECT_TRUE(out.empty()) << "an awaited-and-caught exception was also reported; got: " << out;
}
