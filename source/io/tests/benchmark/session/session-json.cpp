/**
 * @file qb/io/tests/benchmark/session/session-json.cpp
 * @brief Benchmarks for JSON session round-trips over loopback TCP and TLS.
 *
 * End-to-end session throughput: the `qb::io::async` event loop, the
 * `qb::protocol::json` (`\0`-delimited) framing protocol, and the
 * `use<>::tcp::client/server` (and `tcp::ssl::*`) session composition over a
 * real connected loopback socket pair. Each iteration sends a batch of JSON
 * frames client→server, the server echoes them back, and the loop is pumped
 * until the batch round-trips — msgs/sec across plaintext and TLS.
 *
 * THREADING MODEL — why the two variants differ
 * ---------------------------------------------
 * Plaintext TCP can run client AND server on a SINGLE `listener::current`
 * loop: `tcp::socket::connect` returns `Done` immediately on loopback (the
 * kernel finishes the 3-way handshake asynchronously), so one
 * `run(EVRUN_NOWAIT)` pump advances both ends. BM_SessionJson_Tcp keeps that
 * single-loop shape.
 *
 * TLS CANNOT. `tcp::ssl::socket::connect_v4` performs a SYNCHRONOUS handshake
 * (it blocks inside `SSL_connect` until the server answers). If the server
 * shares the calling thread's loop, that loop is never pumped while connect
 * blocks, so the handshake deadlocks — the historical hang of this bench.
 * BM_SessionJson_Tls therefore puts the client (connect + send + receive-drain)
 * on its OWN thread with its OWN event loop, and the main thread pumps the
 * server loop concurrently. This is the proven pattern from
 * system/tls/tls-text-roundtrip.cpp and system/session/session-json.cpp.
 *
 * Every wait is BOUNDED: the single-loop pump caps its pass count and the
 * cross-thread waits cap on a wall-clock deadline; on exceeding the budget the
 * bench calls `state.SkipWithError(...)` and stops — never an unbounded spin.
 * The connection + handshake is established ONCE before the timed loop; only
 * the per-batch send + round-trip drain is timed.
 *
 * Uses shared/loopback_fixture.h (ephemeral-port discipline) and
 * shared/ssl_fixtures.h (shipped cert/key). The integrator gates this target on
 * QB_HAS_SSL.
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

#include <atomic>
#include <benchmark/benchmark.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>

#include <qb/io/async.h>
#include <qb/json.h>

#include "../../shared/loopback_fixture.h"
#include "../../shared/ssl_fixtures.h"

using namespace qb::io;
using namespace std::chrono_literals;

namespace {

constexpr char STRING_MESSAGE[] = "Here is my content test";

// Per-benchmark-run counters: the server echo count and the client receive
// count. Reset before each connection is established.
std::atomic<std::size_t> g_server_echoes{0};
std::atomic<std::size_t> g_client_received{0};

// ---------------------------------------------------------------------------
// Pump the CURRENT thread's shared loop until `pred()` holds or `max_passes` is
// hit. The pass cap is the deadline guard: a stalled round-trip exhausts the
// budget and returns false instead of wedging the run. Returns whether the
// predicate was satisfied within budget.
// ---------------------------------------------------------------------------
template <typename Predicate>
[[nodiscard]] bool
pump_loop_until(Predicate &&pred, std::size_t max_passes = 2'000'000u) {
    auto &listener = qb::io::async::listener::current;
    for (std::size_t pass = 0; pass < max_passes; ++pass) {
        if (pred())
            return true;
        listener.run(EVRUN_NOWAIT);
    }
    return pred();
}

// ---------------------------------------------------------------------------
// Wait until `pred()` holds or the wall-clock `timeout` elapses, WITHOUT
// pumping a loop on this thread (used by the main thread to wait on a worker
// thread that drives its own loop). Bounded by construction. Returns whether
// the predicate was satisfied within budget.
// ---------------------------------------------------------------------------
template <typename Predicate, typename PumpStep>
[[nodiscard]] bool
wait_with_pump_until(Predicate &&pred, PumpStep &&pump_step, std::chrono::milliseconds timeout) {
    if (pred())
        return true;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        pump_step();
        if (pred())
            return true;
    } while (std::chrono::steady_clock::now() < deadline);
    return pred();
}

// ===========================================================================
// PLAINTEXT TCP session types — server echoes each frame, client counts replies
// ===========================================================================

class JsonTcpServer;

class JsonTcpServerSession : public use<JsonTcpServerSession>::tcp::client<JsonTcpServer> {
public:
    using Protocol = qb::protocol::json<JsonTcpServerSession>;
    explicit JsonTcpServerSession(IOServer &server)
        : client(server) {}
    void
    on(Protocol::message &&msg) {
        publish(msg.json, '\0');
        g_server_echoes.fetch_add(1, std::memory_order_relaxed);
    }
};

class JsonTcpServer : public use<JsonTcpServer>::tcp::server<JsonTcpServerSession> {
public:
    void
    on(IOSession &) {}
};

class JsonTcpClient : public use<JsonTcpClient>::tcp::client<> {
public:
    using Protocol = qb::protocol::json<JsonTcpClient>;
    void
    on(Protocol::message &&) {
        g_client_received.fetch_add(1, std::memory_order_relaxed);
    }
};

// ---------------------------------------------------------------------------
// JSON frames round-trip over plaintext loopback TCP. Server, accepted session,
// and client all share ONE `listener::current` loop — plaintext loopback
// connect is non-blocking, so a single `run(EVRUN_NOWAIT)` pump drives both
// ends. Each iteration publishes `batch` frames and pumps until all of them are
// echoed back. msgs/sec.
// ---------------------------------------------------------------------------
void
BM_SessionJson_Tcp(benchmark::State &state) {
    const auto batch = static_cast<std::size_t>(state.range(0));

    qb::io::async::init();
    g_server_echoes.store(0);
    g_client_received.store(0);

    JsonTcpServer server;
    if (server.transport().listen_v4(0, "127.0.0.1") != 0) {
        state.SkipWithError("failed to bind loopback TCP listener");
        return;
    }
    const auto port = server.transport().local_endpoint().port();
    server.start();

    JsonTcpClient client;
    if (client.transport().connect(uri{"tcp://127.0.0.1:" + std::to_string(port)}) != SocketStatus::Done) {
        state.SkipWithError("failed to connect loopback TCP client");
        return;
    }
    client.start();

    const qb::json frame{{"message", STRING_MESSAGE}};

    // Establish the round-trip ONCE (accept + first echo) before timing.
    client.publish(frame, '\0');
    if (!pump_loop_until([&] { return g_client_received.load(std::memory_order_relaxed) >= 1u; })) {
        qb::io::async::listener::current.clear();
        state.SkipWithError("TCP JSON warmup round-trip stalled");
        return;
    }
    const std::size_t warmup_received = g_client_received.load(std::memory_order_relaxed);

    std::size_t sent = 0;
    for (auto _ : state) {
        const auto target = g_client_received.load(std::memory_order_relaxed) + batch;
        for (std::size_t i = 0; i < batch; ++i)
            client.publish(frame, '\0');
        sent += batch;
        if (!pump_loop_until([&] { return g_client_received.load(std::memory_order_relaxed) >= target; })) {
            state.SkipWithError("TCP JSON round-trip stalled (pump budget exhausted)");
            break;
        }
    }

    const auto received = g_client_received.load(std::memory_order_relaxed);
    qb::io::async::listener::current.clear();

    if (received != sent + warmup_received)
        state.SkipWithError("TCP JSON round-trip count mismatch");

    state.SetItemsProcessed(static_cast<std::int64_t>(sent));
}

// ===========================================================================
// TLS session types — same shape over a self-signed loopback TLS channel
// ===========================================================================

#ifdef QB_HAS_SSL

class JsonSecureServer;

class JsonSecureServerSession : public use<JsonSecureServerSession>::tcp::ssl::client<JsonSecureServer> {
public:
    using Protocol = qb::protocol::json<JsonSecureServerSession>;
    explicit JsonSecureServerSession(IOServer &server)
        : client(server) {}
    void
    on(Protocol::message &&msg) {
        publish(msg.json, '\0');
        g_server_echoes.fetch_add(1, std::memory_order_relaxed);
    }
};

class JsonSecureServer : public use<JsonSecureServer>::tcp::ssl::server<JsonSecureServerSession> {
public:
    void
    on(IOSession &) {}
};

class JsonSecureClient : public use<JsonSecureClient>::tcp::ssl::client<> {
public:
    using Protocol = qb::protocol::json<JsonSecureClient>;
    void
    on(Protocol::message &&) {
        g_client_received.fetch_add(1, std::memory_order_relaxed);
    }
};

// ---------------------------------------------------------------------------
// JSON frames round-trip over loopback TLS.
//
// The client runs on its OWN thread + loop because the TLS connect is a
// synchronous blocking handshake (see file header). The main thread owns the
// server loop and drives the timing; the worker thread owns the client loop and
// reacts to batch requests posted by the main thread:
//
//   main thread (timed):    g_batch_request += batch  →  pump server loop +
//                           wait (bounded) until g_client_received hits target.
//   worker thread (loop):   pump client loop; whenever g_batch_request grows,
//                           publish the delta; exits on g_worker_stop.
//
// Both the warmup handshake and every per-batch wait are wall-clock bounded; a
// stall fails LOUD via SkipWithError. Only the per-batch send + drain is timed.
// ---------------------------------------------------------------------------
void
BM_SessionJson_Tls(benchmark::State &state) {
    const auto batch = static_cast<std::size_t>(state.range(0));

    if (!qb::io::test::require_ssl_files()) {
        state.SkipWithError("TLS certs (cert.pem/key.pem) are required for the TLS session benchmark");
        return;
    }

    qb::io::async::init();
    g_server_echoes.store(0);
    g_client_received.store(0);

    JsonSecureServer server;
    server.transport().init(ssl::create_server_context(SSLv23_server_method(), qb::io::test::ssl_resource_path("cert.pem"),
                                                       qb::io::test::ssl_resource_path("key.pem")));
    if (server.transport().listen_v4(0, "127.0.0.1") != 0) {
        state.SkipWithError("failed to bind loopback TLS listener");
        return;
    }
    const auto port = server.transport().local_endpoint().port();
    server.start();

    const qb::json frame{{"message", STRING_MESSAGE}};

    // Cross-thread coordination. g_batch_request is the running total of frames
    // the main thread has asked the worker to send; the worker sends the delta
    // it has not yet published. g_worker_ready signals the connection is up,
    // g_worker_failed signals a connect/handshake failure, g_worker_stop ends
    // the worker loop.
    std::atomic<std::size_t> g_batch_request{0};
    std::atomic<bool>        g_worker_ready{false};
    std::atomic<bool>        g_worker_failed{false};
    std::atomic<bool>        g_worker_stop{false};

    std::thread worker([&] {
        qb::io::async::init(); // the client owns this thread's loop
        JsonSecureClient client;
        client.transport().set_insecure(); // self-signed loopback cert
        if (client.transport().connect_v4("127.0.0.1", port) != 0) {
            g_worker_failed.store(true);
            g_worker_ready.store(true);
            return;
        }
        client.start();
        g_worker_ready.store(true);

        std::size_t published = 0;
        auto       &listener  = qb::io::async::listener::current;
        while (!g_worker_stop.load(std::memory_order_relaxed)) {
            const auto requested = g_batch_request.load(std::memory_order_relaxed);
            for (; published < requested; ++published)
                client.publish(frame, '\0');
            listener.run(EVRUN_NOWAIT);
        }
        // Drain any in-flight bytes so the loop's transports unwind cleanly.
        for (int i = 0; i < 1024; ++i)
            listener.run(EVRUN_NOWAIT);
        qb::io::async::listener::current.clear();
    });

    // Main thread pumps the SERVER loop one slice at a time. The slice spins a
    // bounded number of EVRUN_NOWAIT passes (tight round-trip latency once data
    // flows) and only yields the CPU when the slice made no progress on the
    // shared receive count — so we don't busy-burn a core while the worker is
    // still handshaking, yet add no idle latency once echoes are arriving.
    auto pump_server_slice = [] {
        const auto before = g_client_received.load(std::memory_order_relaxed);
        for (int i = 0; i < 256; ++i)
            qb::io::async::listener::current.run(EVRUN_NOWAIT);
        if (g_client_received.load(std::memory_order_relaxed) == before)
            std::this_thread::sleep_for(std::chrono::microseconds(20));
    };

    auto stop_worker = [&] {
        g_worker_stop.store(true);
        if (worker.joinable())
            worker.join();
    };

    // Wait for the worker to connect (or fail) — bounded.
    if (!wait_with_pump_until([&] { return g_worker_ready.load(); }, pump_server_slice, 5s)) {
        stop_worker();
        qb::io::async::listener::current.clear();
        state.SkipWithError("TLS client thread never became ready");
        return;
    }
    if (g_worker_failed.load()) {
        stop_worker();
        qb::io::async::listener::current.clear();
        state.SkipWithError("failed to connect loopback TLS client");
        return;
    }

    // Drive the handshake + first round-trip to completion before timing.
    g_batch_request.fetch_add(1, std::memory_order_relaxed);
    if (!wait_with_pump_until([&] { return g_client_received.load(std::memory_order_relaxed) >= 1u; }, pump_server_slice, 10s)) {
        stop_worker();
        qb::io::async::listener::current.clear();
        state.SkipWithError("TLS JSON warmup round-trip stalled");
        return;
    }
    const std::size_t warmup_received = g_client_received.load(std::memory_order_relaxed);

    std::size_t sent     = 0;
    bool        stalled  = false;
    for (auto _ : state) {
        const auto target = g_client_received.load(std::memory_order_relaxed) + batch;
        g_batch_request.fetch_add(batch, std::memory_order_relaxed);
        sent += batch;
        if (!wait_with_pump_until([&] { return g_client_received.load(std::memory_order_relaxed) >= target; },
                                  pump_server_slice, 10s)) {
            stalled = true;
            break;
        }
    }

    stop_worker();

    const auto received = g_client_received.load(std::memory_order_relaxed);
    qb::io::async::listener::current.clear();

    if (stalled)
        state.SkipWithError("TLS JSON round-trip stalled (deadline exceeded)");
    else if (received != sent + warmup_received)
        state.SkipWithError("TLS JSON round-trip count mismatch");

    state.SetItemsProcessed(static_cast<std::int64_t>(sent));
}

#endif // QB_HAS_SSL

} // namespace

BENCHMARK(BM_SessionJson_Tcp)->Arg(1)->Arg(64)->Arg(512)->ArgName("batch")->Unit(benchmark::kMicrosecond)->UseRealTime();
#ifdef QB_HAS_SSL
BENCHMARK(BM_SessionJson_Tls)->Arg(1)->Arg(64)->Arg(512)->ArgName("batch")->Unit(benchmark::kMicrosecond)->UseRealTime();
#endif

BENCHMARK_MAIN();
