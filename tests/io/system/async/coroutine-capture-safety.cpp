/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/async/coroutine-capture-safety.cpp
 * @brief Coroutine value/lifetime safety — by-value capture isolation, move-only types, RAII
 *        destructor ordering (including on exception and on cancellation), exception propagation.
 *
 * These observe capture/lifetime semantics *across a `sleep` suspension*, so they need the scheduler
 * and are SYSTEM tier. This is the lifetime-correctness core extracted from
 * coroutine/test-coroutine-safety.cpp; the alloc-pressure half moved to
 * system/async/coroutine-memory-pressure.cpp and the concurrency half to
 * system/async/scheduler-stress.cpp. Waits use the shared bounded pump `qb::io::test::pump_until`.
 *
 * What it proves:
 *   - by-value lambda captures (int / std::string / a struct-with-vector) are isolated from a later
 *     mutation of the original — the captured snapshot survives the post-spawn mutation;
 *   - move-only types work as `task<T>` return values and as init-captures (`unique_ptr`);
 *   - a `shared_ptr` move-captured into the frame keeps its resource alive across suspension (the
 *     real-world footgun: the frame, not the caller, must own the lifetime) — NEW;
 *   - exceptions thrown inside the coroutine (from a plain call and from an inner task) are caught
 *     with the right message and an ordered log;
 *   - a stack RAII guard destructs after normal completion, after an inner throw, AND after the
 *     coroutine is *cancelled mid-flight* via `cancellable_sleep` (the cancellation-unwind path) — NEW.
 */

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <qb/io/async/coroutine.h>

#include "../../shared/coroutine_test_support.h"

using namespace qb::io::async;
using namespace std::chrono_literals;

namespace {

class CoroutineCaptureSafety : public ::testing::Test {
protected:
    void
    SetUp() override {
        qb::io::test::reset_async_context();
    }
    void
    TearDown() override {
        qb::io::async::listener::current.reset_coro_scheduler();
        qb::io::async::listener::current.clear();
    }
};

} // namespace

// ---------------------------------------------------------------------------
// By-value capture isolation
// ---------------------------------------------------------------------------

TEST_F(CoroutineCaptureSafety, IntCaptureIsolatedFromLaterMutation) {
    std::atomic<int>  observed{-1};
    std::atomic<bool> done{false};
    int               local_var = 42;

    coro_scheduler().spawn([local_var, &observed, &done]() -> task<void> {
        co_await sleep(5ms);
        observed = local_var; // must still be 42, not the post-spawn 999
        done     = true;
    });

    local_var = 999; // mutate the original after the capture

    EXPECT_TRUE(qb::io::test::pump_until([&] { return done.load(); })) << "capture coroutine never ran";
    EXPECT_EQ(observed.load(), 42);
}

TEST_F(CoroutineCaptureSafety, StringCaptureIsolatedFromLaterMutation) {
    std::atomic<bool> success{false};
    std::atomic<bool> done{false};
    std::string       message = "original";

    coro_scheduler().spawn([message, &success, &done]() -> task<void> {
        co_await sleep(10ms);
        success = (message == "original");
        done    = true;
    });

    message = "modified";

    EXPECT_TRUE(qb::io::test::pump_until([&] { return done.load(); })) << "string-capture coroutine never ran";
    EXPECT_TRUE(success.load());
}

TEST_F(CoroutineCaptureSafety, ComplexObjectCaptureCopiesAllFields) {
    struct Data {
        int              id;
        std::string      name;
        std::vector<int> values;
    };

    std::atomic<bool> success{false};
    std::atomic<bool> done{false};
    Data              data{42, "test", {1, 2, 3}};

    coro_scheduler().spawn([data, &success, &done]() -> task<void> {
        co_await sleep(5ms);
        success = (data.id == 42 && data.name == "test" && data.values.size() == 3u);
        done    = true;
    });

    data.id   = 999;
    data.name = "modified";

    EXPECT_TRUE(qb::io::test::pump_until([&] { return done.load(); })) << "complex-capture coroutine never ran";
    EXPECT_TRUE(success.load());
}

// ---------------------------------------------------------------------------
// Move-only types
// ---------------------------------------------------------------------------

TEST_F(CoroutineCaptureSafety, MoveOnlyReturnValue) {
    struct MoveOnlyResource {
        std::unique_ptr<int> data;
        explicit MoveOnlyResource(int v)
            : data(std::make_unique<int>(v)) {}
        MoveOnlyResource(MoveOnlyResource &&)                 = default;
        MoveOnlyResource &operator=(MoveOnlyResource &&)      = default;
        MoveOnlyResource(const MoveOnlyResource &)            = delete;
        MoveOnlyResource &operator=(const MoveOnlyResource &) = delete;
    };

    std::atomic<int>  result{0};
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        auto producer = []() -> task<MoveOnlyResource> {
            co_await sleep(10ms);
            co_return MoveOnlyResource{42};
        };
        auto resource = co_await producer();
        if (resource.data && *resource.data == 42)
            result = 1;
        done = true;
    });

    EXPECT_TRUE(qb::io::test::pump_until([&] { return done.load(); })) << "move-only producer never completed";
    EXPECT_EQ(result.load(), 1);
}

TEST_F(CoroutineCaptureSafety, MoveOnlyInitCapture) {
    std::atomic<bool> success{false};
    std::atomic<bool> done{false};

    coro_scheduler().spawn([ptr = std::make_unique<int>(42), &success, &done]() -> task<void> {
        co_await sleep(5ms);
        success = (ptr && *ptr == 42);
        done    = true;
    });

    EXPECT_TRUE(qb::io::test::pump_until([&] { return done.load(); })) << "move-only init-capture coroutine never ran";
    EXPECT_TRUE(success.load());
}

TEST_F(CoroutineCaptureSafety, SharedPtrMoveCaptureKeepsResourceAliveAcrossSuspension) {
    // The footgun: capturing a shared_ptr by move into the frame must keep the pointee alive across
    // a suspension even after every OTHER owner has dropped its reference. We drop the local owner
    // immediately after spawning; only the frame's captured copy keeps use_count >= 1.
    std::atomic<long> observed_use_count{-1};
    std::atomic<int>  observed_value{0};
    std::atomic<bool> done{false};

    auto resource = std::make_shared<int>(7);
    coro_scheduler().spawn([res = resource, &observed_use_count, &observed_value, &done]() -> task<void> {
        co_await sleep(10ms);                 // suspend with only the frame's copy alive
        observed_use_count = res.use_count(); // >= 1 (the frame's own copy)
        observed_value     = *res;            // pointee must still be valid
        done               = true;
    });
    resource.reset(); // drop the local owner immediately — frame must own the lifetime now

    EXPECT_TRUE(qb::io::test::pump_until([&] { return done.load(); })) << "shared_ptr-capture coroutine never ran";
    EXPECT_GE(observed_use_count.load(), 1) << "the frame must keep the shared resource alive";
    EXPECT_EQ(observed_value.load(), 7);
}

// ---------------------------------------------------------------------------
// Exception propagation inside coroutines
// ---------------------------------------------------------------------------

TEST_F(CoroutineCaptureSafety, ExceptionFromPlainCallIsCaught) {
    std::atomic<bool> caught{false};
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        try {
            []() {
                throw std::runtime_error("captured error");
            }();
        } catch (const std::runtime_error &e) {
            caught = std::string(e.what()) == "captured error";
        }
        done = true;
        co_return; // make this a coroutine: the lambda's task<void> body has no other co_*
    });

    EXPECT_TRUE(qb::io::test::pump_until([&] { return done.load(); })) << "exception coroutine never ran";
    EXPECT_TRUE(caught.load());
}

TEST_F(CoroutineCaptureSafety, NestedInnerTaskThrowCaughtWithOrderedLog) {
    std::vector<std::string> log;
    std::atomic<bool>        done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        auto inner = [&log]() -> task<int> {
            log.push_back("inner-start");
            throw std::runtime_error("inner error");
            co_return 1;
        };
        log.push_back("outer-start");
        try {
            co_await inner();
        } catch (const std::runtime_error &e) {
            log.push_back("outer-caught: " + std::string(e.what()));
        }
        done = true;
    });

    EXPECT_TRUE(qb::io::test::pump_until([&] { return done.load(); })) << "nested-exception coroutine never ran";
    ASSERT_EQ(log.size(), 3u);
    EXPECT_EQ(log[0], "outer-start");
    EXPECT_EQ(log[1], "inner-start");
    EXPECT_NE(log[2].find("outer-caught"), std::string::npos);
}

// ---------------------------------------------------------------------------
// RAII destructor ordering — normal, on-throw, and on-cancel-unwind
// ---------------------------------------------------------------------------

namespace {
struct FlagGuard {
    std::atomic<bool> &destroyed;
    explicit FlagGuard(std::atomic<bool> &d)
        : destroyed(d) {
        destroyed = false;
    }
    ~FlagGuard() {
        destroyed = true;
    }
};
} // namespace

TEST_F(CoroutineCaptureSafety, RAIIGuardDestructsAfterNormalCompletion) {
    std::atomic<bool> guard_destroyed{false};
    std::atomic<bool> completed{false};

    coro_scheduler().spawn([&]() -> task<void> {
        FlagGuard guard{guard_destroyed};
        co_await sleep(10ms);
        completed = true;
    });

    EXPECT_FALSE(guard_destroyed.load()) << "guard destroyed before the coroutine even started";

    EXPECT_TRUE(qb::io::test::pump_until([&] { return completed.load(); })) << "guarded coroutine never completed";
    EXPECT_TRUE(qb::io::test::pump_until([&] { return guard_destroyed.load(); })) << "guard never destructed";
}

TEST_F(CoroutineCaptureSafety, RAIIGuardDestructsWhenInnerTaskThrows) {
    std::atomic<bool> guard_destroyed{false};
    std::atomic<bool> exception_caught{false};
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        auto inner = [&]() -> task<void> {
            FlagGuard guard{guard_destroyed};
            co_await sleep(5ms);
            throw std::runtime_error("error");
            co_return;
        };
        try {
            co_await inner();
        } catch (...) {
            exception_caught = true;
        }
        done = true;
    });

    EXPECT_TRUE(qb::io::test::pump_until([&] { return done.load(); })) << "throwing-guard coroutine never finished";
    EXPECT_TRUE(guard_destroyed.load()) << "guard must destruct even when the inner task throws";
    EXPECT_TRUE(exception_caught.load());
}

TEST_F(CoroutineCaptureSafety, RAIIGuardDestructsOnCancellationUnwind) {
    // The cancellation-unwind path: a stack guard must run its destructor when a coroutine is
    // unwound by `cancelled_error` thrown out of `cancellable_sleep`, not only on normal/throw exit.
    std::atomic<bool>  guard_destroyed{false};
    std::atomic<bool>  cancelled{false};
    std::atomic<bool>  done{false};
    cancellation_token token;

    coro_scheduler().spawn([&]() -> task<void> {
        FlagGuard guard{guard_destroyed};
        try {
            co_await cancellable_sleep(5000ms, token); // parked until cancelled
        } catch (const cancelled_error &) {
            cancelled = true;
        }
        done = true;
    });

    // Ensure it is parked (guard constructed, not yet destroyed), then cancel mid-flight.
    EXPECT_TRUE(qb::io::test::pump_until([&] { return token.get_state()->callbacks.size() == 1u; }))
        << "guarded coroutine never parked on cancellable_sleep";
    EXPECT_FALSE(guard_destroyed.load());
    token.cancel();

    EXPECT_TRUE(qb::io::test::pump_until([&] { return done.load(); })) << "cancelled coroutine never unwound";
    EXPECT_TRUE(cancelled.load());
    EXPECT_TRUE(guard_destroyed.load()) << "the stack guard must destruct on the cancellation-unwind path";
}
