/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/metrics/metrics-text.cpp
 * @brief `qb::metrics`: the snapshot model and its Prometheus text exposition, format 0.0.4 (Huly QB-154).
 *
 * Pinned, each byte exact:
 *   - the exposition of counters, gauges and histograms: `# HELP` (escaped) and `# TYPE` once per family, the
 *     label values escaped, an integer printed exactly past 2^53, a double in its shortest round-trip form,
 *     `+Inf` / `-Inf` / `NaN`, histogram buckets cumulated up to `le="+Inf"`, `_sum` only when known, `_count`;
 *   - names refused before anything is emitted: an invalid metric or label name, a repeated label, `le` on a
 *     histogram, bounds not finite or not ascending, buckets that do not count `bounds + 1`, one name asked
 *     under two types;
 *   - the merge: families meet by name, one label set in any order is one series and is summed (integers stay
 *     exact), a type conflict or a bucket-layout conflict is dropped and counted, a histogram sum survives only
 *     when every part had one;
 *   - `describe(CoreStats)`: the `qb_core_*` families, `core` on every sample, the pass-timing families only
 *     when the core timed its passes.
 */

#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>
#include <qb/core/metrics.h>

namespace metrics_text_test {

using qb::metrics::snapshot;

TEST(MetricsText, TheExpositionIsByteExact) {
    snapshot s;
    s.counter("app_requests_total", "Requests,\nby path \\ method.")
        .add({{"path", "/a\"b"}, {"method", "GET"}}, 3)
        .add({{"path", "line\nbreak\\"}, {"method", "POST"}}, std::uint64_t{9007199254740993}); // 2^53 + 1
    s.gauge("app_temperature", "A gauge.").add({}, 21.5).add({{"where", "out"}}, -3).add({{"where", "nan"}}, std::nan(""));
    s.gauge("app_limits", "Infinities.")
        .add({{"side", "up"}}, std::numeric_limits<double>::infinity())
        .add({{"side", "down"}}, -std::numeric_limits<double>::infinity());
    s.histogram("app_latency_seconds", "A histogram.")
        .add({{"route", "/x"}}, {0.005, 0.5, 1.0}, {1, 2, 0, 4}, 7.25)
        .add({{"route", "/y"}}, {0.005, 0.5, 1.0}, {0, 0, 1, 0}, std::nullopt);
    s.counter("app_unused_total", "A family with no sample is not printed.");

    const std::string expected = "# HELP app_requests_total Requests,\\nby path \\\\ method.\n"
                                 "# TYPE app_requests_total counter\n"
                                 "app_requests_total{path=\"/a\\\"b\",method=\"GET\"} 3\n"
                                 "app_requests_total{path=\"line\\nbreak\\\\\",method=\"POST\"} 9007199254740993\n"
                                 "# HELP app_temperature A gauge.\n"
                                 "# TYPE app_temperature gauge\n"
                                 "app_temperature 21.5\n"
                                 "app_temperature{where=\"out\"} -3\n"
                                 "app_temperature{where=\"nan\"} NaN\n"
                                 "# HELP app_limits Infinities.\n"
                                 "# TYPE app_limits gauge\n"
                                 "app_limits{side=\"up\"} +Inf\n"
                                 "app_limits{side=\"down\"} -Inf\n"
                                 "# HELP app_latency_seconds A histogram.\n"
                                 "# TYPE app_latency_seconds histogram\n"
                                 "app_latency_seconds_bucket{route=\"/x\",le=\"0.005\"} 1\n"
                                 "app_latency_seconds_bucket{route=\"/x\",le=\"0.5\"} 3\n"
                                 "app_latency_seconds_bucket{route=\"/x\",le=\"1\"} 3\n"
                                 "app_latency_seconds_bucket{route=\"/x\",le=\"+Inf\"} 7\n"
                                 "app_latency_seconds_sum{route=\"/x\"} 7.25\n"
                                 "app_latency_seconds_count{route=\"/x\"} 7\n"
                                 "app_latency_seconds_bucket{route=\"/y\",le=\"0.005\"} 0\n"
                                 "app_latency_seconds_bucket{route=\"/y\",le=\"0.5\"} 0\n"
                                 "app_latency_seconds_bucket{route=\"/y\",le=\"1\"} 1\n"
                                 "app_latency_seconds_bucket{route=\"/y\",le=\"+Inf\"} 1\n"
                                 "app_latency_seconds_count{route=\"/y\"} 1\n";
    EXPECT_EQ(qb::metrics::write_text(s), expected);
    EXPECT_EQ(qb::metrics::text_content_type, "text/plain; version=0.0.4; charset=utf-8");
}

TEST(MetricsText, InvalidShapesAreRefused) {
    snapshot s;
    EXPECT_THROW(s.counter("1starts_with_a_digit", ""), std::invalid_argument);
    EXPECT_THROW(s.counter("has-a-dash_total", ""), std::invalid_argument);
    EXPECT_THROW(s.gauge("", ""), std::invalid_argument);
    EXPECT_NO_THROW(s.gauge("ns:sub_name", ""));

    auto &c = s.counter("ok_total", "");
    EXPECT_THROW(c.add({{"__reserved", "x"}}, 1), std::invalid_argument);
    EXPECT_THROW(c.add({{"9lives", "x"}}, 1), std::invalid_argument);
    EXPECT_THROW(c.add({{"a-b", "x"}}, 1), std::invalid_argument);
    EXPECT_THROW(c.add({{"a", "x"}, {"a", "y"}}, 1), std::invalid_argument);
    EXPECT_THROW(c.add({}, {0.1}, {1, 2}, std::nullopt), std::logic_error) << "a histogram sample on a counter";
    EXPECT_TRUE(c.empty());

    auto &h = s.histogram("ok_seconds", "");
    EXPECT_THROW(h.add({}, 1), std::logic_error) << "a plain sample on a histogram";
    EXPECT_THROW(h.add({{"le", "1"}}, {0.1}, {1, 2}, std::nullopt), std::invalid_argument);
    EXPECT_THROW(h.add({}, {0.1, 0.1}, {1, 2, 3}, std::nullopt), std::invalid_argument) << "bounds not strictly ascending";
    EXPECT_THROW(h.add({}, {0.5, 0.1}, {1, 2, 3}, std::nullopt), std::invalid_argument);
    EXPECT_THROW(h.add({}, {0.1, std::numeric_limits<double>::infinity()}, {1, 2, 3}, std::nullopt), std::invalid_argument);
    EXPECT_THROW(h.add({}, {0.1}, {1}, std::nullopt), std::invalid_argument) << "buckets must count bounds + 1";
    EXPECT_TRUE(h.empty());

    EXPECT_THROW(s.gauge("ok_total", ""), std::invalid_argument) << "one name, two types";
    EXPECT_EQ(&s.counter("ok_total", "ignored"), &s.counter("ok_total", "")) << "the family is found again, not duplicated";
    EXPECT_TRUE(qb::metrics::write_text(s).empty()) << "nothing was added, nothing is printed";
}

TEST(MetricsText, TheMergeSumsOneSeriesAndKeepsIntegersExact) {
    snapshot a;
    a.counter("hits_total", "Hits.").add({{"route", "/x"}, {"method", "GET"}}, std::uint64_t{9007199254740993});
    a.gauge("load", "Load.").add({{"core", "0"}}, 1);
    snapshot b;
    b.counter("hits_total", "Other help: the first one stays.").add({{"method", "GET"}, {"route", "/x"}}, 2); // same series, other order
    b.counter("hits_total", "").add({{"method", "GET"}, {"route", "/y"}}, 5);
    b.gauge("load", "").add({{"core", "0"}}, 0.5);

    a.merge(std::move(b));
    a.canonicalize();
    EXPECT_EQ(a.conflicts(), 0u);
    EXPECT_EQ(qb::metrics::write_text(a), "# HELP hits_total Hits.\n"
                                          "# TYPE hits_total counter\n"
                                          "hits_total{route=\"/x\",method=\"GET\"} 9007199254740995\n"
                                          "hits_total{method=\"GET\",route=\"/y\"} 5\n"
                                          "# HELP load Load.\n"
                                          "# TYPE load gauge\n"
                                          "load{core=\"0\"} 1.5\n");
}

TEST(MetricsText, TheMergeDropsAndCountsConflicts) {
    snapshot a;
    a.counter("x_total", "").add({}, 1);
    a.histogram("lat_seconds", "").add({{"k", "v"}}, {0.1, 1.0}, {1, 1, 1}, 0.5);
    snapshot b;
    b.gauge("x_total", "").add({}, 2);                                            // a counter elsewhere
    b.histogram("lat_seconds", "").add({{"k", "v"}}, {0.2, 1.0}, {1, 1, 1}, 0.5); // another layout
    b.histogram("lat_seconds", "").add({{"k", "w"}}, {0.1, 1.0}, {0, 2, 0}, std::nullopt);
    snapshot c;
    c.histogram("lat_seconds", "").add({{"k", "w"}}, {0.1, 1.0}, {1, 0, 0}, 0.25); // sums: one part has none

    a.merge(std::move(b));
    a.merge(std::move(c));
    a.canonicalize();
    EXPECT_EQ(a.conflicts(), 2u);
    EXPECT_EQ(qb::metrics::write_text(a), "# HELP x_total \n"
                                          "# TYPE x_total counter\n"
                                          "x_total 1\n"
                                          "# HELP lat_seconds \n"
                                          "# TYPE lat_seconds histogram\n"
                                          "lat_seconds_bucket{k=\"v\",le=\"0.1\"} 1\n"
                                          "lat_seconds_bucket{k=\"v\",le=\"1\"} 2\n"
                                          "lat_seconds_bucket{k=\"v\",le=\"+Inf\"} 3\n"
                                          "lat_seconds_sum{k=\"v\"} 0.5\n"
                                          "lat_seconds_count{k=\"v\"} 3\n"
                                          "lat_seconds_bucket{k=\"w\",le=\"0.1\"} 1\n"
                                          "lat_seconds_bucket{k=\"w\",le=\"1\"} 3\n"
                                          "lat_seconds_bucket{k=\"w\",le=\"+Inf\"} 3\n"
                                          "lat_seconds_count{k=\"w\"} 3\n");
}

TEST(MetricsText, ValuesAddExactlyOrAsDoubles) {
    qb::metrics::value v = std::uint64_t{1} << 60;
    v += 1;
    EXPECT_TRUE(v.is_integer());
    EXPECT_EQ(v.integer(), (std::uint64_t{1} << 60) + 1);
    v += 0.5;
    EXPECT_FALSE(v.is_integer());
    qb::metrics::value negative = -2;
    EXPECT_FALSE(negative.is_integer());
    EXPECT_EQ(negative.as_double(), -2.0);
}

TEST(MetricsText, DescribeCoreStatsLabelsEveryCoreSample) {
    qb::CoreStats stats{};
    stats.loop_passes                                                                       = 10;
    stats.events_received                                                                   = 7;
    stats.buckets_received                                                                  = 14;
    stats.events_sent                                                                       = 5;
    stats.buckets_sent                                                                      = 6;
    stats.sends_blocked                                                                     = 2;
    stats.events_dropped                                                                    = 1;
    stats.dead_letters                                                                      = 3;
    stats.dead_letters_by_reason[static_cast<std::size_t>(qb::DeadLetterReason::unhandled)] = 3;
    stats.io_events                                                                         = 4;

    snapshot s;
    qb::metrics::describe(s, stats, 3);
    const std::string text = qb::metrics::write_text(s);
    for (auto const *line :
         {"qb_core_loop_passes_total{core=\"3\"} 10\n", "qb_core_events_received_total{core=\"3\"} 7\n",
          "qb_core_event_buckets_received_total{core=\"3\"} 14\n", "qb_core_events_sent_total{core=\"3\"} 5\n",
          "qb_core_event_buckets_sent_total{core=\"3\"} 6\n", "qb_core_sends_blocked_total{core=\"3\"} 2\n",
          "qb_core_events_dropped_total{core=\"3\"} 1\n", "qb_core_dead_letters_total{core=\"3\",reason=\"unhandled\"} 3\n",
          "qb_core_dead_letters_total{core=\"3\",reason=\"not_found\"} 0\n", "qb_core_io_events_total{core=\"3\"} 4\n"})
        EXPECT_NE(text.find(line), std::string::npos) << line << "\nin:\n" << text;
    ASSERT_NE(s.find("qb_core_dead_letters_total"), nullptr);
    EXPECT_EQ(s.find("qb_core_dead_letters_total")->samples.size(), qb::DeadLetterReasons);
    EXPECT_EQ(s.find("qb_core_passes_timed_total"), nullptr) << "no pass timing: no pass-timing family";

    stats.pass_time.count      = 2;
    stats.pass_time.total      = std::chrono::milliseconds(3);
    stats.pass_time.last       = std::chrono::microseconds(500);
    stats.pass_time.max        = std::chrono::milliseconds(2);
    stats.pass_time.recent_max = std::chrono::milliseconds(1);
    snapshot timed;
    qb::metrics::describe(timed, stats, 0);
    const std::string timed_text = qb::metrics::write_text(timed);
    for (auto const *line :
         {"qb_core_passes_timed_total{core=\"0\"} 2\n", "qb_core_pass_time_seconds_total{core=\"0\"} 0.003\n",
          "qb_core_pass_last_duration_seconds{core=\"0\"} 0.0005\n", "qb_core_pass_max_duration_seconds{core=\"0\"} 0.002\n",
          "qb_core_pass_recent_max_duration_seconds{core=\"0\"} 0.001\n"})
        EXPECT_NE(timed_text.find(line), std::string::npos) << line << "\nin:\n" << timed_text;
}

TEST(MetricsText, ListenerDispatchScopeIsExplicitAndLastRunEventsIsAGauge) {
    snapshot untimed;
    qb::metrics::describe_dispatch(untimed, qb::TimingStats{}, 7, {{"listener", "standalone"}});
    EXPECT_EQ(untimed.families().size(), 1u);
    const auto *events = untimed.find("qb_io_last_run_events");
    ASSERT_NE(events, nullptr);
    EXPECT_EQ(events->type, qb::metrics::metric_type::gauge);
    EXPECT_EQ(events->samples[0].number.integer(), 7u);
    EXPECT_EQ(untimed.find("qb_io_dispatches_timed_total"), nullptr);

    qb::TimingStats stats;
    stats.count      = 9007199254740993ULL;
    stats.total      = std::chrono::milliseconds(125);
    stats.last       = std::chrono::milliseconds(2);
    stats.max        = std::chrono::milliseconds(10);
    stats.recent_max = std::chrono::milliseconds(5);
    snapshot timed;
    qb::metrics::describe_dispatch(timed, stats, 0, {{"listener", "standalone"}});
    const auto text = qb::metrics::write_text(timed);
    for (const auto *line :
         {"qb_io_last_run_events{listener=\"standalone\"} 0\n", "qb_io_dispatches_timed_total{listener=\"standalone\"} 9007199254740993\n",
          "qb_io_dispatch_time_seconds_total{listener=\"standalone\"} 0.125\n",
          "qb_io_dispatch_last_duration_seconds{listener=\"standalone\"} 0.002\n",
          "qb_io_dispatch_max_duration_seconds{listener=\"standalone\"} 0.01\n",
          "qb_io_dispatch_recent_max_duration_seconds{listener=\"standalone\"} 0.005\n"})
        EXPECT_NE(text.find(line), std::string::npos) << line;
}

} // namespace metrics_text_test
