//
// Created by isnDev on 2/23/2019.
//

#include <benchmark/benchmark.h>
#include <cube/actor.h>
#include <cube/main.h>

static void BM_Main_Example(benchmark::State& state) {
    for (auto _ : state) {
        qb::Main  main{0};

        main.start();
        main.join();
    }
}
// Register the function as a benchmark
BENCHMARK(BM_Main_Example);

BENCHMARK_MAIN();