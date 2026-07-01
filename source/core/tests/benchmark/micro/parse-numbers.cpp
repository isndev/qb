/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file benchmark/micro/parse-numbers.cpp
 * @brief Numeric string→value parse throughput: `qb::to_number` vs `std::from_chars` vs libc.
 *
 * `qb::to_number<T>` (<qb/system/parse.h>) is the framework's strict parser — it trims leading
 * C-locale whitespace, dispatches to `std::from_chars`, and requires the WHOLE view to be one
 * canonical number (returns `std::optional<T>`). The codebase migrated off `std::sto*`/`strtod`
 * onto it, so this micro prices that hot path against the raw `std::from_chars` floor and the old
 * libc `strtoll`/`strtod` idiom it replaced — on integer and floating corpora.
 *
 * Methodology (perf harness, never a ctest gate): the corpora are built once out of the timed
 * region; each iteration parses the entire corpus and folds every value into a checksum consumed
 * with `benchmark::DoNotOptimize`, so nothing is elided. A one-shot out-of-loop guard asserts
 * `qb::to_number` parses every entry AND agrees bit-for-bit with `std::from_chars` (a silently
 * mis-parsing wrapper can't report throughput). Counters: items/s (values) + bytes/s (corpus).
 */

#include <benchmark/benchmark.h>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include <qb/system/parse.h>

namespace {

// Expand a small set of representative literals into a fixed-size corpus (deterministic, no RNG):
// enough entries per iteration for a stable measurement, covering the interesting shapes.
std::vector<std::string>
expand(std::vector<std::string> const &base, std::size_t const reps) {
    std::vector<std::string> out;
    out.reserve(base.size() * reps);
    for (std::size_t r = 0; r < reps; ++r)
        for (auto const &s : base)
            out.push_back(s);
    return out;
}

// Integer shapes: zero, small, negative, both 32/64-bit extremes, round millions, hex-of-decimal.
const std::vector<std::string> kIntBase = {"0",          "7",          "-42",       "255",
                                           "65535",      "1000000",    "-1",        "2147483647",
                                           "-2147483648", "305419896", "9223372036854775807",
                                           "-9223372036854775808"};

// Floating shapes: integral, decimals, signed, scientific (±exp), tiny, a subnormal-ish value.
const std::vector<std::string> kDblBase = {"0.0",       "3.14159",     "-2.5",      "1e10",
                                           "6.022e23",  "-1.5e-9",     "0.000123",  "123456.789",
                                           "42",        "-0.0",        "2.5e-300",  "9.87654321e8"};

std::size_t
corpus_bytes(std::vector<std::string> const &c) {
    std::size_t n = 0;
    for (auto const &s : c)
        n += s.size();
    return n;
}

// --- integer parsers --------------------------------------------------------

void
BM_Parse_Int_QbToNumber(benchmark::State &state) {
    const auto corpus = expand(kIntBase, 64);

    // One-shot out-of-loop guard: qb::to_number must parse every entry AND match std::from_chars.
    {
        for (auto const &s : corpus) {
            auto        qb_v = qb::to_number<std::int64_t>(s);
            std::int64_t fc  = 0;
            const auto  r    = std::from_chars(s.data(), s.data() + s.size(), fc);
            const bool  fc_ok = (r.ec == std::errc{} && r.ptr == s.data() + s.size());
            if (!qb_v || !fc_ok || *qb_v != fc) {
                state.SkipWithError("qb::to_number<int64> disagrees with std::from_chars on the corpus");
                return;
            }
        }
    }

    // Accumulate in unsigned: the corpus spans the full int64 range, so a signed sum would overflow
    // (UB). Unsigned wraparound is well-defined and still a valid anti-elision sink.
    std::uint64_t sum = 0;
    for (auto _ : state) {
        for (auto const &s : corpus)
            sum += static_cast<std::uint64_t>(qb::to_number<std::int64_t>(s).value_or(0));
        benchmark::DoNotOptimize(sum);
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * corpus.size()));
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * corpus_bytes(corpus)));
}

void
BM_Parse_Int_FromChars(benchmark::State &state) {
    const auto    corpus = expand(kIntBase, 64);
    std::uint64_t sum    = 0; // unsigned: full-range corpus would overflow a signed sum (UB)
    for (auto _ : state) {
        for (auto const &s : corpus) {
            std::int64_t v = 0;
            std::from_chars(s.data(), s.data() + s.size(), v);
            sum += static_cast<std::uint64_t>(v);
        }
        benchmark::DoNotOptimize(sum);
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * corpus.size()));
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * corpus_bytes(corpus)));
}

void
BM_Parse_Int_Strtoll(benchmark::State &state) {
    const auto    corpus = expand(kIntBase, 64);
    std::uint64_t sum    = 0; // unsigned: full-range corpus would overflow a signed sum (UB)
    for (auto _ : state) {
        for (auto const &s : corpus)
            sum += static_cast<std::uint64_t>(std::strtoll(s.c_str(), nullptr, 10));
        benchmark::DoNotOptimize(sum);
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * corpus.size()));
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * corpus_bytes(corpus)));
}

// Lenient (stoi/strtol idiom) prefix parse — accepts a trailing remainder.
void
BM_Parse_Int_QbToNumberPrefix(benchmark::State &state) {
    const auto    corpus = expand(kIntBase, 64);
    std::uint64_t sum    = 0; // unsigned: full-range corpus would overflow a signed sum (UB)
    for (auto _ : state) {
        for (auto const &s : corpus)
            sum += static_cast<std::uint64_t>(qb::to_number_prefix<std::int64_t>(s).value_or(0));
        benchmark::DoNotOptimize(sum);
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * corpus.size()));
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * corpus_bytes(corpus)));
}

// --- floating parsers -------------------------------------------------------

void
BM_Parse_Double_QbToNumber(benchmark::State &state) {
    const auto corpus = expand(kDblBase, 64);

    // One-shot out-of-loop guard: parse every entry AND match std::from_chars bit-for-bit.
    {
        for (auto const &s : corpus) {
            auto       qb_v = qb::to_number<double>(s);
            double     fc   = 0.0;
            const auto r    = std::from_chars(s.data(), s.data() + s.size(), fc);
            const bool fc_ok = (r.ec == std::errc{} && r.ptr == s.data() + s.size());
            if (!qb_v || !fc_ok || *qb_v != fc) {
                state.SkipWithError("qb::to_number<double> disagrees with std::from_chars on the corpus");
                return;
            }
        }
    }

    double sum = 0.0;
    for (auto _ : state) {
        for (auto const &s : corpus)
            sum += qb::to_number<double>(s).value_or(0.0);
        benchmark::DoNotOptimize(sum);
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * corpus.size()));
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * corpus_bytes(corpus)));
}

void
BM_Parse_Double_FromChars(benchmark::State &state) {
    const auto corpus = expand(kDblBase, 64);
    double     sum    = 0.0;
    for (auto _ : state) {
        for (auto const &s : corpus) {
            double v = 0.0;
            std::from_chars(s.data(), s.data() + s.size(), v);
            sum += v;
        }
        benchmark::DoNotOptimize(sum);
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * corpus.size()));
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * corpus_bytes(corpus)));
}

void
BM_Parse_Double_Strtod(benchmark::State &state) {
    const auto corpus = expand(kDblBase, 64);
    double     sum    = 0.0;
    for (auto _ : state) {
        for (auto const &s : corpus)
            sum += std::strtod(s.c_str(), nullptr);
        benchmark::DoNotOptimize(sum);
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * corpus.size()));
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * corpus_bytes(corpus)));
}

} // namespace

BENCHMARK(BM_Parse_Int_QbToNumber)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_Parse_Int_FromChars)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_Parse_Int_Strtoll)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_Parse_Int_QbToNumberPrefix)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_Parse_Double_QbToNumber)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_Parse_Double_FromChars)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_Parse_Double_Strtod)->Unit(benchmark::kMicrosecond);

BENCHMARK_MAIN();
