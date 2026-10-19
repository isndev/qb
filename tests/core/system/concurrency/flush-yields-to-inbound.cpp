/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/concurrency/flush-yields-to-inbound.cpp
 * @brief A core blocked on a peer's full mailbox drains its own inbound work instead of burning
 *        the pass in the flush's backoff (Huly QB-979).
 *
 * The shape savina/barber found: one core holds its peer's ring full for the length of a long
 * handler, and the peer -- which has work of its own queued -- used to spend every pass retrying
 * the head of its outbound pipe up to 512 times before it got to that work. Core 1 floods core 0
 * while core 0 waits in its first handler, and runs a self-addressed Step chain beside the flood.
 *
 * The sink's consumer index stays fixed until the flooder releases it. The first Step observing
 * that hold follows a flush that may predate it, so two MORE complete passes establish the stall.
 * The following 64 passes must count one refused attempt each; disabling the stall rule counts
 * 513 each. The 60-second watchdog only releases a failed harness, never decides this oracle.
 * After release the flood must arrive whole and in order. Distinct sink pass numbers count its
 * actual non-empty receive batches for diagnostics, without assuming those batches fill a ring.
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include <gtest/gtest.h>
#include <qb/main.h>

namespace flush_yields_to_inbound_test {

using namespace std::chrono_literals;

struct Flood : qb::Event {
    std::uint64_t seq;
    explicit Flood(std::uint64_t s) noexcept
        : seq(s) {}
};
struct Step : qb::Event {};
struct Done : qb::Event {
    std::uint64_t received;
    explicit Done(std::uint64_t r) noexcept
        : received(r) {}
};

constexpr std::uint64_t kFlood          = 4096;
constexpr std::uint64_t kWarmupPasses   = 2;
constexpr std::uint64_t kMeasuredPasses = 64;

struct TestState {
    const qb::mono_time     deadline = qb::mono_now() + 60s;
    std::mutex              mutex;
    std::condition_variable condition;
    bool                    released = false; // protected by mutex
    std::atomic<bool>       held{false};
    std::atomic<bool>       timed_out{false};

    // Each result has one actor-thread writer and is read by the test only after join.
    qb::CoreStats flooder{};
    qb::CoreStats phase_begin{};
    qb::CoreStats phase_end{};
    std::uint64_t received           = 0;
    std::uint64_t in_order           = 0;
    std::uint64_t receive_batches    = 0;
    std::uint64_t steps              = 0;
    std::uint64_t held_observed_pass = 0;
    std::uint64_t measured_steps     = 0;
    std::uint64_t bad_intervals      = 0;
    bool          phase_complete     = false;
    bool          done_received      = false;

    void
    release_sink() {
        {
            std::lock_guard lock(mutex);
            released = true;
        }
        condition.notify_one();
    }

    void
    hold_sink() {
        std::unique_lock lock(mutex);
        held.store(true, std::memory_order_release);
        const bool awakened = condition.wait_until(lock, deadline, [this] { return released; });
        held.store(false, std::memory_order_release);
        lock.unlock();
        if (!awakened) {
            timed_out.store(true, std::memory_order_relaxed);
            qb::Main::stop();
        }
    }
};

// Core 0: copy-out has already freed the first batch's ring slots before this handler waits.
// Another ring can fill behind it, but the entire flood cannot fit in those two batches.
class Sink : public qb::Actor {
    TestState    *_state;
    std::uint64_t _last_receive_pass = 0;

public:
    explicit Sink(TestState *state) noexcept
        : _state(state) {}

    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Flood>(*this);
        co_return true;
    }
    void
    on(Flood const &flood) {
        // One producer, one copy-out per active pass. A wrapped copy-out is still one batch.
        // Sink dies only after every Flood, so none of these callbacks belong to shutdown drains.
        const auto pass = getCoreStats().loop_passes;
        if (pass != _last_receive_pass) {
            ++_state->receive_batches;
            _last_receive_pass = pass;
        }
        if (flood.seq == _state->received)
            ++_state->in_order;
        if (_state->received++ == 0)
            _state->hold_sink();
        if (_state->received == kFlood) {
            push<Done>(flood.getSource(), _state->received);
            kill();
        }
    }
};

// Core 1: the flood, pushed before the loop's first pass, and a self-addressed chain that makes
// every pass of this core one with inbound work pending at flush time.
class Flooder : public qb::Actor {
    qb::ActorId   _sink;
    TestState    *_state;
    qb::Main     *_main;
    qb::CoreStats _previous{};
    std::uint64_t _first_held_pass = 0;
    bool          _observed_hold   = false;
    bool          _baseline_taken  = false;
    bool          _done            = false;

public:
    explicit Flooder(qb::ActorId sink, TestState *state, qb::Main *main) noexcept
        : _sink(sink)
        , _state(state)
        , _main(main) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Step>(*this);
        registerEvent<Done>(*this);
        for (std::uint64_t i = 0; i < kFlood; ++i)
            push<Flood>(_sink, i);
        push<Step>(id());
        co_return true;
    }
    void
    on(Step const &) {
        ++_state->steps;
        if (_done) {
            kill();
            return;
        }

        // A failed peer must not leave this self-chain alive forever. The deadline is a
        // cleanup watchdog, separate from the pass-count oracle below.
        const bool expired = qb::mono_now() >= _state->deadline;
        if (_main->hasError() || expired) {
            if (expired)
                _state->timed_out.store(true, std::memory_order_relaxed);
            _state->release_sink();
            qb::Main::stop();
            kill();
            return;
        }

        if (!_state->phase_complete && _state->held.load(std::memory_order_acquire)) {
            const auto stats = getCoreStats();
            if (!_observed_hold) {
                _observed_hold             = true;
                _first_held_pass           = stats.loop_passes;
                _state->held_observed_pass = stats.loop_passes;
            } else if (!_baseline_taken) {
                // The first observation's preceding flush may predate held. Count two complete
                // passes AFTER that first observation before recording the baseline.
                if (stats.loop_passes - _first_held_pass >= kWarmupPasses) {
                    _state->phase_begin = stats;
                    _previous           = stats;
                    _baseline_taken     = true;
                }
            } else {
                const auto passes  = stats.loop_passes - _previous.loop_passes;
                const auto blocked = stats.sends_blocked - _previous.sends_blocked;
                if (passes != 1 || blocked != passes)
                    ++_state->bad_intervals;
                _previous         = stats;
                _state->phase_end = stats;
                if (++_state->measured_steps == kMeasuredPasses) {
                    _state->phase_complete = true;
                    _state->release_sink();
                }
            }
        }
        push<Step>(id());
    }
    void
    on(Done const &) {
        // Read in the handler of the sink's answer: every Flood was published by then, so the
        // counters below are the whole blocked phase and nothing of the shutdown drain.
        _state->flooder       = getCoreStats();
        _state->done_received = true;
        _done                 = true;
    }
};

TEST(FlushYieldsToInbound, ABlockedPassTriesTheHeadOnceWhenInboundWorkIsPending) {
    if (std::thread::hardware_concurrency() < 2u)
        GTEST_SKIP() << "requires-multicore: the flood must cross a mailbox";
    TestState  state;
    qb::Main   main;
    const auto sink = main.addActor<Sink>(0, &state);
    main.addActor<Flooder>(1, sink, &state, &main);
    main.start(false);
    // The calling thread can own either worker. Release a waiting sink before joining;
    // only hasError() is safe to inspect before both workers have finished.
    state.release_sink();
    if (main.hasError())
        qb::Main::stop();
    main.join();

    SCOPED_TRACE(::testing::Message() << "receive batches=" << state.receive_batches << ", flooder passes=" << state.flooder.loop_passes
                                      << ", blocked=" << state.flooder.sends_blocked << ", steps=" << state.steps);
    ASSERT_FALSE(main.hasError());
    ASSERT_FALSE(state.timed_out.load(std::memory_order_relaxed)) << "60-second harness watchdog expired";
    ASSERT_TRUE(state.phase_complete) << "the held sink must permit the complete count-based observation window";
    ASSERT_TRUE(state.done_received);

    const auto phase_passes  = state.phase_end.loop_passes - state.phase_begin.loop_passes;
    const auto phase_blocked = state.phase_end.sends_blocked - state.phase_begin.sends_blocked;
    RecordProperty("receive_batches", std::to_string(state.receive_batches));
    RecordProperty("phase_passes", std::to_string(phase_passes));
    RecordProperty("phase_blocked", std::to_string(phase_blocked));
    RecordProperty("total_passes", std::to_string(state.flooder.loop_passes));
    RecordProperty("total_blocked", std::to_string(state.flooder.sends_blocked));
    RecordProperty("warmup_passes", std::to_string(state.phase_begin.loop_passes - state.held_observed_pass));
    EXPECT_GE(state.phase_begin.loop_passes - state.held_observed_pass, kWarmupPasses);
    EXPECT_EQ(state.measured_steps, kMeasuredPasses);
    EXPECT_EQ(phase_passes, kMeasuredPasses);
    EXPECT_EQ(phase_blocked, phase_passes)
        << "a fixed consumer index and pending Step require one refused attempt per pass after the two warmup passes";
    EXPECT_EQ(state.bad_intervals, 0u) << "each observed pass must satisfy the same invariant";
    EXPECT_EQ(state.received, kFlood) << "the flood arrives whole: the early bail drops nothing";
    EXPECT_EQ(state.in_order, kFlood) << "and in order: the pipe is FIFO and the bail keeps the head where it was";
    EXPECT_EQ(state.flooder.events_dropped, 0u);
    EXPECT_GT(state.flooder.sends_blocked, 0u) << "the sink's ring must have been found full, or nothing was tested";
    EXPECT_GT(state.receive_batches, 0u);
    EXPECT_GT(state.steps, 0u);
}

} // namespace flush_yields_to_inbound_test
