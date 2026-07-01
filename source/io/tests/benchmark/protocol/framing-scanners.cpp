/**
 * @file qb/io/tests/benchmark/protocol/framing-scanners.cpp
 * @brief Benchmarks for qb protocol framing primitives.
 *
 * The protocol layer turns raw input pipes into framed application messages.
 * These benchmarks isolate delimiter scans, multi-byte terminators, binary
 * size headers, and JSON depth guards without involving sockets.
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

#include <benchmark/benchmark.h>
#include <cstdint>
#include <cstring>
#include <string>

#include <qb/io/protocol/base.h>
#include <qb/io/protocol/json.h>
#include <qb/system/allocator/pipe.h>

namespace {

struct ProtocolProbe {
    using base_io_t = ProtocolProbe;

    qb::allocator::pipe<char> input;
    bool                      ok = true;

    qb::allocator::pipe<char> &
    in() noexcept {
        return input;
    }
    qb::allocator::pipe<char> const &
    in() const noexcept {
        return input;
    }

    void
    not_ok() noexcept {
        ok = false;
    }
};

struct HttpHeaderEnd {
    static constexpr char _EndBytes[] = "\r\n\r\n";
};

template <char EndByte>
class BenchByteTerminated : public qb::protocol::base::byte_terminated<ProtocolProbe, EndByte> {
public:
    explicit BenchByteTerminated(ProtocolProbe &probe) noexcept
        : qb::protocol::base::byte_terminated<ProtocolProbe, EndByte>(probe) {}

    void
    onMessage(std::size_t) noexcept final {}
};

template <typename Trait>
class BenchBytesTerminated : public qb::protocol::base::bytes_terminated<ProtocolProbe, Trait> {
public:
    explicit BenchBytesTerminated(ProtocolProbe &probe) noexcept
        : qb::protocol::base::bytes_terminated<ProtocolProbe, Trait>(probe) {}

    void
    onMessage(std::size_t) noexcept final {}
};

template <typename Size>
class BenchSizeHeader : public qb::protocol::base::size_as_header<ProtocolProbe, Size> {
public:
    explicit BenchSizeHeader(ProtocolProbe &probe) noexcept
        : qb::protocol::base::size_as_header<ProtocolProbe, Size>(probe) {}

    void
    onMessage(std::size_t) noexcept final {}
};

std::string
make_line_frame(std::size_t payload_size) {
    std::string frame(payload_size, 'x');
    frame.push_back('\n');
    return frame;
}

std::string
make_http_header_frame(std::size_t headers) {
    std::string frame = "GET /resource HTTP/1.1\r\n";
    for (std::size_t i = 0; i < headers; ++i) {
        frame += "X-QB-Header-";
        frame += std::to_string(i);
        frame += ": value-";
        frame += std::to_string(i);
        frame += "\r\n";
    }
    frame += "\r\n";
    return frame;
}

template <typename Size>
std::string
make_binary_frame(std::size_t payload_size) {
    std::string frame(sizeof(Size) + payload_size, 'b');
    const auto  header = qb::protocol::base::size_as_header<ProtocolProbe, Size>::Header(payload_size);
    std::memcpy(frame.data(), &header, sizeof(Size));
    return frame;
}

std::string
make_nested_json(std::size_t depth) {
    std::string json;
    json.reserve(depth * 2u + 1u);
    for (std::size_t i = 0; i < depth; ++i)
        json.push_back('[');
    json.push_back('0');
    for (std::size_t i = 0; i < depth; ++i)
        json.push_back(']');
    return json;
}

void
BM_Protocol_ByteTerminatedScan(benchmark::State &state) {
    const auto                frame = make_line_frame(static_cast<std::size_t>(state.range(0)));
    ProtocolProbe             probe;
    BenchByteTerminated<'\n'> protocol(probe);

    for (auto _ : state) {
        probe.input.reset();
        std::memcpy(probe.input.allocate_back(frame.size()), frame.data(), frame.size());
        benchmark::DoNotOptimize(protocol.getMessageSize());
    }

    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(frame.size()));
}

void
BM_Protocol_BytesTerminatedScan(benchmark::State &state) {
    const auto                          frame = make_http_header_frame(static_cast<std::size_t>(state.range(0)));
    ProtocolProbe                       probe;
    BenchBytesTerminated<HttpHeaderEnd> protocol(probe);

    for (auto _ : state) {
        probe.input.reset();
        std::memcpy(probe.input.allocate_back(frame.size()), frame.data(), frame.size());
        benchmark::DoNotOptimize(protocol.getMessageSize());
    }

    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(frame.size()));
}

template <typename Size>
void
BM_Protocol_SizeHeader(benchmark::State &state) {
    const auto            frame = make_binary_frame<Size>(static_cast<std::size_t>(state.range(0)));
    ProtocolProbe         probe;
    BenchSizeHeader<Size> protocol(probe);

    for (auto _ : state) {
        probe.input.reset();
        std::memcpy(probe.input.allocate_back(frame.size()), frame.data(), frame.size());
        benchmark::DoNotOptimize(protocol.getMessageSize());
    }

    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(frame.size()));
}

void
BM_Protocol_JsonDepthGuard(benchmark::State &state) {
    const auto json = make_nested_json(static_cast<std::size_t>(state.range(0)));

    for (auto _ : state) {
        benchmark::DoNotOptimize(qb::protocol::detail::json_depth_within(json.data(), json.size(), qb::protocol::detail::kJsonMaxNestingDepth));
    }

    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(json.size()));
}

// Worst case for a byte-terminated scanner: a full buffer with NO delimiter.
// getMessageSize() must scan every byte and conclude "no complete frame yet"
// (returns 0) — the cost a slow/partial sender forces on the receive path each
// time more bytes arrive without completing a frame.
void
BM_Protocol_ByteTerminatedNoDelimiter(benchmark::State &state) {
    const auto                size = static_cast<std::size_t>(state.range(0));
    const std::string         frame(size, 'x'); // deliberately no '\n'
    ProtocolProbe             probe;
    BenchByteTerminated<'\n'> protocol(probe);

    // Build the delimiter-free buffer ONCE: getMessageSize()'s not-found path (base.h:106-108)
    // only advances the resume cursor `_offset`, it never consumes/mutates the buffer. So the
    // buffer persists, and each iteration must rewind the cursor with protocol.reset() — otherwise
    // _offset stays at end-of-buffer after iteration 1 and every later call scans ZERO bytes,
    // measuring nothing. Resetting (one assignment) instead of rebuilding the buffer per-iteration
    // also isolates the scan cost cleanly.
    std::memcpy(probe.input.allocate_back(frame.size()), frame.data(), frame.size());

    std::size_t last = 1;
    for (auto _ : state) {
        protocol.reset();
        last = protocol.getMessageSize();
        benchmark::DoNotOptimize(last);
    }

    // Out-of-loop correctness assert: a delimiter-free buffer must NOT yield a
    // frame; if it did, the scanner is broken and the numbers are meaningless.
    if (last != 0)
        state.SkipWithError("no-delimiter buffer reported a complete frame");

    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(frame.size()));
}

// Over-depth early-abort: input nested past kJsonMaxNestingDepth. json_depth_within
// must bail the instant depth exceeds the cap (after ~max_depth opening brackets),
// NOT scan the whole document — this measures that the guard short-circuits.
void
BM_Protocol_JsonDepthGuardOverDepth(benchmark::State &state) {
    const auto depth = static_cast<std::size_t>(state.range(0));
    const auto json  = make_nested_json(depth); // depth > 512 ⇒ must be rejected

    bool last = true;
    for (auto _ : state) {
        last = qb::protocol::detail::json_depth_within(json.data(), json.size(), qb::protocol::detail::kJsonMaxNestingDepth);
        benchmark::DoNotOptimize(last);
    }

    // Out-of-loop correctness assert: an over-depth document must be rejected.
    if (last)
        state.SkipWithError("over-depth JSON was not rejected by the depth guard");

    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(json.size()));
}

void
BM_Protocol_MsgpackDepthGuard(benchmark::State &state) {
    const auto     depth = static_cast<std::size_t>(state.range(0));
    nlohmann::json value = 0;
    for (std::size_t i = 0; i < depth; ++i)
        value = nlohmann::json::array({std::move(value)});
    const auto packed = nlohmann::json::to_msgpack(value);

    for (auto _ : state) {
        benchmark::DoNotOptimize(qb::protocol::detail::msgpack_depth_within(reinterpret_cast<const char *>(packed.data()), packed.size(),
                                                                            qb::protocol::detail::kJsonMaxNestingDepth));
    }

    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(packed.size()));
}

} // namespace

BENCHMARK(BM_Protocol_ByteTerminatedScan)->Args({32})->Args({1024})->Args({64 * 1024})->ArgName("payload_bytes")->Unit(benchmark::kNanosecond);
BENCHMARK(BM_Protocol_ByteTerminatedNoDelimiter)
    ->Args({32})
    ->Args({1024})
    ->Args({64 * 1024})
    ->ArgName("buffer_bytes")
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_Protocol_BytesTerminatedScan)->Args({4})->Args({32})->Args({128})->ArgName("headers")->Unit(benchmark::kNanosecond);
BENCHMARK_TEMPLATE(BM_Protocol_SizeHeader, std::uint16_t)
    ->Args({32})
    ->Args({1024})
    ->Args({60 * 1024})
    ->ArgName("payload_bytes")
    ->Unit(benchmark::kNanosecond);
BENCHMARK_TEMPLATE(BM_Protocol_SizeHeader, std::uint32_t)
    ->Args({32})
    ->Args({1024})
    ->Args({1024 * 1024})
    ->ArgName("payload_bytes")
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_Protocol_JsonDepthGuard)->Args({8})->Args({64})->Args({256})->ArgName("depth")->Unit(benchmark::kNanosecond);
BENCHMARK(BM_Protocol_JsonDepthGuardOverDepth)->Args({600})->Args({4096})->ArgName("depth")->Unit(benchmark::kNanosecond);
BENCHMARK(BM_Protocol_MsgpackDepthGuard)->Args({8})->Args({64})->Args({256})->ArgName("depth")->Unit(benchmark::kNanosecond);

BENCHMARK_MAIN();
