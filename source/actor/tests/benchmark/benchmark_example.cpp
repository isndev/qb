//
// Created by isnDev on 2/23/2019.
//

#include <benchmark/benchmark.h>
#include <cube/actor/actor.h>
#include <cube/actor/main.h>

static void BM_StringCreation(benchmark::State& state) {
    for (auto _ : state)
        std::string empty_string;
}
// Register the function as a benchmark
BENCHMARK(BM_StringCreation);

// Define another benchmark
static void BM_StringCopy(benchmark::State& state) {
    std::string x = "hello";
    for (auto _ : state)
        std::string copy(x);
}
BENCHMARK(BM_StringCopy);

BENCHMARK_MAIN();