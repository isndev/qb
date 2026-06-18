/**
 * @file qb/io/tests/benchmark/bm-file-stream.cpp
 * @brief Benchmarks for qb file helpers and file-backed streams.
 *
 * These scenarios keep file throughput measurements out of GTest while still
 * exercising the same file_to_pipe, pipe_to_file, istream, and ostream paths
 * covered by the functional system tests.
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
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <qb/io/stream.h>
#include <qb/io/system/file.h>
#include <qb/system/allocator/pipe.h>
#include <string>
#include <string_view>
#include <tuple>

namespace {

std::string
make_payload(std::size_t size) {
    std::string out;
    out.reserve(size);
    constexpr std::string_view pattern = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abcdefghijklmnopqrstuvwxyz\n";
    while (out.size() < size)
        out.append(pattern.data(), std::min(pattern.size(), size - out.size()));
    return out;
}

std::filesystem::path
benchmark_directory() {
    const auto suffix = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    auto       dir    = std::filesystem::temp_directory_path() / ("qb-io-benchmark-" + std::to_string(suffix));
    std::filesystem::create_directories(dir);
    return dir;
}

void
write_payload(std::filesystem::path const &path, std::string const &payload) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(payload.data(), static_cast<std::streamsize>(payload.size()));
}

void
fill_pipe(qb::allocator::pipe<char> &pipe, std::string const &payload) {
    pipe.reset();
    auto *dst = pipe.allocate_back(payload.size());
    std::memcpy(dst, payload.data(), payload.size());
}

void
BM_FileToPipe_ReadAll(benchmark::State &state) {
    const auto size    = static_cast<std::size_t>(state.range(0));
    const auto payload = make_payload(size);
    const auto dir     = benchmark_directory();
    const auto input   = dir / "input.dat";
    write_payload(input, payload);

    for (auto _ : state) {
        qb::allocator::pipe<char> pipe;
        qb::io::sys::file_to_pipe reader(pipe);

        if (!reader.open(input.string())) {
            state.SkipWithError("file_to_pipe could not open input file");
            break;
        }

        while (!reader.eof()) {
            const int bytes = reader.read();
            if (bytes < 0) {
                state.SkipWithError("file_to_pipe read failed");
                break;
            }
        }

        benchmark::DoNotOptimize(pipe.begin());
        benchmark::DoNotOptimize(pipe.size());
    }

    std::filesystem::remove_all(dir);
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(size));
}

void
BM_PipeToFile_WriteAll(benchmark::State &state) {
    const auto size    = static_cast<std::size_t>(state.range(0));
    const auto payload = make_payload(size);
    const auto dir     = benchmark_directory();
    int        index   = 0;

    for (auto _ : state) {
        state.PauseTiming();
        qb::allocator::pipe<char> pipe;
        fill_pipe(pipe, payload);
        const auto output = dir / ("pipe-output-" + std::to_string(index++) + ".dat");
        state.ResumeTiming();

        qb::io::sys::pipe_to_file writer(pipe);
        if (!writer.open(output.string())) {
            state.SkipWithError("pipe_to_file could not open output file");
            break;
        }

        int result = writer.write_all();
        benchmark::DoNotOptimize(result);
        benchmark::DoNotOptimize(writer.written_bytes());

        state.PauseTiming();
        std::filesystem::remove(output);
        state.ResumeTiming();
    }

    std::filesystem::remove_all(dir);
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(size));
}

void
BM_FileIStream_ReadAll(benchmark::State &state) {
    const auto size    = static_cast<std::size_t>(state.range(0));
    const auto payload = make_payload(size);
    const auto dir     = benchmark_directory();
    const auto input   = dir / "stream-input.dat";
    write_payload(input, payload);

    for (auto _ : state) {
        qb::io::sys::file file;
        if (file.open(input.string(), O_RDONLY) < 0) {
            state.SkipWithError("stream input file open failed");
            break;
        }

        qb::io::istream<qb::io::sys::file> stream;
        stream.transport() = std::move(file);

        std::size_t total = 0;
        while (total < size) {
            const int bytes = stream.read();
            if (bytes <= 0)
                break;
            total += stream.in().size();
            stream.flush(stream.in().size());
        }

        benchmark::DoNotOptimize(total);
        stream.close();
    }

    std::filesystem::remove_all(dir);
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(size));
}

void
BM_FileOStream_WriteAll(benchmark::State &state) {
    const auto size    = static_cast<std::size_t>(state.range(0));
    const auto payload = make_payload(size);
    const auto dir     = benchmark_directory();
    int        index   = 0;

    for (auto _ : state) {
        state.PauseTiming();
        const auto output = dir / ("stream-output-" + std::to_string(index++) + ".dat");
        state.ResumeTiming();

        qb::io::sys::file file;
        if (file.open(output.string(), O_WRONLY | O_CREAT | O_TRUNC, 0644) < 0) {
            state.SkipWithError("stream output file open failed");
            break;
        }

        qb::io::ostream<qb::io::sys::file> stream;
        stream.transport() = std::move(file);
        std::ignore        = stream.publish(payload.data(), payload.size());

        while (stream.pendingWrite() > 0u) {
            const int bytes = stream.write();
            if (bytes <= 0)
                break;
        }

        benchmark::DoNotOptimize(stream.pendingWrite());
        stream.close();

        state.PauseTiming();
        std::filesystem::remove(output);
        state.ResumeTiming();
    }

    std::filesystem::remove_all(dir);
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(size));
}

} // namespace

BENCHMARK(BM_FileToPipe_ReadAll)->Args({4 * 1024})->Args({1024 * 1024})->ArgName("bytes")->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_PipeToFile_WriteAll)->Args({4 * 1024})->Args({1024 * 1024})->ArgName("bytes")->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_FileIStream_ReadAll)->Args({4 * 1024})->Args({1024 * 1024})->ArgName("bytes")->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_FileOStream_WriteAll)->Args({4 * 1024})->Args({1024 * 1024})->ArgName("bytes")->Unit(benchmark::kMicrosecond);

BENCHMARK_MAIN();
