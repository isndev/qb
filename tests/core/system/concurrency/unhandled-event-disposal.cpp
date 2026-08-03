/**
 * @file system/concurrency/unhandled-event-disposal.cpp
 * @brief An event whose type NO actor subscribed to must still have its payload disposed.
 *
 * The type-erased disposer registry that frees an event the framework must DROP was populated
 * only as a side effect of `router::memh::subscribe<E>()` — i.e. only when some actor called
 * `registerEvent<E>()`. A type that is pushed but never subscribed anywhere therefore had NO
 * disposer at all, and `memh::route(event, onError)` did not dispose on its unhandled branch
 * either (its sibling `sesh::route` and every `_CleanEvent` path always did). Result: every such
 * event leaked its `std::string` / `std::vector` members, permanently and unboundedly.
 *
 * The trigger is ordinary, not adversarial: pushing to an actor that does not handle the type —
 * a routine refactor mistake (a `registerEvent` removed, or the recipient turns out to be on a
 * different core). Both halves are fixed: `router::ensure_disposer<Event, E>()` at every enqueue
 * funnel (`VirtualCore::push`/`send`, `Pipe::push`/`allocated_push`) guarantees a disposer exists,
 * and the router's unhandled branch now disposes.
 *
 * Single-core on purpose: the local pipe path only, so no mailbox residue or teardown sweep can
 * mask the result. The control event (handled by the receiver) pins that normal delivery still
 * disposes exactly once.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * @ingroup Tests
 */
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/main.h>

namespace {
std::atomic<std::int64_t> g_live{0};
struct Tracked {
    std::string blob;
    Tracked() : blob(64, 'z') { g_live.fetch_add(1); }
    Tracked(Tracked const &o) : blob(o.blob) { g_live.fetch_add(1); }
    ~Tracked() { g_live.fetch_sub(1); }
};
struct UnhandledEvent : qb::Event { Tracked p; };   // NO actor ever registers this
struct HandledEvent : qb::Event { Tracked p; };     // registered by the receiver

std::atomic<std::uint32_t> g_dst{0};
std::atomic<int>           g_handled{0};

class Receiver : public qb::Actor {
public:
    qb::io::async::task<bool> onInit() final {
        registerEvent<HandledEvent>(*this);
        registerEvent<qb::KillEvent>(*this);
        g_dst.store(static_cast<std::uint32_t>(id()));
        co_return true;
    }
    void on(HandledEvent const &) { g_handled.fetch_add(1); }
    void on(qb::KillEvent const &) { kill(); }
};

class Sender : public qb::Actor, public qb::ICallback {
    int _n = 0;
public:
    qb::io::async::task<bool> onInit() final {
        registerEvent<qb::KillEvent>(*this);
        registerCallback(*this);
        co_return true;
    }
    void on(qb::KillEvent const &) { unregisterCallback(); kill(); }
    void on(qb::LoopEvent const &) final {
        const auto d = g_dst.load();
        if (!d || _n >= 1000) return;
        qb::ActorId dest{d};
        for (int i = 0; i < 100 && _n < 1000; ++i, ++_n) {
            push<UnhandledEvent>(dest);
            push<HandledEvent>(dest);
        }
    }
};
} // namespace

TEST(UnhandledEventDisposal, EventWithNoSubscriberStillDisposesItsPayload) {
    g_live.store(0);
    {
        qb::Main main;
        main.addActor<Receiver>(0);
        main.addActor<Sender>(0);   // same core: pure local pipe, no mailbox involved
        main.start();
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        qb::Main::stop();
        main.join();
    }
    EXPECT_EQ(g_handled.load(), 1000) << "the control (subscribed) event must be delivered normally";
    EXPECT_EQ(g_live.load(), 0) << "an event NOBODY subscribed to must still have its payload disposed "
                                   "(pre-fix this leaked one payload per push, forever)";
}
