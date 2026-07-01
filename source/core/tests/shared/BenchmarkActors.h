/**
 * @file qb/source/core/tests/shared/BenchmarkActors.h
 * @brief Count-and-kill sink + loop-and-kill source skeletons for one-way throughput benches
 *
 * The one-way push throughput benches (bm-allocated-push, bm-producer-burst, bm-fan-in,
 * bm-producer-consumer, bm-broadcast-fanout) all share the same two-actor shutdown protocol:
 *
 *   - a \b sink registers a marker event, counts deliveries, and ends the run with
 *     \c broadcast<qb::KillEvent>() once it has seen exactly \c expect of them;
 *   - a \b source emits a fixed number of marker events in \c onInit() then \c kill()s itself.
 *
 * These templates capture that skeleton (parameterised on the marker \c Event) so each bench
 * only supplies its own empty marker type. Benches with a bespoke send pattern (burst-per-tick
 * callbacks, allocated_push, broadcast-vs-explicit fan-out, atomic global delivery counters)
 * keep their own source/sink and reuse only what fits.
 *
 * The marker \c Event must be default-constructible and \c push<Event>(dst)-able; the sink
 * registers it in \c onInit() so placement matches \c Main::start(true) worker semantics.
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
 * @ingroup Core
 */

#ifndef QB_BENCHMARK_ACTORS_H
#define QB_BENCHMARK_ACTORS_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <qb/actor.h>

namespace qb::bench {

/**
 * Count-and-kill sink: registers \c Event, counts deliveries, and ends the run with
 * \c broadcast<qb::KillEvent>() the instant it has observed exactly \c expect of them.
 *
 * The marker is registered in \c onInit() (not the ctor) so registration runs on the owning
 * VirtualCore worker under \c Main::start(true), matching the throughput-bench placement model.
 *
 * An optional \c tally (a cross-thread atomic) receives the final delivery count in the destructor
 * — which runs on the worker during shutdown, before \c join() returns — so an out-of-loop probe
 * can assert the topology delivered exactly \c expect (a positive check rather than relying on a
 * \c join() hang to surface a dropped/mis-routed message). Pass \c nullptr (the default) to opt out.
 */
template <typename Event>
class CountAndKillSinkActor final : public qb::Actor {
    const std::uint64_t                         _expect;
    std::uint64_t                               _got = 0;
    std::shared_ptr<std::atomic<std::uint64_t>> _tally;

public:
    explicit CountAndKillSinkActor(std::uint64_t const expect, std::shared_ptr<std::atomic<std::uint64_t>> tally = nullptr)
        : _expect(expect)
        , _tally(std::move(tally)) {}

    ~CountAndKillSinkActor() final {
        if (_tally)
            _tally->store(_got, std::memory_order_relaxed);
    }

    qb::io::async::task<bool>
    onInit() final {
        registerEvent<Event>(*this);
        co_return true;
    }

    void
    on(Event const &) {
        if (++_got == _expect)
            broadcast<qb::KillEvent>();
    }
};

/**
 * Loop-and-kill source: emits exactly \c count one-way \c push<Event> messages to \c dst in
 * \c onInit(), then \c kill()s itself. The matching \c CountAndKillSinkActor terminates the
 * engine once it has counted them all.
 */
template <typename Event>
class LoopAndKillSourceActor final : public qb::Actor {
    const qb::ActorId   _dst;
    const std::uint64_t _count;

public:
    LoopAndKillSourceActor(qb::ActorId const dst, std::uint64_t const count)
        : _dst(dst)
        , _count(count) {}

    qb::io::async::task<bool>
    onInit() final {
        for (std::uint64_t i = 0; i < _count; ++i)
            push<Event>(_dst);
        kill();
        co_return true;
    }
};

} // namespace qb::bench

#endif // QB_BENCHMARK_ACTORS_H
