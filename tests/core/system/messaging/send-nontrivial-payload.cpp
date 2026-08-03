/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/messaging/send-nontrivial-payload.cpp
 * @brief `Actor::send()` disposes a payload-owning event exactly once, on every placement path.
 *
 * `send()` is documented as taking a trivially-destructible event — a deliberately conservative
 * guideline, since an unordered fire-and-forget message is meant to stay small and POD-ish. It is
 * NOT statically enforced, though: the only `static_assert` in `fill_event` fires for `EventQOS0`,
 * which is the type the engine may genuinely DROP (the cross-core flush discards `qos == 0` on
 * backpressure without disposing it). So a payload-owning event does reach `send()` in practice,
 * and this file pins what the engine then owes it.
 *
 * `send()` differs from `push()` in exactly one respect that matters here: it allocates via
 * `pipe::allocate()`, which prefers the FREED PREFIX of the buffer (hence the unordered delivery)
 * and falls back to `allocate_back()`, and on the cross-core path it hands the event straight to
 * the destination ring and releases the pipe slot with `pipe::free()` — a cursor rewind with NO
 * destructor call. That is correct only if the bytes really were relocated. Both halves run here:
 *
 *   - same-core `send`: never relocated, drained through the mono pipe and routed;
 *   - cross-core `send`: relocated into the peer's MPSC ring by `try_send`, then `free()`d here
 *     and destroyed on the far side;
 *   - `send` interleaved with `push` to the same destination, so the pipe alternates between
 *     front and back placement and `pipe::free()`'s `_flag_front` bookkeeping is exercised.
 *
 * The oracle is per-object: the payload counts its own live instances and owns a real allocation,
 * so a missed destructor shows up as a non-zero balance after `Main::join()` (and, under ASan, as
 * a leak report), and a double destructor shows up as a negative one.
 */

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <type_traits>

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/main.h>

using namespace std::chrono_literals;

namespace {

std::atomic<int> g_live{0};

/// Owns heap; fully movable (the ordinary case) — a moved-from instance still needs its dtor.
struct Payload {
    std::string *heap = nullptr;

    Payload()
        : heap(new std::string(96, 'p')) {
        g_live.fetch_add(1, std::memory_order_relaxed);
    }
    Payload(Payload const &o)
        : heap(new std::string(*o.heap)) {
        g_live.fetch_add(1, std::memory_order_relaxed);
    }
    Payload(Payload &&o) noexcept
        : heap(o.heap) {
        o.heap = nullptr;
        g_live.fetch_add(1, std::memory_order_relaxed);
    }
    Payload &operator=(Payload const &) = delete;
    Payload &operator=(Payload &&)      = delete;
    ~Payload() {
        delete heap;
        g_live.fetch_sub(1, std::memory_order_relaxed);
    }
};

struct Unordered : qb::Event {
    Payload payload{};
    int     seq = 0;
    explicit Unordered(int s)
        : seq(s) {}
};

struct Ordered : qb::Event {
    Payload payload{};
    int     seq = 0;
    explicit Ordered(int s)
        : seq(s) {}
};

static_assert(!std::is_trivially_destructible_v<Unordered>,
              "the point of this file is a send() whose event is NOT trivially destructible");

constexpr int kCount = 200;

std::atomic<int> g_received{0};

class Sink : public qb::Actor {
    int _remaining;

public:
    explicit Sink(int expected)
        : _remaining(expected) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Unordered>(*this);
        registerEvent<Ordered>(*this);
        co_return true;
    }
    void
    on(Unordered const &e) {
        (void) e;
        tick();
    }
    void
    on(Ordered const &e) {
        (void) e;
        tick();
    }

private:
    void
    tick() {
        g_received.fetch_add(1, std::memory_order_relaxed);
        if (--_remaining <= 0)
            kill();
    }
};

/// Emits `kCount` events, alternating `send` (front-or-back placement) and `push` (back only),
/// so the pipe's placement bookkeeping is exercised in both directions.
class Emitter : public qb::Actor {
    qb::ActorId _sink;

public:
    explicit Emitter(qb::ActorId sink)
        : _sink(sink) {}
    qb::io::async::task<bool>
    onInit() override {
        for (int i = 0; i < kCount; ++i) {
            if (i % 2 == 0)
                send<Unordered>(_sink, i);
            else
                push<Ordered>(_sink, i);
        }
        kill();
        co_return true;
    }
};

void
run(qb::CoreId sink_core) {
    g_live.store(0, std::memory_order_relaxed);
    g_received.store(0, std::memory_order_relaxed);
    {
        qb::Main   main;
        const auto sink = main.addActor<Sink>(sink_core, kCount);
        main.addActor<Emitter>(0, sink);
        main.start(false);
        main.join();
        EXPECT_FALSE(main.hasError());
    }
    EXPECT_EQ(g_received.load(std::memory_order_relaxed), kCount);
    EXPECT_EQ(g_live.load(std::memory_order_relaxed), 0)
        << "a non-trivially-destructible event passed to send() was not destroyed exactly once";
}

TEST(SendNonTrivialPayload, SameCoreDestroysEveryPayloadExactlyOnce) {
    run(0);
}

TEST(SendNonTrivialPayload, CrossCoreDestroysEveryPayloadExactlyOnce) {
    if (std::thread::hardware_concurrency() < 2)
        GTEST_SKIP() << "requires-multicore: needs a distinct destination core to exercise try_send relocation";
    run(1);
}

} // namespace
