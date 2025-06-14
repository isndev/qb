/**
 * @file qb/core/tests/benchmark/benchmark_example.cpp
 * @brief Example benchmark for the QB Actor Framework
 *
 * This file contains a simple benchmark example that demonstrates how to write and
 * register benchmarks for the QB Actor Framework. It provides a basic test case that
 * measures the performance of creating, starting, and joining a Main instance.
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
 * @ingroup Core
 */

#include <benchmark/benchmark.h>
#include <qb/actor.h>
#include <qb/main.h>

static void
BM_Main_Example(benchmark::State &state) {
    for (auto _ : state) {
        qb::Main main{0};

        main.start();
        main.join();
    }
}
// Register the function as a benchmark
BENCHMARK(BM_Main_Example);

BENCHMARK_MAIN();