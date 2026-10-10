/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0
 */

/**
 * @file system/messaging/handler-throw.cpp
 * @brief An exception from event routing stops its core and releases owned events.
 *
 * Routes through the ordinary, default, activation-replay and death-watch
 * dispatch paths. A throwing handler is never resumed. Events already removed
 * from a pipe or mailbox batch are disposed exactly once, and Main reports the
 * error after the failing core has terminated.
 */

#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/main.h>

#include "../../shared/InitFixtures.h"

using namespace std::chrono_literals;

namespace handler_throw_test {

std::atomic<int>         g_payloads{0};
std::atomic<int>         g_calls{0};
std::atomic<int>         g_actors{0};
std::atomic<std::size_t> g_faulting_width{0};
std::atomic<int>         g_payloads_at_throw{-1};
std::atomic<bool>        g_stash_started{false};
std::atomic<int>         g_stash_calls{0};
std::atomic<bool>        g_wait_for_peer{false};
std::atomic<bool>        g_peer_entered_workflow{false};

void
reset() {
    g_payloads          = 0;
    g_calls             = 0;
    g_actors            = 0;
    g_faulting_width    = 0;
    g_payloads_at_throw = -1;
    g_stash_started     = false;
    g_stash_calls       = 0;
}

struct PayloadDeleter {
    void
    operator()(int *p) const noexcept {
        ++g_payloads;
        delete p;
    }
};

struct OwnedEvent : qb::Event {
    std::unique_ptr<int, PayloadDeleter> payload;
    // The base's tail padding can absorb the pointer on g++/clang. Keep this event wider than
    // one bucket on every ABI so an interrupted mailbox walk must honor bucket_size.
    char padding[QB_LOCKFREE_EVENT_BUCKET_BYTES]{};
    explicit OwnedEvent(int n)
        : payload(new int(n)) {}
};
static_assert(qb::allocator::getItemSize<OwnedEvent, EventBucket>() > 1u);

class ThrowsOnOwnedEvent : public qb::Actor {
public:
    ~ThrowsOnOwnedEvent() override {
        ++g_actors;
    }
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<OwnedEvent>(*this);
        push<OwnedEvent>(id(), 1);
        push<OwnedEvent>(id(), 2); // already in the same self-pipe batch
        co_return true;
    }
    void
    on(OwnedEvent const &) {
        if (g_wait_for_peer.load()) {
            bool entered = g_peer_entered_workflow.load(std::memory_order_acquire);
            while (!entered) {
                g_peer_entered_workflow.wait(false);
                entered = g_peer_entered_workflow.load(std::memory_order_acquire);
            }
        }
        ++g_calls;
        throw std::runtime_error("custom event handler failed");
    }
};

TEST(HandlerThrow, CustomUnicastDisposesEntireBatchAndReportsError) {
    reset();
    qb::Main main;
    main.addActor<ThrowsOnOwnedEvent>(0);
    main.start(false);
    main.join();
    EXPECT_TRUE(main.hasError());
    EXPECT_EQ(g_calls.load(), 1);
    EXPECT_EQ(g_payloads.load(), 2);
    EXPECT_EQ(g_actors.load(), 1);
}

std::atomic<int> g_peer_ticks_after_failure{0};

class ErrorObservingPeer final
    : public qb::Actor
    , public qb::ICallback {
    qb::Main *_main;

public:
    explicit ErrorObservingPeer(qb::Main *main)
        : _main(main) {}
    ~ErrorObservingPeer() final {
        ++g_actors;
    }
    qb::io::async::task<bool>
    onInit() override {
        registerCallback(*this);
        co_return true;
    }
    void
    on(qb::LoopEvent const &) override {
        g_peer_entered_workflow.store(true, std::memory_order_release);
        g_peer_entered_workflow.notify_all();
        if (_main->hasError()) {
            ++g_peer_ticks_after_failure;
            kill();
        }
    }
};

TEST(HandlerThrow, AHealthyPeerCoreContinuesAfterTheOtherCoreFails) {
    reset();
    g_peer_ticks_after_failure = 0;
    g_peer_entered_workflow    = false;
    g_wait_for_peer            = true;
    qb::Main main;
    main.addActor<ThrowsOnOwnedEvent>(0);
    main.addActor<ErrorObservingPeer>(1, &main);

    std::promise<void> finished;
    auto               finished_future = finished.get_future();
    std::atomic<bool>  watchdog_stopped_peer{false};
    std::thread        watchdog([&] {
        if (finished_future.wait_for(5s) != std::future_status::ready) {
            watchdog_stopped_peer   = true;
            g_peer_entered_workflow = true;
            g_peer_entered_workflow.notify_all(); // release a thrower if the peer never ticked
            qb::Main::stop();                     // bounded escape if the failure is not published
        }
    });
    main.start(true);
    main.join();
    g_wait_for_peer = false;
    finished.set_value();
    watchdog.join();

    EXPECT_FALSE(watchdog_stopped_peer.load());
    EXPECT_TRUE(main.hasError());
    EXPECT_EQ(g_peer_ticks_after_failure.load(), 1);
    EXPECT_EQ(g_payloads.load(), 2);
    EXPECT_EQ(g_actors.load(), 2);
}

class ThrowsOnDefault : public qb::Actor {
public:
    ~ThrowsOnDefault() override {
        ++g_actors;
    }
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<qb::KillEvent>(*this);
        push<qb::KillEvent>(id());
        co_return true;
    }
    void
    on(qb::KillEvent const &) {
        ++g_calls;
        throw std::runtime_error("default event handler failed");
    }
};

TEST(HandlerThrow, DefaultEventOverrideReportsError) {
    reset();
    qb::Main main;
    main.addActor<ThrowsOnDefault>(0);
    main.start(false);
    main.join();
    EXPECT_TRUE(main.hasError());
    EXPECT_EQ(g_calls.load(), 1);
    EXPECT_EQ(g_actors.load(), 1);
}

class ActivatingThrower : public qb::Actor {
public:
    ~ActivatingThrower() override {
        ++g_actors;
    }
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<OwnedEvent>(*this);
        co_await context().sleep(20ms);
        co_return true;
    }
    void
    on(OwnedEvent const &) {
        ++g_calls;
        throw std::runtime_error("activation replay failed");
    }
};

class SendsToActivating : public qb::Actor {
    qb::ActorId _target;

public:
    explicit SendsToActivating(qb::ActorId target)
        : _target(target) {}
    qb::io::async::task<bool>
    onInit() override {
        push<OwnedEvent>(_target, 1);
        push<OwnedEvent>(_target, 2);
        kill();
        co_return true;
    }
};

TEST(HandlerThrow, ActivationReplayDisposesRemainingStash) {
    reset();
    qb::Main main;
    auto     id = main.addActor<ActivatingThrower>(0);
    main.addActor<SendsToActivating>(0, id);
    main.start(false);
    main.join();
    EXPECT_TRUE(main.hasError());
    EXPECT_EQ(g_calls.load(), 1);
    EXPECT_EQ(g_payloads.load(), 2);
    EXPECT_EQ(g_actors.load(), 1);
}

std::atomic<int> g_targets{0};

class WatchedTarget : public qb::Actor {
public:
    ~WatchedTarget() override {
        ++g_targets;
    }
};

class ThrowsOnDown : public qb::Actor {
    qb::ActorId _target;

public:
    explicit ThrowsOnDown(qb::ActorId target)
        : _target(target) {}
    ~ThrowsOnDown() override {
        ++g_actors;
    }
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<qb::DownEvent>(*this);
        watch(_target);
        push<qb::KillEvent>(_target);
        co_return true;
    }
    void
    on(qb::DownEvent const &) {
        ++g_calls;
        throw std::runtime_error("death-watch handler failed");
    }
};

TEST(HandlerThrow, DeathWatchHandlerReportsErrorAfterTargetDestruction) {
    reset();
    g_targets = 0;
    qb::Main main;
    auto     target = main.addActor<WatchedTarget>(0);
    main.addActor<ThrowsOnDown>(0, target);
    main.start(false);
    main.join();
    EXPECT_TRUE(main.hasError());
    EXPECT_EQ(g_targets.load(), 1);
    EXPECT_EQ(g_calls.load(), 1);
    EXPECT_EQ(g_actors.load(), 1);
}

std::atomic<bool> g_broadcast_throw{false};

struct BroadcastEvent : qb::Event {};

class BroadcastReceiver : public qb::Actor {
public:
    ~BroadcastReceiver() override {
        ++g_actors;
    }
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<BroadcastEvent>(*this);
        push<BroadcastEvent>(qb::BroadcastId(getIndex()));
        co_return true;
    }
    void
    on(BroadcastEvent const &) {
        ++g_calls;
        if (g_broadcast_throw.load())
            throw std::runtime_error("broadcast handler failed");
        kill();
    }
};

TEST(HandlerThrow, BroadcastSnapshotIsRestoredAfterException) {
    reset();
    g_broadcast_throw = true;
    {
        qb::Main main;
        main.addActor<BroadcastReceiver>(0);
        main.start(false);
        main.join();
        EXPECT_TRUE(main.hasError());
    }
    g_broadcast_throw = false;
    {
        qb::Main main;
        main.addActor<BroadcastReceiver>(0);
        main.start(false); // same caller thread and thread_local broadcast snapshot
        main.join();
        EXPECT_FALSE(main.hasError());
    }
    EXPECT_EQ(g_calls.load(), 2);
    EXPECT_EQ(g_actors.load(), 2);
}

std::atomic<int>  g_producers_flushed{0};
std::atomic<bool> g_receiver_waiting{false};

class StashedMailboxTarget final : public qb::Actor {
public:
    ~StashedMailboxTarget() final {
        ++g_actors;
    }
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<OwnedEvent>(*this);
        g_stash_started.store(true, std::memory_order_release);
        co_await context().until_cancelled(); // only core teardown releases this activation gate
        co_return true;
    }
    void
    on(OwnedEvent const &) {
        ++g_stash_calls; // a stashed event must not reach the handler before activation
    }
};

class ThrowsFromMailbox final
    : public qb::Actor
    , public qb::ICallback {
public:
    ~ThrowsFromMailbox() final {
        ++g_actors;
    }
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<OwnedEvent>(*this);
        registerCallback(*this);
        co_return true;
    }
    void
    on(qb::LoopEvent const &) override {
        g_receiver_waiting.store(true, std::memory_order_release);
        g_receiver_waiting.notify_all();
        int n = g_producers_flushed.load(std::memory_order_acquire);
        while (n < 2) {
            g_producers_flushed.wait(n);
            n = g_producers_flushed.load(std::memory_order_acquire);
        }
    }
    void
    on(OwnedEvent const &event) {
        g_faulting_width.store(event.getSize() / sizeof(EventBucket), std::memory_order_relaxed);
        g_payloads_at_throw.store(g_payloads.load(), std::memory_order_relaxed);
        ++g_calls;
        throw std::runtime_error("mailbox batch handler failed");
    }
};

class MailboxProducer final
    : public qb::Actor
    , public qb::ICallback {
    qb::ActorId _receiver;
    qb::ActorId _stash;
    int         _ticks = 0;

public:
    MailboxProducer(qb::ActorId receiver, qb::ActorId stash)
        : _receiver(receiver)
        , _stash(stash) {}
    qb::io::async::task<bool>
    onInit() override {
        registerCallback(*this);
        co_return true;
    }
    void
    on(qb::LoopEvent const &) override {
        if (++_ticks == 1) {
            bool waiting = g_receiver_waiting.load(std::memory_order_acquire);
            while (!waiting) {
                g_receiver_waiting.wait(false);
                waiting = g_receiver_waiting.load(std::memory_order_acquire);
            }
            push<OwnedEvent>(_stash, 0); // copied into the activating actor's stash, then retired in this batch
            push<OwnedEvent>(_receiver, 1);
            push<OwnedEvent>(_receiver, 2);
        } else if (_ticks == 2) {
            g_producers_flushed.fetch_add(1, std::memory_order_release);
            g_producers_flushed.notify_all();
            kill();
        }
    }
};

TEST(HandlerThrow, MailboxBatchesFromBothProducersDisposeAfterFirstThrow) {
    reset();
    g_producers_flushed = 0;
    g_receiver_waiting  = false;
    qb::test::ScopedDeadline deadline{0}; // a loaded host cannot expire the activation gate
    qb::Main                 main;
    auto                     receiver = main.addActor<ThrowsFromMailbox>(0);
    auto                     stash    = main.addActor<StashedMailboxTarget>(0);
    main.addActor<MailboxProducer>(1, receiver, stash);
    main.addActor<MailboxProducer>(2, receiver, stash);
    main.start(true);
    main.join();
    EXPECT_TRUE(main.hasError());
    EXPECT_EQ(g_producers_flushed.load(), 2);
    EXPECT_TRUE(g_stash_started.load()) << "the first event must target an activating actor";
    EXPECT_EQ(g_stash_calls.load(), 0) << "the first event must stay stashed until teardown";
    EXPECT_EQ(g_payloads_at_throw.load(), 0) << "the stashed event must still own its payload when the next handler throws";
    EXPECT_GT(g_faulting_width.load(), 1u) << "the faulting event must span multiple buckets";
    EXPECT_EQ(g_calls.load(), 1);
    EXPECT_EQ(g_payloads.load(), 6) << "stashed, faulting, later and unconsumed events must each be disposed once";
    EXPECT_EQ(g_actors.load(), 2);
}

} // namespace handler_throw_test
