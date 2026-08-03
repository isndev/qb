/**
 * @file qb/io/tests/benchmark/transport/async-bases-framing.cpp
 * @brief Benchmarks for the read→frame→onMessage→drain loop of the async I/O bases.
 *
 * Every qb-io session funnels inbound bytes through the same machine: pull bytes
 * from the transport into the input pipe (`istream::read()`), then run the
 * framing loop (`buffered_io::process_input()`) which asks the protocol for the
 * next frame, delivers it via `onMessage`, and flushes the consumed bytes. This
 * benchmark exercises that loop with a deterministic 4-byte length-prefixed
 * protocol, sourcing bytes from `shared/scripted_stream_transport.h` instead of
 * a kernel socket — so the measurement is pure buffer + framing cost, with no
 * syscall / scheduling noise. frames/sec + bytes/sec.
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
#include <cstddef>
#include <cstdint>
#include <string>

#include <qb/io/async/buffered_io.h>
#include <qb/io/protocol/base.h>
#include <qb/io/stream.h>

#include "../../shared/scripted_stream_transport.h"

using qb::io::test::ScriptedStreamTransport;

namespace {

// ---------------------------------------------------------------------------
// FramedSession: the framing CRTP bases wired to a scripted (socket-free)
// transport. It owns a `stream<ScriptedStreamTransport>` for the read side and
// inherits `buffered_io<>` for the protocol/frame machine — exactly the shape an
// async::input<> session has, minus the libev fd watcher. The read→frame loop is
//   read()              → pull scripted bytes into in()
//   process_input()     → frame, onMessage, flush consumed bytes
// ---------------------------------------------------------------------------
class FramedSession : public qb::io::async::buffered_io<FramedSession> {
    // `mutable`: stream::in() is non-const, but buffered_io's const accessors (pendingRead(),
    // the const in() overload) must read the same input pipe — reading the buffer is logically const.
    mutable qb::io::stream<ScriptedStreamTransport> _io;
    qb::allocator::pipe<char>                       _out;
    std::size_t                                     _max_write_buffer_size = QB_MAX_WRITE_BUFFER_SIZE;

public:
    using base_io_t = qb::io::async::buffered_io<FramedSession>;

    std::size_t frames_delivered = 0;
    std::size_t payload_bytes    = 0;

    explicit FramedSession(std::string script) {
        // Seed the scripted transport with the full wire image to hand back via read().
        _io.transport() = ScriptedStreamTransport{std::move(script)};
    }

    // --- buffered_io derived contract -------------------------------------
    qb::allocator::pipe<char> &
    in() noexcept {
        return _io.in();
    }
    qb::allocator::pipe<char> const &
    in() const noexcept {
        return _io.in();
    }
    qb::allocator::pipe<char> &
    out() noexcept {
        return _out;
    }
    qb::allocator::pipe<char> const &
    out() const noexcept {
        return _out;
    }
    [[nodiscard]] std::size_t
    pendingRead() const noexcept {
        return _io.in().size();
    }
    [[nodiscard]] std::size_t
    pendingWrite() const noexcept {
        return _out.size();
    }
    [[nodiscard]] std::size_t
    max_write_buffer_size() const noexcept {
        return _max_write_buffer_size;
    }
    void
    flush(std::size_t size) noexcept {
        _io.flush(size);
    }

    // --- transport read side ----------------------------------------------
    // Pull one bucket of scripted bytes into in(); returns the byte count read
    // (0 at end of script).
    int
    read() noexcept {
        const int ret = _io.read();
        if (ret > 0)
            account_read(static_cast<std::size_t>(ret));
        return ret;
    }

    // --- lifecycle hooks (counted, never reached on a clean drain) ---------
    void
    on(qb::io::async::event::pending_read &&) noexcept {}
    void
    on(qb::io::async::event::eof &&) noexcept {}
    void
    on(qb::io::async::event::disconnected &&) noexcept {}
    void
    on(qb::io::async::event::dispose &&) noexcept {}
};

// 4-byte little-endian length-prefixed protocol: [u32 payload_len][payload...].
// getMessageSize() returns the full frame size once the whole frame is buffered.
class LengthPrefixedProtocol : public qb::io::async::AProtocol<FramedSession> {
public:
    static constexpr std::size_t kHeader = 4u;

    explicit LengthPrefixedProtocol(FramedSession &io) noexcept
        : AProtocol(io) {}

    std::size_t
    getMessageSize() noexcept final {
        const auto pending = _io.pendingRead();
        if (pending < kHeader)
            return 0u;
        const auto       *raw     = reinterpret_cast<const unsigned char *>(_io.in().begin());
        const std::size_t payload = static_cast<std::size_t>(raw[0]) | (static_cast<std::size_t>(raw[1]) << 8)
                                    | (static_cast<std::size_t>(raw[2]) << 16) | (static_cast<std::size_t>(raw[3]) << 24);
        const std::size_t frame   = kHeader + payload;
        return pending >= frame ? frame : 0u;
    }

    void
    onMessage(std::size_t size) noexcept final {
        ++_io.frames_delivered;
        _io.payload_bytes += (size - kHeader);
    }

    void
    reset() noexcept final {}
};

// Build the wire image: `frames` back-to-back length-prefixed records of
// `payload_size` bytes each.
std::string
build_wire(std::size_t frames, std::size_t payload_size) {
    std::string wire;
    wire.reserve(frames * (LengthPrefixedProtocol::kHeader + payload_size));
    for (std::size_t f = 0; f < frames; ++f) {
        const auto len = static_cast<std::uint32_t>(payload_size);
        wire.push_back(static_cast<char>(len & 0xffu));
        wire.push_back(static_cast<char>((len >> 8) & 0xffu));
        wire.push_back(static_cast<char>((len >> 16) & 0xffu));
        wire.push_back(static_cast<char>((len >> 24) & 0xffu));
        for (std::size_t b = 0; b < payload_size; ++b)
            wire.push_back(static_cast<char>('a' + ((f + b) % 26u)));
    }
    return wire;
}

// ---------------------------------------------------------------------------
// Drive the full read→frame→onMessage→drain loop over the scripted wire image.
// Each iteration rebuilds a fresh session (the read cursor + buffers must reset)
// OUTSIDE the timed region (PauseTiming), then times only the read+frame loop.
// frames/sec + bytes/sec.
// ---------------------------------------------------------------------------
void
BM_Framing_ReadFrameDrain(benchmark::State &state) {
    const auto frames       = static_cast<std::size_t>(state.range(0));
    const auto payload_size = static_cast<std::size_t>(state.range(1));
    const auto wire         = build_wire(frames, payload_size);
    const auto wire_bytes   = wire.size();

    std::size_t last_frames  = 0;
    std::size_t last_payload = 0;
    for (auto _ : state) {
        state.PauseTiming();
        FramedSession session{wire};
        session.switch_protocol<LengthPrefixedProtocol>(session);
        state.ResumeTiming();

        // Pump the transport until the script is exhausted, framing as we go.
        while (session.read() > 0)
            benchmark::DoNotOptimize(session.process_input());
        // Final pass to drain any frame completed by the last read.
        benchmark::DoNotOptimize(session.process_input());

        last_frames  = session.frames_delivered;
        last_payload = session.payload_bytes;
        benchmark::DoNotOptimize(last_frames);
    }

    // One out-of-loop correctness assert: every frame must have been delivered.
    if (last_frames != frames || last_payload != frames * payload_size)
        state.SkipWithError("framing loop did not deliver every frame/byte");

    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(frames));
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(wire_bytes));
}

} // namespace

BENCHMARK(BM_Framing_ReadFrameDrain)
    ->Args({64, 16})
    ->Args({256, 64})
    ->Args({1024, 256})
    ->Args({256, 4096})
    ->ArgNames({"frames", "payload_bytes"})
    ->Unit(benchmark::kMicrosecond);

BENCHMARK_MAIN();
