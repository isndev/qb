/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/concurrency/qos1-backpressure.cpp
 * @brief QoS-1 deadlock recovery under heavy cross-core backpressure.
 *
 * Several source cores each push a heavy burst of QoS-1 events into a single sink. The sink's
 * mailbox fills faster than it drains, forcing every source core's `__flush_all__` into its
 * bounded backoff (spin → yield → bail+notify). The recovery path must lose nothing and never
 * livelock: the test asserts every event arrives (full FIFO), every per-source EOS arrives, and
 * the whole run finishes in bounded wall-clock time.
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/main.h>

namespace {

struct StressEvent : public qb::Event {
    std::uint32_t seq;
    explicit StressEvent(std::uint32_t s) noexcept : seq(s) {}
};
struct StressEosEvent : public qb::Event {};

std::atomic<std::uint32_t> stress_received{0};
std::atomic<std::uint32_t> stress_eos_received{0};
std::uint32_t              stress_eos_expected{0};

class StressSinkActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() final {
        registerEvent<StressEvent>(*this);
        registerEvent<StressEosEvent>(*this);
        co_return true;
    }
    void on(StressEvent const &) { stress_received.fetch_add(1, std::memory_order_relaxed); }
    void on(StressEosEvent const &) {
        // Kill only once every source has flushed its burst — guarantees all QoS-1 events were
        // counted (FIFO: the EOS lands strictly after this source's burst).
        if (stress_eos_received.fetch_add(1, std::memory_order_acq_rel) + 1 == stress_eos_expected)
            kill();
    }
};

class StressSourceActor : public qb::Actor {
    qb::ActorId   _sink;
    std::uint32_t _budget;

public:
    StressSourceActor(qb::ActorId sink, std::uint32_t budget) : _sink(sink), _budget(budget) {}
    qb::io::async::task<bool>
    onInit() final {
        for (std::uint32_t i = 0; i < _budget; ++i)
            push<StressEvent>(_sink, i);
        push<StressEosEvent>(_sink); // one EOS per source, after its burst
        kill();
        co_return true;
    }
};

} // namespace

TEST(DeadlockRecovery, QoS1HighBackpressureNoLivelock) {
    constexpr std::uint32_t kSources = 4;
    constexpr std::uint32_t kBurst   = 200'000;
    constexpr std::uint32_t kTotal   = kSources * kBurst;

    if (std::thread::hardware_concurrency() <= kSources)
        GTEST_SKIP() << "requires-multicore: needs " << (kSources + 1) << " cores for true cross-core backpressure";

    stress_received.store(0, std::memory_order_relaxed);
    stress_eos_received.store(0, std::memory_order_relaxed);
    stress_eos_expected = kSources;

    qb::Main main;
    auto     sink = main.core(0).addActor<StressSinkActor>();
    for (qb::CoreId c = 1; c <= static_cast<qb::CoreId>(kSources); ++c)
        main.core(c).addActor<StressSourceActor>(sink, kBurst);

    const auto t_start = std::chrono::steady_clock::now();
    main.start(false);
    main.join();
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t_start);

    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(stress_received.load(), kTotal)
        << "all QoS-1 events must be delivered — recovery must preserve full FIFO, no drops";
    EXPECT_EQ(stress_eos_received.load(), kSources) << "every source's EOS marker must arrive";
    EXPECT_LT(elapsed.count(), 60) << "deadlock recovery must terminate in bounded time (no livelock)";
}
