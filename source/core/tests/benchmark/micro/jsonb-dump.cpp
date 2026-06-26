/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file benchmark/micro/jsonb-dump.cpp
 * @brief `qb::json` / `qb::jsonb` serialize (`dump()`) + parse throughput on an http-shaped payload.
 *
 * Measures the two halves of the JSON round-trip the http module leans on, with no `qb::Main`:
 *   - `BM_Json_Dump`  / `BM_Jsonb_Dump`  — `value.dump()` on a pre-built object;
 *   - `BM_Json_Parse`              — `qb::json::parse(text)` of the same payload's serialized form.
 *
 * The payload is an http-shaped envelope (status, headers, a `meta` object, and a `data` array of
 * record objects) deliberately loaded with the number kinds that previously broke the qb pipe
 * serializer: 64-bit integer timestamps (`int64`/`uint64` that DO NOT fit in 32 bits, so a truncating
 * dump would corrupt them) and high-precision doubles. `payload_size` parameterises the record count
 * so throughput can be read against payload volume.
 *
 * Benchmark methodology (perf harness, never a ctest gate — no `EXPECT_LT(duration,…)`):
 *   - the payload object (dump benches) and the serialized text (parse bench) are built ONCE before
 *     the timed loop — only `dump()` / `parse()` is measured (the per-iteration string allocation
 *     `dump()` returns is inherent to the operation and intentionally inside the region);
 *   - the result is `DoNotOptimize`d every iteration so the call cannot be elided;
 *   - `SetBytesProcessed` publishes `bytes_per_second` against the serialized byte length;
 *   - a one-shot, out-of-loop correctness assert round-trips the payload and verifies a 64-bit
 *     timestamp and a double survive `parse(dump(x))` exactly (the int64-truncation regression guard),
 *     so a broken serializer is caught before timing rather than as a duration gate.
 *
 * New bench (no predecessor in the flat `benchmark/` set); exercises `qb/json.h`.
 */

#include <benchmark/benchmark.h>
#include <cmath>
#include <cstdint>
#include <string>

#include <qb/json.h>

namespace {

// A 64-bit timestamp that does NOT fit in 32 bits — a truncating serializer would corrupt it.
constexpr std::int64_t  kBigTimestampNs = 1'673'785'845'123'456'789LL;
constexpr std::uint64_t kBigCounter     = 4'294'967'296ULL; // 2^32, also > uint32 range
constexpr double        kPreciseDouble  = 3.141592653589793238;

// Build an http-shaped JSON envelope with `records` data rows, each carrying int64/uint64/double.
[[nodiscard]] qb::json
make_http_payload(std::size_t const records) {
    qb::json root;
    root["status"]                = 200;
    root["ok"]                    = true;
    root["headers"]               = {{"content-type", "application/json"}, {"x-request-id", "f3c1a2b4-0001-4abc-8def-0123456789ab"}};
    root["meta"]                  = qb::json::object();
    root["meta"]["server_time"]   = kBigTimestampNs; // int64 timestamp (the truncation tripwire)
    root["meta"]["request_count"] = kBigCounter;     // uint64 > 32-bit
    root["meta"]["latency_ms"]    = kPreciseDouble;  // high-precision double

    qb::json data = qb::json::array();
    for (std::size_t i = 0; i < records; ++i) {
        qb::json rec;
        rec["id"]          = static_cast<std::uint64_t>(i);
        rec["created_at"]  = kBigTimestampNs + static_cast<std::int64_t>(i);  // int64
        rec["balance"]     = kPreciseDouble * static_cast<double>(i + 1);     // double
        rec["name"]        = "record-" + std::to_string(i);
        rec["active"]      = (i % 2u) == 0u;
        rec["tags"]        = {"alpha", "beta", "gamma"};
        data.push_back(std::move(rec));
    }
    root["data"] = std::move(data);
    return root;
}

void
BM_Json_Dump(benchmark::State &state) {
    const auto        records = static_cast<std::size_t>(state.range(0));
    const qb::json    payload = make_http_payload(records);
    const std::size_t bytes   = payload.dump().size();

    for (auto _ : state) {
        std::string out = payload.dump();
        benchmark::DoNotOptimize(out.data());
        benchmark::ClobberMemory();
    }

    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * bytes));
    state.counters["serialized_bytes"] = static_cast<double>(bytes);
}

void
BM_Jsonb_Dump(benchmark::State &state) {
    const auto        records = static_cast<std::size_t>(state.range(0));
    const qb::jsonb   payload = qb::jsonb(make_http_payload(records));
    const std::size_t bytes   = payload.dump().size();

    for (auto _ : state) {
        std::string out = payload.dump();
        benchmark::DoNotOptimize(out.data());
        benchmark::ClobberMemory();
    }

    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * bytes));
    state.counters["serialized_bytes"] = static_cast<double>(bytes);
}

void
BM_Json_Parse(benchmark::State &state) {
    const auto        records = static_cast<std::size_t>(state.range(0));
    const std::string text    = make_http_payload(records).dump();
    const std::size_t bytes   = text.size();

    // One-shot out-of-loop correctness assert: a round-trip must preserve the 64-bit timestamp and the
    // precise double exactly (guards the int64/double truncation regression). Broken serialization
    // fails here, before timing — not as a duration gate.
    {
        const qb::json round = qb::json::parse(text);
        const bool     int64_ok =
            round.at("meta").at("server_time").get<std::int64_t>() == kBigTimestampNs;
        const bool double_ok = std::abs(round.at("meta").at("latency_ms").get<double>() - kPreciseDouble) < 1e-12;
        if (!int64_ok || !double_ok) {
            state.SkipWithError("json round-trip corrupted an int64 timestamp or a double");
            return;
        }
    }

    for (auto _ : state) {
        qb::json parsed = qb::json::parse(text);
        benchmark::DoNotOptimize(parsed);
    }

    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * bytes));
    state.counters["serialized_bytes"] = static_cast<double>(bytes);
}

} // namespace

BENCHMARK(BM_Json_Dump)->Arg(8)->Arg(64)->Arg(512)->ArgName("records")->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_Jsonb_Dump)->Arg(8)->Arg(64)->Arg(512)->ArgName("records")->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_Json_Parse)->Arg(8)->Arg(64)->Arg(512)->ArgName("records")->Unit(benchmark::kMicrosecond);

BENCHMARK_MAIN();
