/**
 * @file qb/io/tests/benchmark/bm-json-pipe.cpp
 * @brief Benchmarks for qb JSON pipe serialization workloads.
 *
 * qb-io extends pipe<char> with JSON serialization. These benchmarks exercise
 * primitive, array, object, and parse/serialize round-trip paths used by JSON
 * protocols and higher-level modules.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2025 qb - isndev (cpp.actor)
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
 * @ingroup IO
 */

#include <benchmark/benchmark.h>
#include <string>

#include <qb/json.h>
#include <qb/system/allocator/pipe.h>

namespace {

qb::json
make_object(std::size_t fields) {
    qb::json value;
    value["name"]    = "qb";
    value["version"] = 2;
    value["active"]  = true;
    for (std::size_t i = 0; i < fields; ++i) {
        value["field_" + std::to_string(i)] = {
            {"id", i},
            {"label", "benchmark-" + std::to_string(i)},
            {"enabled", (i % 2u) == 0u},
            {"weight", static_cast<double>(i) * 1.25},
        };
    }
    return value;
}

qb::json
make_array(std::size_t items) {
    qb::json value = qb::json::array();
    for (std::size_t i = 0; i < items; ++i)
        value.push_back({{"id", i}, {"payload", "item-" + std::to_string(i)}});
    return value;
}

void
BM_JsonPipe_Primitive(benchmark::State &state) {
    const qb::json            values[] = {nullptr, true, 42, 3.14159, "qb"};
    qb::allocator::pipe<char> pipe;

    for (auto _ : state) {
        for (auto const &value : values) {
            pipe.reset();
            pipe.put<qb::json>(value);
            benchmark::DoNotOptimize(pipe.begin());
            benchmark::DoNotOptimize(pipe.size());
        }
    }

    state.SetItemsProcessed(state.iterations() * 5);
}

void
BM_JsonPipe_Object(benchmark::State &state) {
    const auto                value = make_object(static_cast<std::size_t>(state.range(0)));
    qb::allocator::pipe<char> pipe;

    for (auto _ : state) {
        pipe.reset();
        pipe.put<qb::json>(value);
        benchmark::DoNotOptimize(pipe.begin());
        benchmark::DoNotOptimize(pipe.size());
    }

    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(value.dump().size()));
}

void
BM_JsonPipe_Array(benchmark::State &state) {
    const auto                value = make_array(static_cast<std::size_t>(state.range(0)));
    qb::allocator::pipe<char> pipe;

    for (auto _ : state) {
        pipe.reset();
        pipe.put<qb::json>(value);
        benchmark::DoNotOptimize(pipe.begin());
        benchmark::DoNotOptimize(pipe.size());
    }

    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(value.dump().size()));
}

void
BM_JsonPipe_ParseAndSerialize(benchmark::State &state) {
    const auto                value      = make_object(static_cast<std::size_t>(state.range(0)));
    const auto                serialized = value.dump();
    qb::allocator::pipe<char> pipe;

    for (auto _ : state) {
        auto parsed = qb::json::parse(serialized);
        pipe.reset();
        pipe.put<qb::json>(parsed);
        benchmark::DoNotOptimize(pipe.begin());
        benchmark::DoNotOptimize(pipe.size());
    }

    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(serialized.size()));
}

} // namespace

BENCHMARK(BM_JsonPipe_Primitive)->Unit(benchmark::kNanosecond);
BENCHMARK(BM_JsonPipe_Object)->Args({4})->Args({32})->Args({128})->ArgName("fields")->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_JsonPipe_Array)->Args({16})->Args({256})->Args({1024})->ArgName("items")->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_JsonPipe_ParseAndSerialize)->Args({4})->Args({32})->Args({128})->ArgName("fields")->Unit(benchmark::kMicrosecond);

BENCHMARK_MAIN();
