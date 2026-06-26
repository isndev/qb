/**
 * @file qb/source/core/tests/shared/LatencyFlush.h
 * @brief Flush a `pg::latency` histogram into the shared bench sink (+ optional dump)
 *
 * The three latency benches (bm-ping-pong-latency, bm-multicast-latency, bm-pipeline-latency)
 * end every run with the same two-step hand-off out of their per-actor / per-thread
 * \c pg::latency accumulator:
 *
 *   1. if it took any samples, publish its mean + sample count into the cross-thread sink
 *      (\c qb::bench::record_last_latency) so the benchmark thread can read them after
 *      \c join() — the accumulator lives on a VirtualCore worker, the counters are read on
 *      the benchmark thread, so the value must cross threads via the shared sink;
 *   2. when \c QB_ACTOR_BENCH_HISTOGRAM is set in the environment (or the caller forces it),
 *      dump the percentile histogram to \c std::cout.
 *
 * \c flush_latency_to_sink() captures exactly that idiom; call it once from the place that
 * owns the accumulator (a \c PingActor / \c ProducerActor destructor, or a reference-impl
 * thread tail). Templated on the accumulator type so it works for any \c pg::latency<...>.
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

#ifndef QB_LATENCY_FLUSH_H
#define QB_LATENCY_FLUSH_H

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>

#include "BenchmarkIterationSink.h"
#include "TestLatency.h"

namespace qb::bench {

/**
 * Publish \p latency's mean + sample count into the cross-thread last-run sink, then (when
 * \p force_histogram is true or \c QB_ACTOR_BENCH_HISTOGRAM is set) dump its percentile
 * histogram to \c std::cout. No-op for either step when the accumulator took zero samples.
 *
 * \tparam Latency  any \c pg::latency<...> instantiation (exposes \c sample_count(),
 *                  \c mean_nanoseconds(), and \c generate<O, Ratio>(out, unit)).
 */
template <typename Latency>
inline void
flush_latency_to_sink(Latency &latency, bool const force_histogram = false) {
    if (latency.sample_count()) {
        record_last_latency(latency.mean_nanoseconds(), static_cast<std::uint64_t>(latency.sample_count()));
    }
    if ((force_histogram || std::getenv("QB_ACTOR_BENCH_HISTOGRAM")) && latency.sample_count()) {
        latency.template generate<std::ostream, std::chrono::nanoseconds>(std::cout, "ns");
    }
}

} // namespace qb::bench

#endif // QB_LATENCY_FLUSH_H
