/**
 * @file test-actor-routing-pubsub.cpp
 * @brief Tests for the distribution patterns: `qb::WorkerPool` (round-robin / sticky-by-key /
 *        broadcast worker pool) and `qb::PubSub<Topic>` (per-core publish/subscribe bus).
 *
 * Coverage:
 *   - Router: round-robin cycling, sticky `for_key`, add/remove (pure unit tests), and a
 *     round-robin dispatch integration test (even distribution across a worker pool);
 *   - PubSub: fan-out reaches every subscriber, unsubscribe stops delivery, subscriber_count
 *     tracks subscribe/unsubscribe (idempotent).
 *
 * Run under ASAN_OPTIONS=detect_leaks=0 like the rest of the actor suites.
 */

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/core/patterns.h>
#include <qb/main.h>
#include <qb/io/async.h>
#include <atomic>
#include <chrono>
#include <vector>

using namespace qb;
using namespace std::chrono_literals;

// ===========================================================================
// Router — pure unit tests (no engine; placeholder ActorIds).
// ===========================================================================
TEST(WorkerPoolUnit, RoundRobinCycles) {
    qb::ActorId a(1), b(2), c(3);
    qb::WorkerPool  r{{a, b, c}};
    EXPECT_EQ(r.size(), 3u);
    EXPECT_FALSE(r.empty());
    EXPECT_EQ(r.next(), a);
    EXPECT_EQ(r.next(), b);
    EXPECT_EQ(r.next(), c);
    EXPECT_EQ(r.next(), a); // cycles back
    EXPECT_EQ(r.next(), b);
}

TEST(WorkerPoolUnit, ForKeyIsStickyAndDistributes) {
    qb::ActorId w0(10), w1(20), w2(30);
    qb::WorkerPool  r{{w0, w1, w2}};
    EXPECT_EQ(r.for_key(0), w0);
    EXPECT_EQ(r.for_key(1), w1);
    EXPECT_EQ(r.for_key(2), w2);
    EXPECT_EQ(r.for_key(3), w0);              // 3 % 3 == 0
    EXPECT_EQ(r.for_key(99), r.for_key(99));  // deterministic for the same key
}

TEST(WorkerPoolUnit, AddRemoveResizesPool) {
    qb::WorkerPool r;
    EXPECT_TRUE(r.empty());
    qb::ActorId a(1), b(2);
    r.add(a);
    r.add(b);
    EXPECT_EQ(r.size(), 2u);
    r.remove(a);
    EXPECT_EQ(r.size(), 1u);
    EXPECT_EQ(r.next(), b);
    EXPECT_EQ(r.next(), b); // single worker -> always itself
}

// ===========================================================================
// Router — round-robin dispatch integration (even distribution).
// ===========================================================================
struct Job : public qb::Event {
    int n{0};
    explicit Job(int v)
        : n(v) {}
};

namespace {
std::atomic<int> g_worker_recv[3];
std::atomic<int> g_jobs_done{0};
constexpr int    kJobs = 9;
} // namespace

class Worker : public qb::Actor {
    int _idx;

public:
    explicit Worker(int idx)
        : _idx(idx) {}
    bool
    onInit() override {
        registerEvent<Job>(*this);
        return true;
    }
    void
    on(Job &) {
        g_worker_recv[_idx].fetch_add(1);
        if (g_jobs_done.fetch_add(1) + 1 == kJobs)
            qb::Main::stop();
    }
};

class Dispatcher : public qb::Actor {
    std::vector<qb::ActorId> _workers;

public:
    explicit Dispatcher(std::vector<qb::ActorId> w)
        : _workers(std::move(w)) {}
    bool
    onInit() override {
        qb::WorkerPool r{_workers};
        for (int i = 0; i < kJobs; ++i)
            push<Job>(r.next(), i);
        return true;
    }
};

TEST(ActorWorkerPool, RoundRobinDistributesEvenly) {
    g_worker_recv[0] = g_worker_recv[1] = g_worker_recv[2] = 0;
    g_jobs_done                                            = 0;
    qb::Main                 main;
    std::vector<qb::ActorId> workers;
    for (int i = 0; i < 3; ++i)
        workers.push_back(main.addActor<Worker>(0, i));
    main.addActor<Dispatcher>(0, workers);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_worker_recv[0].load(), 3); // 9 jobs / 3 workers, round-robin
    EXPECT_EQ(g_worker_recv[1].load(), 3);
    EXPECT_EQ(g_worker_recv[2].load(), 3);
}

// ===========================================================================
// PubSub<Topic> — per-core publish/subscribe bus.
// ===========================================================================
struct Tick : public qb::Event {
    int value{0};
    explicit Tick(int v)
        : value(v) {}
};

namespace {
std::atomic<int>  g_sub_recv[3];
std::atomic<bool> g_unsub_received{false};
std::atomic<int>  g_count_after_sub{-1};
std::atomic<int>  g_count_after_unsub{-1};
} // namespace

class Sub : public qb::Actor {
    int _idx;

public:
    explicit Sub(int idx)
        : _idx(idx) {}
    bool
    onInit() override {
        registerEvent<Tick>(*this);
        getService<qb::PubSub<Tick>>()->subscribe(id());
        return true;
    }
    void
    on(Tick &) {
        g_sub_recv[_idx].fetch_add(1);
    }
};

// Subscribes then immediately leaves — must never receive a publication.
class UnsubSub : public qb::Actor {
public:
    bool
    onInit() override {
        registerEvent<Tick>(*this);
        auto *bus = getService<qb::PubSub<Tick>>();
        bus->subscribe(id());
        bus->unsubscribe(id());
        return true;
    }
    void
    on(Tick &) {
        g_unsub_received = true;
    }
};

// Publishes two ticks shortly after start (once every subscriber has registered).
class Publisher : public qb::Actor {
public:
    bool
    onInit() override {
        qb::io::async::callback(
            [this] {
                auto *bus = getService<qb::PubSub<Tick>>();
                bus->publish(1); // Tick{1}
                bus->publish(2); // Tick{2}
                qb::io::async::callback([] { qb::Main::stop(); }, 50ms);
            },
            20ms);
        return true;
    }
};

TEST(ActorPubSub, FanOutReachesAllSubscribers) {
    g_sub_recv[0] = g_sub_recv[1] = g_sub_recv[2] = 0;
    qb::Main main;
    main.addActor<qb::PubSub<Tick>>(0); // the bus first
    for (int i = 0; i < 3; ++i)
        main.addActor<Sub>(0, i);
    main.addActor<Publisher>(0);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_sub_recv[0].load(), 2); // each subscriber received both ticks
    EXPECT_EQ(g_sub_recv[1].load(), 2);
    EXPECT_EQ(g_sub_recv[2].load(), 2);
}

TEST(ActorPubSub, UnsubscribeStopsDelivery) {
    g_sub_recv[0]    = 0;
    g_unsub_received = false;
    qb::Main main;
    main.addActor<qb::PubSub<Tick>>(0);
    main.addActor<Sub>(0, 0);     // stays subscribed
    main.addActor<UnsubSub>(0);   // leaves immediately
    main.addActor<Publisher>(0);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_sub_recv[0].load(), 2);       // the staying subscriber still gets both
    EXPECT_FALSE(g_unsub_received.load());    // the one that left gets nothing
}

// Subscribes twice (idempotent) then unsubscribes, reporting the bus count each time.
class CountProbe : public qb::Actor {
public:
    bool
    onInit() override {
        auto *bus = getService<qb::PubSub<Tick>>();
        bus->subscribe(id());
        bus->subscribe(id()); // idempotent — still one subscriber
        g_count_after_sub = static_cast<int>(bus->subscriber_count());
        bus->unsubscribe(id());
        g_count_after_unsub = static_cast<int>(bus->subscriber_count());
        qb::io::async::callback([] { qb::Main::stop(); }, 10ms);
        return true;
    }
};

TEST(ActorPubSub, SubscriberCountTracksSubscribeUnsubscribe) {
    g_count_after_sub = g_count_after_unsub = -1;
    qb::Main main;
    main.addActor<qb::PubSub<Tick>>(0);
    main.addActor<CountProbe>(0);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_count_after_sub.load(), 1);   // idempotent subscribe
    EXPECT_EQ(g_count_after_unsub.load(), 0); // removed
}
