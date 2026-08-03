/**
 * @file qb/io/tests/benchmark/transport/tcp-loopback-echo.cpp
 * @brief Plain-TCP loopback echo round-trip throughput (no TLS, daemon-free).
 *
 * The raw asynchronous transport hot path: a `use<>::tcp::server` accepts a `use<>::tcp::client`
 * session over 127.0.0.1 and echoes newline-framed (`qb::protocol::text::command`) messages; a
 * standalone `use<>::tcp::client<>` fires a batch and counts the replies. This prices the real
 * socket round-trip — kernel loopback + the async read/frame/write path + libev dispatch — that
 * every network actor pays, with NO TLS (cf. session-json, which measures the SSL session) and NO
 * scripted stand-in (cf. async-bases-framing, which is socket-free).
 *
 * Single-loop model: on loopback `connect()` returns `Done` immediately, so client + server + the
 * accepted session all live on ONE `listener::current` loop driven by the benchmark thread — no
 * second thread is needed (that machinery in session-json exists only for the blocking TLS
 * handshake). Everything runs cooperatively on one thread, so the reply counter is a plain member.
 *
 * Methodology (perf harness, never a ctest gate): listener bring-up + connect + one warm-up
 * round-trip happen before the timed region; each timed iteration fires `batch` messages and pumps
 * the loop until all `batch` replies return, bounded by a pass cap so a stall fails via
 * `SkipWithError` instead of hanging. A final out-of-loop assert requires `received == sent` — a
 * dropped/merged frame can't report throughput. Teardown `clear()`s the loop before leaving.
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

#include <qb/io/async.h>
#include <qb/io/protocol/text.h>

namespace {

using namespace qb::io;

const std::string kPayload = "round-trip-payload-0123456789";

class EchoServer;

// Server-side accepted session: echo every framed message straight back.
class EchoServerSession : public use<EchoServerSession>::tcp::client<EchoServer> {
public:
    using Protocol = qb::protocol::text::command<EchoServerSession>;

    explicit EchoServerSession(IOServer &server)
        : client(server) {}

    void
    on(Protocol::message &&msg) {
        *this << msg.text << Protocol::end;
    }
};

// Acceptor.
class EchoServer : public use<EchoServer>::tcp::server<EchoServerSession> {
public:
    void
    on(IOSession &) {}
};

// Standalone client: count replies (single-thread loop ⇒ a plain member suffices).
class EchoClient : public use<EchoClient>::tcp::client<> {
public:
    using Protocol = qb::protocol::text::command<EchoClient>;

    std::size_t received = 0;

    void
    on(Protocol::message &&) {
        ++received;
    }
};

// Pump the single thread-local loop until `pred()` holds or the pass cap is hit.
template <typename Predicate>
bool
pump_until(Predicate &&pred, std::size_t const max_passes = 5'000'000u) {
    auto &loop = qb::io::async::listener::current;
    for (std::size_t i = 0; i < max_passes; ++i) {
        if (pred())
            return true;
        loop.run(EVRUN_NOWAIT);
    }
    return pred();
}

void
BM_Tcp_LoopbackEcho(benchmark::State &state) {
    const auto batch = static_cast<std::size_t>(state.range(0));
    qb::io::async::init(); // this thread owns the loop

    EchoServer server;
    if (server.transport().listen_v4(0, "127.0.0.1") != 0) {
        state.SkipWithError("listen_v4 on loopback failed");
        return;
    }
    const auto port = server.transport().local_endpoint().port(); // kernel-assigned ephemeral port
    server.start();

    EchoClient client;
    if (client.transport().connect(uri("tcp://127.0.0.1:" + std::to_string(port))) != SocketStatus::Done) {
        qb::io::async::listener::current.clear();
        state.SkipWithError("loopback connect failed");
        return;
    }
    client.start();

    // Warm up: establish the accept + a first round-trip before timing.
    client << kPayload << EchoClient::Protocol::end;
    if (!pump_until([&client] { return client.received >= 1; })) {
        qb::io::async::listener::current.clear();
        state.SkipWithError("warmup round-trip stalled");
        return;
    }
    const std::size_t warmup = client.received;

    std::size_t sent = 0;
    for (auto _ : state) {
        const std::size_t target = client.received + batch;
        for (std::size_t i = 0; i < batch; ++i)
            client << kPayload << EchoClient::Protocol::end;
        sent += batch;
        if (!pump_until([&client, target] { return client.received >= target; })) {
            state.SkipWithError("round-trip stalled (a reply never came back)");
            break;
        }
    }

    const std::size_t received = client.received;
    qb::io::async::listener::current.clear(); // dispose sessions/watchers before leaving

    if (received != sent + warmup) {
        state.SkipWithError("echo count mismatch: received != sent");
        return;
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(sent));
    state.SetBytesProcessed(static_cast<std::int64_t>(sent * 2u * kPayload.size())); // send + echo
}

} // namespace

BENCHMARK(BM_Tcp_LoopbackEcho)->Arg(1)->Arg(64)->Arg(512)->ArgNames({"batch"})->Unit(benchmark::kMicrosecond)->UseRealTime();

BENCHMARK_MAIN();
