/**
 * @file system/concurrency/shutdown-multibucket-residue.cpp
 * @brief The teardown mailbox sweep must handle a MULTI-BUCKET event that straddles the ring wrap.
 *
 * `qb::Event`s occupy a whole number of ring slots (`EventBucket`s) and the producer's
 * `enqueue<_All=true>` is atomic but NOT wrap-aligned — it splits an item across the end of the
 * storage with a two-section memcpy. `SharedCoreCommunication::dispose_residual_mailbox_events()`
 * (the post-join sweep that frees events a stopped core never drained) parses that storage by the
 * embedded `bucket_size`, so it needs its batch delivered CONTIGUOUSLY.
 *
 * It used `consume_all`, which walks the ring IN PLACE and therefore invokes its functor TWICE
 * when the readable range wraps (pinned by unit/lockfree/ring-wrap-batching.cpp). A multi-bucket
 * event straddling the wrap was then torn in half: the first call read a header whose
 * `bucket_size` ran past the segment and disposed an event whose payload bytes were not there —
 * running `~std::string` / `~std::vector` on out-of-range memory — and the second call
 * reinterpreted that event's TAIL buckets as a fresh event header and disposed a bogus type.
 *
 * The existing shutdown-saturation test cannot reach this: its events fit in ONE bucket, so they
 * can never straddle. This one uses a deliberately LARGE event (several buckets, and an odd
 * bucket count so the write index cannot stay wrap-aligned) and leaves residue in a saturated
 * mailbox at shutdown. Run under ASan the pre-fix code corrupts the heap here; the live-payload
 * counter also catches a double- or missed-dispose in a plain build.
 *
 * That counter is 0 both when the sweep is correct and when nothing was ever pushed, so the case
 * also carries the two runtime witnesses its sibling shutdown-saturation.cpp has: the sink must
 * have received events (traffic really flowed, so the ring really wrapped) and the flood must have
 * outrun it (residue really existed for the post-join sweep). Both are asserted per iteration.
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
#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/main.h>

namespace {

std::atomic<std::int64_t> g_live_payloads{0};

// Heap-owning payload so a wrong dispose runs a real destructor on wrong bytes.
struct Tracked {
    std::string blob;
    Tracked()
        : blob(48, 'x') {
        g_live_payloads.fetch_add(1, std::memory_order_relaxed);
    }
    Tracked(Tracked const &o)
        : blob(o.blob) {
        g_live_payloads.fetch_add(1, std::memory_order_relaxed);
    }
    Tracked &operator=(Tracked const &) = delete;
    ~Tracked() {
        g_live_payloads.fetch_sub(1, std::memory_order_relaxed);
    }
};

// Sized so the event spans SEVERAL buckets, with an ODD bucket count: the ring storage is a
// power-of-two-ish slot count, so an even/aligned stride could march the write index around
// without ever straddling. An odd stride guarantees the wrap is hit.
struct FatEvent : public qb::Event {
    Tracked                  payload;
    std::array<char, 2 * QB_LOCKFREE_EVENT_BUCKET_BYTES> filler{}; // -> 3 buckets: an ODD stride, so the write index cannot stay wrap-aligned
    std::uint32_t            seq;
    explicit FatEvent(std::uint32_t s)
        : seq(s) {}
};

std::atomic<std::uint32_t> g_sink_id{0};
std::atomic<bool>          g_sink_ready{false};

// Runtime witnesses that the scenario actually happened, mirroring `g_received` in the sibling
// shutdown-saturation.cpp. Without them the only assertion is a live-payload count, which is
// trivially 0 when NOTHING was ever pushed: a flood that never started (sink id never published, the
// source core never scheduled, the sleep below landing on the wrong side of startup) left the case
// green having exercised no sweep at all.
std::atomic<std::uint64_t> g_pushed{0};
std::atomic<std::uint64_t> g_received{0};

// Drains, but deliberately slowly, so the mailbox stays saturated (and therefore wrapped)
// while the source keeps flooding — mirrors SlowSinkActor in shutdown-saturation.cpp.
class SlowSinkActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() final {
        registerEvent<FatEvent>(*this);
        registerEvent<qb::KillEvent>(*this);
        g_sink_id.store(static_cast<std::uint32_t>(id()), std::memory_order_release);
        g_sink_ready.store(true, std::memory_order_release);
        co_return true;
    }
    void
    on(FatEvent const &e) {
        g_received.fetch_add(e.payload.blob.empty() ? 0u : 1u, std::memory_order_relaxed);
        volatile int sink = 0;
        for (int i = 0; i < 256; ++i)
            sink = sink + i;
        (void) sink;
    }
    void
    on(qb::KillEvent const &) {
        kill();
    }
};

class FloodActor : public qb::Actor, public qb::ICallback {
    std::uint32_t _seq{0};

public:
    qb::io::async::task<bool>
    onInit() final {
        registerEvent<qb::KillEvent>(*this);
        registerCallback(*this);
        co_return true;
    }
    void
    on(qb::KillEvent const &) {
        unregisterCallback();
        kill();
    }
    void
    on(qb::LoopEvent const &) final {
        if (!g_sink_ready.load(std::memory_order_acquire))
            return;
        const qb::ActorId sink{g_sink_id.load(std::memory_order_acquire)};
        for (int i = 0; i < 1500; ++i)
            push<FatEvent>(sink, _seq++);
        g_pushed.fetch_add(1500, std::memory_order_relaxed);
    }
};

} // namespace

TEST(ShutdownMultibucketResidue, TeardownSweepHandlesAWrapStraddlingEvent) {
    // A multi-bucket event is the whole point of this test.
    ASSERT_GT(sizeof(FatEvent), static_cast<std::size_t>(QB_LOCKFREE_EVENT_BUCKET_BYTES))
        << "FatEvent must span more than one ring slot";

    if (std::thread::hardware_concurrency() < 2) {
        GTEST_SKIP() << "needs >= 2 hardware cores (1 sink + 1 source)";
    }

    // Where the residue lands relative to the ring wrap is timing-dependent, so repeat: a
    // regression leaked on ~2 of 3 single runs, which over these iterations is effectively certain.
    constexpr int kIterations = 5;
    for (int it = 0; it < kIterations; ++it) {
        g_live_payloads.store(0, std::memory_order_relaxed);
        g_sink_ready.store(false, std::memory_order_relaxed);
        g_pushed.store(0, std::memory_order_relaxed);
        g_received.store(0, std::memory_order_relaxed);

        {
            qb::Main main;
            main.addActor<SlowSinkActor>(0);
            main.addActor<FloodActor>(1);
            main.start();

            // Let the flood saturate the sink's mailbox and wrap its ring many times.
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            qb::Main::stop();
            main.join();
            EXPECT_FALSE(main.hasError()) << "iteration " << it;
        } // ~Main -> post-join residual sweep

        // The payload count below is 0 both when the sweep is correct and when the flood never
        // happened, so witness the scenario before believing it: the sink must have DRAINED
        // multi-bucket events (the ring wrapped under real traffic) and the flood must have
        // outrun it (residue was left for the post-join sweep to find). Without both, the
        // assertion that follows is vacuous.
        ASSERT_GT(g_received.load(std::memory_order_relaxed), 0u)
            << "iteration " << it << ": the sink received no FatEvent — the flood never reached it, so no ring wrapped";
        ASSERT_GT(g_pushed.load(std::memory_order_relaxed), g_received.load(std::memory_order_relaxed))
            << "iteration " << it << ": the sink drained everything pushed — no residue was left for the teardown sweep";

        ASSERT_EQ(g_live_payloads.load(std::memory_order_relaxed), 0)
            << "iteration " << it << ": every FatEvent payload must be disposed exactly once by the teardown sweep";
    }
}
