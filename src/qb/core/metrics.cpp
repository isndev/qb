/**
 * @file qb/core/metrics.cpp
 * @brief The metrics model and its Prometheus text exposition (`qb/core/metrics.h`, Huly QB-154).
 *
 * Compiled as part of qb-core's single translation unit (`core.cpp`).
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
 */

#include "metrics.h"
#include "metrics_registry.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace qb::metrics {

namespace {

[[nodiscard]] bool
is_name_start(char const c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

[[nodiscard]] bool
is_name_char(char const c) noexcept {
    return is_name_start(c) || (c >= '0' && c <= '9');
}

void
validate_labels(label_set const &labels, bool const histogram) {
    for (std::size_t i = 0; i < labels.size(); ++i) {
        if (!is_valid_label_name(labels[i].name))
            throw std::invalid_argument("qb::metrics: invalid label name '" + labels[i].name + "'");
        if (histogram && labels[i].name == "le")
            throw std::invalid_argument("qb::metrics: a histogram sample cannot carry the label 'le'");
        for (std::size_t j = 0; j < i; ++j)
            if (labels[j].name == labels[i].name)
                throw std::invalid_argument("qb::metrics: the label '" + labels[i].name + "' is given twice");
    }
}

/// The identity of a label set: its pairs sorted by name, joined -- the same series in any label order.
[[nodiscard]] std::string
series_key(label_set const &labels) {
    std::vector<label const *> sorted;
    sorted.reserve(labels.size());
    for (auto const &l : labels)
        sorted.push_back(&l);
    std::sort(sorted.begin(), sorted.end(), [](label const *a, label const *b) { return a->name < b->name; });
    std::string key;
    for (auto const *l : sorted) {
        key += l->name;
        key += '\0';
        key += l->value;
        key += '\0';
    }
    return key;
}

void
append_escaped(std::string &out, std::string_view const text, bool const quote) {
    for (char const c : text) {
        if (c == '\\')
            out += "\\\\";
        else if (c == '\n')
            out += "\\n";
        else if (quote && c == '"')
            out += "\\\"";
        else
            out += c;
    }
}

void
append_double(std::string &out, double const v) {
    if (std::isnan(v)) {
        out += "NaN";
        return;
    }
    if (std::isinf(v)) {
        out += v > 0 ? "+Inf" : "-Inf";
        return;
    }
    char buf[64];
    // The shortest form that reads back as v, in %g style -- what the Prometheus clients print (Go: FormatFloat g -1).
    auto const res = std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::general);
    out.append(buf, res.ptr);
}

void
append_integer(std::string &out, std::uint64_t const v) {
    char       buf[24];
    auto const res = std::to_chars(buf, buf + sizeof(buf), v);
    out.append(buf, res.ptr);
}

void
append_value(std::string &out, value const &v) {
    if (v.is_integer())
        append_integer(out, v.integer());
    else
        append_double(out, v.as_double());
}

/// `{a="x",b="y"}`, with `le` appended when given; nothing at all for an empty set.
void
append_labels(std::string &out, label_set const &labels, std::string_view const le = {}) {
    if (labels.empty() && le.empty())
        return;
    out += '{';
    bool first = true;
    for (auto const &l : labels) {
        if (!first)
            out += ',';
        first = false;
        out += l.name;
        out += "=\"";
        append_escaped(out, l.value, true);
        out += '"';
    }
    if (!le.empty()) {
        if (!first)
            out += ',';
        out += "le=\"";
        out += le;
        out += '"';
    }
    out += '}';
}

} // namespace

bool
is_valid_metric_name(std::string_view const name) noexcept {
    if (name.empty() || !(is_name_start(name.front()) || name.front() == ':'))
        return false;
    return std::all_of(name.begin(), name.end(), [](char const c) { return is_name_char(c) || c == ':'; });
}

bool
is_valid_label_name(std::string_view const name) noexcept {
    if (name.empty() || !is_name_start(name.front()) || name.starts_with("__"))
        return false;
    return std::all_of(name.begin(), name.end(), is_name_char);
}

std::uint64_t
histogram_sample::count() const noexcept {
    std::uint64_t n = 0;
    for (auto const b : buckets)
        n += b;
    return n;
}

family &
family::add(label_set labels, metrics::value const number) {
    if (type == metric_type::histogram)
        throw std::logic_error("qb::metrics: '" + name + "' is a histogram: add a histogram sample");
    validate_labels(labels, false);
    samples.push_back(sample{std::move(labels), number});
    return *this;
}

family &
family::add(histogram_sample histogram) {
    if (type != metric_type::histogram)
        throw std::logic_error("qb::metrics: '" + name + "' is not a histogram");
    validate_labels(histogram.labels, true);
    if (histogram.buckets.size() != histogram.bounds.size() + 1)
        throw std::invalid_argument("qb::metrics: a histogram needs one bucket per bound, plus the one above them");
    for (std::size_t i = 0; i < histogram.bounds.size(); ++i) {
        if (!std::isfinite(histogram.bounds[i]) || (i > 0 && !(histogram.bounds[i - 1] < histogram.bounds[i])))
            throw std::invalid_argument("qb::metrics: histogram bounds must be finite and strictly ascending");
    }
    histograms.push_back(std::move(histogram));
    return *this;
}

family &
family::add(label_set labels, std::vector<double> bounds, std::vector<std::uint64_t> buckets, std::optional<double> const sum) {
    return add(histogram_sample{std::move(labels), std::move(bounds), std::move(buckets), sum});
}

bool
family::empty() const noexcept {
    return samples.empty() && histograms.empty();
}

family &
snapshot::get(std::string name, std::string help, metric_type const type) {
    for (auto &f : _families) {
        if (f.name == name) {
            if (f.type != type)
                throw std::invalid_argument("qb::metrics: '" + name + "' is already a " + std::string(type_name(f.type)));
            return f;
        }
    }
    if (!is_valid_metric_name(name))
        throw std::invalid_argument("qb::metrics: invalid metric name '" + name + "'");
    family f;
    f.name = std::move(name);
    f.help = std::move(help);
    f.type = type;
    _families.push_back(std::move(f));
    return _families.back();
}

family &
snapshot::counter(std::string name, std::string help) {
    return get(std::move(name), std::move(help), metric_type::counter);
}

family &
snapshot::gauge(std::string name, std::string help) {
    return get(std::move(name), std::move(help), metric_type::gauge);
}

family &
snapshot::histogram(std::string name, std::string help) {
    return get(std::move(name), std::move(help), metric_type::histogram);
}

family const *
snapshot::find(std::string_view const name) const noexcept {
    for (auto const &f : _families)
        if (f.name == name)
            return &f;
    return nullptr;
}

std::vector<family> const &
snapshot::families() const noexcept {
    return _families;
}

bool
snapshot::empty() const noexcept {
    return _families.empty();
}

std::uint64_t
snapshot::conflicts() const noexcept {
    return _conflicts;
}

void
snapshot::merge(snapshot other) {
    _conflicts += other._conflicts;
    for (auto &incoming : other._families) {
        family *here = nullptr;
        for (auto &f : _families) {
            if (f.name == incoming.name) {
                here = &f;
                break;
            }
        }
        if (!here) {
            _families.push_back(std::move(incoming));
            continue;
        }
        if (here->type != incoming.type) {
            ++_conflicts;
            continue;
        }
        for (auto &s : incoming.samples)
            here->samples.push_back(std::move(s));
        for (auto &h : incoming.histograms)
            here->histograms.push_back(std::move(h));
    }
}

void
snapshot::canonicalize() {
    for (auto &f : _families) {
        if (f.samples.size() > 1) {
            std::unordered_map<std::string, std::size_t> seen;
            std::vector<sample>                          kept;
            kept.reserve(f.samples.size());
            for (auto &s : f.samples) {
                auto [it, fresh] = seen.try_emplace(series_key(s.labels), kept.size());
                if (fresh)
                    kept.push_back(std::move(s));
                else
                    kept[it->second].number += s.number;
            }
            f.samples = std::move(kept);
        }
        if (f.histograms.size() > 1) {
            std::unordered_map<std::string, std::size_t> seen;
            std::vector<histogram_sample>                kept;
            kept.reserve(f.histograms.size());
            for (auto &h : f.histograms) {
                auto [it, fresh] = seen.try_emplace(series_key(h.labels), kept.size());
                if (fresh) {
                    kept.push_back(std::move(h));
                    continue;
                }
                auto &into = kept[it->second];
                if (into.bounds != h.bounds) {
                    ++_conflicts;
                    continue;
                }
                for (std::size_t i = 0; i < into.buckets.size(); ++i)
                    into.buckets[i] += h.buckets[i];
                if (into.sum && h.sum)
                    *into.sum += *h.sum;
                else
                    into.sum.reset();
            }
            f.histograms = std::move(kept);
        }
    }
}

void
write_text(snapshot const &snap, std::string &out) {
    for (auto const &f : snap.families()) {
        if (f.empty())
            continue;
        out += "# HELP ";
        out += f.name;
        out += ' ';
        append_escaped(out, f.help, false);
        out += "\n# TYPE ";
        out += f.name;
        out += ' ';
        out += type_name(f.type);
        out += '\n';
        if (f.type != metric_type::histogram) {
            for (auto const &s : f.samples) {
                out += f.name;
                append_labels(out, s.labels);
                out += ' ';
                append_value(out, s.number);
                out += '\n';
            }
            continue;
        }
        for (auto const &h : f.histograms) {
            std::uint64_t cumulated = 0;
            std::string   le;
            for (std::size_t i = 0; i <= h.bounds.size(); ++i) {
                cumulated += h.buckets[i];
                le.clear();
                if (i < h.bounds.size())
                    append_double(le, h.bounds[i]);
                else
                    le = "+Inf";
                out += f.name;
                out += "_bucket";
                append_labels(out, h.labels, le);
                out += ' ';
                append_integer(out, cumulated);
                out += '\n';
            }
            if (h.sum) {
                out += f.name;
                out += "_sum";
                append_labels(out, h.labels);
                out += ' ';
                append_double(out, *h.sum);
                out += '\n';
            }
            out += f.name;
            out += "_count";
            append_labels(out, h.labels);
            out += ' ';
            append_integer(out, cumulated);
            out += '\n';
        }
    }
}

std::string
write_text(snapshot const &snap) {
    std::string out;
    write_text(snap, out);
    return out;
}

void
describe(snapshot &snap, qb::CoreStats const &stats, qb::CoreId const core) {
    const std::string core_id = std::to_string(core);
    auto              on_core = [&core_id] {
        return label_set{{"core", core_id}};
    };

    snap.counter("qb_core_loop_passes_total", "Passes of the core's event loop.").add(on_core(), stats.loop_passes);
    snap.counter("qb_core_events_received_total", "Events the core took in, from other cores and its own queue.")
        .add(on_core(), stats.events_received);
    snap.counter("qb_core_event_buckets_received_total", "Size of the events received, in 64-byte buckets.")
        .add(on_core(), stats.buckets_received);
    snap.counter("qb_core_events_sent_total", "Events the core published into another core's mailbox.").add(on_core(), stats.events_sent);
    snap.counter("qb_core_event_buckets_sent_total", "Size of the events sent, in 64-byte buckets.").add(on_core(), stats.buckets_sent);
    snap.counter("qb_core_sends_blocked_total", "Attempts to publish an event that found the destination's mailbox full.")
        .add(on_core(), stats.sends_blocked);
    snap.counter("qb_core_events_dropped_total", "QoS0 events discarded because the destination's mailbox was full.")
        .add(on_core(), stats.events_dropped);
    auto &dead = snap.counter("qb_core_dead_letters_total", "Events that reached no actor, by reason.");
    for (std::size_t r = 0; r < qb::DeadLetterReasons; ++r) {
        dead.add({{"core", core_id}, {"reason", std::string(qb::dead_letter_reason_name(static_cast<qb::DeadLetterReason>(r)))}},
                 stats.dead_letters_by_reason[r]);
    }
    snap.counter("qb_core_io_events_total", "qb-io callbacks the core's loop ran.").add(on_core(), stats.io_events);

    if (stats.pass_time.count > 0) {
        auto const seconds = [](qb::duration const d) {
            return std::chrono::duration<double>(d).count();
        };
        snap.counter("qb_core_passes_timed_total", "Passes timed (setPassTiming).").add(on_core(), stats.pass_time.count);
        snap.counter("qb_core_pass_time_seconds_total", "Time spent in the timed passes, park excluded.")
            .add(on_core(), seconds(stats.pass_time.total));
        snap.gauge("qb_core_pass_last_duration_seconds", "Duration of the core's last timed pass.")
            .add(on_core(), seconds(stats.pass_time.last));
        snap.gauge("qb_core_pass_max_duration_seconds", "Longest timed pass since timing began.").add(on_core(), seconds(stats.pass_time.max));
        snap.gauge("qb_core_pass_recent_max_duration_seconds", "Longest timed pass of the last one to two seconds.")
            .add(on_core(), seconds(stats.pass_time.recent_max));
    }
}

source_handle::source_handle(std::weak_ptr<detail::source_list> list, std::uint64_t const id) noexcept
    : _list(std::move(list))
    , _id(id) {}

source_handle::source_handle(source_handle &&other) noexcept
    : _list(std::move(other._list))
    , _id(std::exchange(other._id, 0)) {}

source_handle &
source_handle::operator=(source_handle &&other) noexcept {
    if (this != &other) {
        reset();
        _list = std::move(other._list);
        _id   = std::exchange(other._id, 0);
    }
    return *this;
}

source_handle::~source_handle() {
    reset();
}

void
source_handle::reset() noexcept {
    if (auto const list = _list.lock()) {
        auto &entries = list->entries;
        std::erase_if(entries, [id = _id](auto const &entry) { return entry.first == id; });
    }
    _list.reset();
    _id = 0;
}

bool
source_handle::active() const noexcept {
    if (auto const list = _list.lock()) {
        for (auto const &entry : list->entries)
            if (entry.first == _id)
                return true;
    }
    return false;
}

namespace detail {

qb::io::async::task<std::optional<snapshot>>
collect_from(qb::ScopedCoroContext ctx, qb::ActorId const registry, qb::duration const timeout) {
    try {
        auto answer = co_await qb::ask(ctx, registry, CollectRequest{}, timeout);
        co_return std::optional<snapshot>{std::move(answer.response)};
    } catch (qb::io::async::timeout_error const &) {
        co_return std::nullopt;
    }
}

} // namespace detail

} // namespace qb::metrics
