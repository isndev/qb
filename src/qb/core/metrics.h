/**
 * @file qb/core/metrics.h
 * @brief The metrics model of qb and its modules -- families, samples, histograms, a snapshot -- and its
 *        Prometheus text exposition (format 0.0.4). Plain values: no actor, no engine, no clock.
 *
 * A snapshot is what a source knows at the moment it is asked: a list of metric families, each a name, a help
 * text, a type (counter, gauge, histogram) and its samples, one per label set. Sources fill one -- the qb-core
 * counters of a `qb::CoreStats` (`describe()` below), the HTTP server's request metrics, a Redis or PostgreSQL
 * client's `stats()` -- and `write_text()` renders it in the Prometheus text exposition format, version 0.0.4.
 * Snapshots of several sources and cores merge (`snapshot::merge`, then `snapshot::canonicalize`): families meet
 * by name, and the samples of one label set are summed. The per-core service that collects a core's sources and
 * the scrape that gathers every core are in `<qb/core/metrics_registry.h>` (Huly QB-154).
 *
 * Names follow `qb_<layer>_<name>_<unit>`, in base units (seconds, bytes), counters ending in `_total`; a
 * family produced on several cores either carries a `core` label or is meant to be summed across them.
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
 * @ingroup Engine
 */

#ifndef QB_CORE_METRICS_H
#define QB_CORE_METRICS_H

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "CoreStats.h"

namespace qb::metrics {

/// The type of a metric family, as its `# TYPE` line names it.
enum class metric_type : std::uint8_t {
    counter,   ///< a count that only grows; its name ends in `_total`
    gauge,     ///< a value that goes up and down
    histogram, ///< observations counted into buckets, with their count and (when known) their sum
};

/// `"counter"`, `"gauge"` or `"histogram"`.
[[nodiscard]] constexpr std::string_view
type_name(metric_type const type) noexcept {
    switch (type) {
        case metric_type::counter:
            return "counter";
        case metric_type::gauge:
            return "gauge";
        case metric_type::histogram:
            return "histogram";
    }
    return "untyped";
}

/// The `Content-Type` of the text `write_text()` produces.
inline constexpr std::string_view text_content_type = "text/plain; version=0.0.4; charset=utf-8";

/// The bucket bounds, in seconds, of a latency histogram nobody configured: Prometheus' defaults, 5 ms to 10 s.
inline constexpr std::array<double, 11> default_duration_buckets{0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0, 10.0};

/// Whether `name` is a valid metric name: `[a-zA-Z_:][a-zA-Z0-9_:]*`.
[[nodiscard]] bool is_valid_metric_name(std::string_view name) noexcept;

/// Whether `name` is a valid label name: `[a-zA-Z_][a-zA-Z0-9_]*`, not starting with `__` (reserved).
[[nodiscard]] bool is_valid_label_name(std::string_view name) noexcept;

/**
 * @brief A sample's value: an exact unsigned integer, or a double.
 * @details A counter of events is an integer and is printed exactly, past the 2^53 a double holds. A duration
 *          in seconds, a ratio, a negative gauge are doubles. Adding two integers keeps an integer; anything else
 *          adds as doubles.
 */
class value {
    std::uint64_t _integer = 0;
    double        _real    = 0.0;
    bool          _exact   = true;

public:
    /// Zero, exact.
    constexpr value() noexcept = default;
    /// An integer: exact when it is not negative, a double otherwise.
    template <std::integral I>
    constexpr value(I const v) noexcept // NOLINT(google-explicit-constructor): a number is a value
        : _integer(v < 0 ? 0 : static_cast<std::uint64_t>(v))
        , _real(static_cast<double>(v))
        , _exact(!(v < 0)) {}
    /// A double.
    constexpr value(double const v) noexcept // NOLINT(google-explicit-constructor)
        : _real(v)
        , _exact(false) {}

    /// Whether the value is an exact unsigned integer.
    [[nodiscard]] constexpr bool
    is_integer() const noexcept {
        return _exact;
    }
    /// The integer (meaningful when `is_integer()`).
    [[nodiscard]] constexpr std::uint64_t
    integer() const noexcept {
        return _integer;
    }
    /// The value as a double, whichever it is.
    [[nodiscard]] constexpr double
    as_double() const noexcept {
        return _exact ? static_cast<double>(_integer) : _real;
    }
    /// Adds `other`: integer + integer stays exact, anything else is a double.
    constexpr value &
    operator+=(value const &other) noexcept {
        if (_exact && other._exact) {
            _integer += other._integer;
            _real = static_cast<double>(_integer);
        } else {
            _real  = as_double() + other.as_double();
            _exact = false;
        }
        return *this;
    }
};

/// One label: its name and its value. A value is any UTF-8 text; the exposition escapes it.
struct label {
    std::string name;
    std::string value;
};

/// The labels of one sample, in the order they are printed. Two label sets are the same series when they hold
/// the same pairs, in any order.
using label_set = std::vector<label>;

/// One sample of a counter or a gauge.
struct sample {
    label_set      labels;
    metrics::value number;
};

/**
 * @brief One histogram: its labels, its bucket bounds, the observations of each bucket, and their sum.
 * @details `buckets` holds `bounds.size() + 1` counts, NOT cumulative: `buckets[i]` counts the observations in
 *          `(bounds[i-1], bounds[i]]`, and the last one those above the last bound. The exposition prints them
 *          cumulated, ending with `le="+Inf"`, which is `count()`. `sum` is absent when the source does not keep
 *          one; the exposition then prints no `_sum` line.
 */
struct histogram_sample {
    label_set                  labels;
    std::vector<double>        bounds;  ///< finite upper bounds, strictly ascending
    std::vector<std::uint64_t> buckets; ///< `bounds.size() + 1` per-bucket counts
    std::optional<double>      sum;     ///< the sum of the observations, when the source keeps it

    /// Every observation: the sum of `buckets`.
    [[nodiscard]] std::uint64_t count() const noexcept;
};

/**
 * @brief A metric family: one name, one help text, one type, and its samples.
 * @details The `add` functions validate what they are given and throw `std::invalid_argument` on a label name
 *          that is not valid or repeated in one set, a `le` label on a histogram, bounds that are not finite and
 *          strictly ascending, or buckets that do not count `bounds.size() + 1`; and `std::logic_error` when the
 *          sample does not match the family's type.
 */
class family {
public:
    std::string                   name;
    std::string                   help;
    metric_type                   type = metric_type::gauge;
    std::vector<sample>           samples;    ///< counter and gauge samples
    std::vector<histogram_sample> histograms; ///< histogram samples

    /// Adds a counter or gauge sample. @return `*this`, to chain.
    family &add(label_set labels, metrics::value number);
    /// Adds a histogram sample. @return `*this`, to chain.
    family &add(histogram_sample histogram);
    /**
     * @brief Adds a histogram sample from per-bucket counts. @return `*this`, to chain.
     * @param labels The sample's labels.
     * @param bounds The finite upper bounds, strictly ascending.
     * @param buckets `bounds.size() + 1` per-bucket (not cumulative) counts.
     * @param sum The sum of the observations, if known.
     */
    family &add(label_set labels, std::vector<double> bounds, std::vector<std::uint64_t> buckets, std::optional<double> sum);
    /// Whether the family holds no sample.
    [[nodiscard]] bool empty() const noexcept;
};

/**
 * @brief What one or several sources reported: an ordered list of metric families.
 * @details `counter`, `gauge` and `histogram` return the family of that name, appending it the first time (with
 *          its help text): a source asks for its family and adds its samples. They throw `std::invalid_argument` on
 *          a name that is not a valid metric name, or that names a family of another type in this snapshot.
 *          The reference they return is valid until the next family is added: a family is an element of a vector,
 *          so fill one family before asking for the next.
 *          A snapshot is a value: copyable, movable, and carried between cores by an event.
 */
class snapshot {
    std::vector<family> _families;
    std::uint64_t       _conflicts = 0;

    family &get(std::string name, std::string help, metric_type type);

public:
    /// The counter family `name`, appended when absent.
    family &counter(std::string name, std::string help);
    /// The gauge family `name`, appended when absent.
    family &gauge(std::string name, std::string help);
    /// The histogram family `name`, appended when absent.
    family &histogram(std::string name, std::string help);

    /// The family named `name`, or null.
    [[nodiscard]] family const *find(std::string_view name) const noexcept;
    /// Every family, in the order they were added.
    [[nodiscard]] std::vector<family> const &families() const noexcept;
    /// Whether the snapshot holds no family.
    [[nodiscard]] bool empty() const noexcept;

    /**
     * @brief Appends the families of `other`. A family already here receives `other`'s samples; a family of the
     *        same name and another type is dropped and counted in `conflicts()`. Call `canonicalize()` after the
     *        last merge.
     */
    void merge(snapshot other);
    /**
     * @brief Sums the samples of each family that have the same label set (in any order), keeping the first's
     *        place and label order; two histograms of one label set and different bounds keep the first and count
     *        a conflict. A histogram sum is kept only when both had one.
     */
    void canonicalize();
    /// The families and histograms `merge` and `canonicalize` dropped, plus those of the merged snapshots.
    [[nodiscard]] std::uint64_t conflicts() const noexcept;
};

/**
 * @brief Appends `snap` to `out` in the Prometheus text exposition format, version 0.0.4.
 * @details Each non-empty family is one `# HELP` line (`\` and line feeds escaped), one `# TYPE` line, then its
 *          samples, one per line, `name{label="value",...} value`, label values escaped (`\`, `"`, line feed).
 *          An integer prints exactly; a double in its shortest round-trip `%g` form, `+Inf`, `-Inf` or `NaN`. A
 *          histogram prints its cumulated `name_bucket{...,le="bound"}` lines, `le="+Inf"`, then `name_sum` (when
 *          known) and `name_count`. No timestamps; every line ends with a line feed. Call it on a canonical
 *          snapshot: two samples of one label set are printed as they are.
 */
void write_text(snapshot const &snap, std::string &out);

/// `write_text()` into a new string.
[[nodiscard]] std::string write_text(snapshot const &snap);

/**
 * @brief Adds the counters of one core to `snap`, every sample labelled `core="<core>"`.
 * @details Families: `qb_core_loop_passes_total`, `qb_core_events_received_total`,
 *          `qb_core_event_buckets_received_total`, `qb_core_events_sent_total`, `qb_core_event_buckets_sent_total`,
 *          `qb_core_sends_blocked_total`, `qb_core_events_dropped_total`, `qb_core_dead_letters_total` (also labelled
 *          `reason`, one sample per `qb::DeadLetterReason`) and `qb_core_io_events_total`; with pass timing on
 *          (`qb::CoreInitializer::setPassTiming`, `stats.pass_time.count > 0`) also `qb_core_passes_timed_total`,
 *          `qb_core_pass_time_seconds_total` and the gauges `qb_core_pass_last_duration_seconds`,
 *          `qb_core_pass_max_duration_seconds` and `qb_core_pass_recent_max_duration_seconds`.
 * @param snap The snapshot to fill.
 * @param stats The core's counters (`qb::Actor::getCoreStats()` on that core).
 * @param core The core's index.
 */
void describe(snapshot &snap, qb::CoreStats const &stats, qb::CoreId core);

/**
 * @brief Describes an autonomous listener's dispatch timing and its last run's event count.
 * @details Pass `listener.dispatch_timing()` and `listener.nb_invoked_event()` on the listener's thread.
 *          `qb_io_last_run_events` is a gauge, not a cumulative counter. With recorded timing, adds
 *          `qb_io_dispatches_timed_total`, `qb_io_dispatch_time_seconds_total` and last/max/recent-max
 *          duration gauges. Dispatch timing covers libev watcher callbacks, not deferred callbacks or
 *          coroutine resumes outside that dispatch. This adapter neither enables timing nor reads a clock.
 *          Give distinct listeners distinct labels; do not sum last/max gauges from unrelated listeners.
 */
void describe_dispatch(snapshot &snap, qb::TimingStats const &stats, std::uint64_t last_run_events, label_set labels);

} // namespace qb::metrics

#endif // QB_CORE_METRICS_H
