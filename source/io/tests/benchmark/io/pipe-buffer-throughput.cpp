/**
 * @file qb/io/tests/benchmark/io/pipe-buffer-throughput.cpp
 * @brief Benchmarks for qb::allocator::pipe<char> IO buffer workloads.
 *
 * The IO layer uses pipe<char> as the hot buffer for transport reads, writes,
 * protocol framing, and UDP datagram queues. These benchmarks isolate common
 * append, consume, reuse, and growth patterns without involving sockets.
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
 * @ingroup IO
 */

#include <algorithm>
#include <benchmark/benchmark.h>
#include <cstddef>
#include <cstring>
#include <string>

#include <qb/system/allocator/pipe.h>

namespace {

std::string
make_payload(std::size_t size) {
    std::string payload(size, '\0');
    for (std::size_t i = 0; i < payload.size(); ++i)
        payload[i] = static_cast<char>('a' + (i % 26u));
    return payload;
}

void
BM_Pipe_AppendReset(benchmark::State &state) {
    const auto payload_size = static_cast<std::size_t>(state.range(0));
    const auto payload      = make_payload(payload_size);

    qb::allocator::pipe<char> pipe;

    for (auto _ : state) {
        std::memcpy(pipe.allocate_back(payload.size()), payload.data(), payload.size());
        benchmark::DoNotOptimize(pipe.begin());
        benchmark::DoNotOptimize(pipe.size());
        pipe.reset();
    }

    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(payload_size));
    state.SetItemsProcessed(state.iterations());
}

void
BM_Pipe_PublishStylePut(benchmark::State &state) {
    const auto payload_size = static_cast<std::size_t>(state.range(0));
    const auto payload      = make_payload(payload_size);

    qb::allocator::pipe<char> pipe;

    for (auto _ : state) {
        pipe.put(payload);
        benchmark::DoNotOptimize(pipe.begin());
        benchmark::DoNotOptimize(pipe.size());
        pipe.reset();
    }

    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(payload_size));
    state.SetItemsProcessed(state.iterations());
}

void
BM_Pipe_ConsumeAndReuseWindow(benchmark::State &state) {
    const auto chunk_size = static_cast<std::size_t>(state.range(0));
    const auto payload    = make_payload(chunk_size);

    qb::allocator::pipe<char> pipe;
    std::memcpy(pipe.allocate_back(chunk_size), payload.data(), chunk_size);

    for (auto _ : state) {
        pipe.free_front(std::min<std::size_t>(chunk_size, pipe.size()));
        std::memcpy(pipe.allocate_back(chunk_size), payload.data(), chunk_size);
        benchmark::DoNotOptimize(pipe.begin());
        benchmark::DoNotOptimize(pipe.size());

        if (pipe.size() > chunk_size * 8u)
            pipe.reset();
    }

    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(chunk_size));
    state.SetItemsProcessed(state.iterations());
}

void
BM_Pipe_GrowLargeFrame(benchmark::State &state) {
    const auto frame_size = static_cast<std::size_t>(state.range(0));
    const auto payload    = make_payload(frame_size);

    for (auto _ : state) {
        qb::allocator::pipe<char> pipe;
        std::memcpy(pipe.allocate_back(payload.size()), payload.data(), payload.size());
        benchmark::DoNotOptimize(pipe.begin());
        benchmark::DoNotOptimize(pipe.size());
    }

    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(frame_size));
    state.SetItemsProcessed(state.iterations());
}

// Fragmentation pattern: drain the front in a SMALLER stride than we append at
// the back, so the live window slides forward across the buffer and forces the
// allocator's reorder()/compaction path — the realistic shape of a transport
// that consumes partial frames while more bytes keep arriving.
void
BM_Pipe_FragmentedSlidingWindow(benchmark::State &state) {
    const auto chunk_size = static_cast<std::size_t>(state.range(0));
    const auto drain_size = std::max<std::size_t>(chunk_size / 4u, 1u); // drain < append
    const auto payload    = make_payload(chunk_size);

    qb::allocator::pipe<char> pipe;
    std::memcpy(pipe.allocate_back(chunk_size), payload.data(), chunk_size);

    for (auto _ : state) {
        pipe.free_front(std::min<std::size_t>(drain_size, pipe.size()));
        std::memcpy(pipe.allocate_back(chunk_size), payload.data(), chunk_size);
        benchmark::DoNotOptimize(pipe.begin());
        benchmark::DoNotOptimize(pipe.size());

        // Cap unbounded growth: once the live window is large, fully drain it so
        // the next round starts the slide again (mirrors a flushed connection).
        if (pipe.size() > chunk_size * 16u) {
            pipe.free_front(pipe.size());
            std::memcpy(pipe.allocate_back(chunk_size), payload.data(), chunk_size);
        }
    }

    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(chunk_size));
    state.SetItemsProcessed(state.iterations());
}

} // namespace

BENCHMARK(BM_Pipe_AppendReset)->Args({64})->Args({1024})->Args({16 * 1024})->ArgNames({"bytes"})->Unit(benchmark::kNanosecond);
BENCHMARK(BM_Pipe_PublishStylePut)->Args({64})->Args({1024})->Args({16 * 1024})->ArgNames({"bytes"})->Unit(benchmark::kNanosecond);
BENCHMARK(BM_Pipe_ConsumeAndReuseWindow)->Args({64})->Args({1024})->Args({16 * 1024})->ArgNames({"chunk_bytes"})->Unit(benchmark::kNanosecond);
BENCHMARK(BM_Pipe_FragmentedSlidingWindow)->Args({64})->Args({1024})->Args({16 * 1024})->ArgNames({"chunk_bytes"})->Unit(benchmark::kNanosecond);
BENCHMARK(BM_Pipe_GrowLargeFrame)
    ->Args({4 * 1024})
    ->Args({64 * 1024})
    ->Args({1024 * 1024})
    ->ArgNames({"frame_bytes"})
    ->Unit(benchmark::kMicrosecond);

BENCHMARK_MAIN();
