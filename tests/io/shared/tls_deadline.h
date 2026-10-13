/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

#ifndef QB_IO_TESTS_SHARED_TLS_DEADLINE_H
#define QB_IO_TESTS_SHARED_TLS_DEADLINE_H

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include <qb/io/async/tcp/connector.h>
#include <qb/io/tcp/ssl/socket.h>
#include <qb/io/uri.h>

namespace qb::io::test {

inline void
clear_tls_blocking_io_timeout(tcp::ssl::socket &socket) {
#if defined(_WIN32)
    const DWORD native_timeout = 0;
#else
    const timeval native_timeout{};
#endif
    (void) socket.set_optval(SOL_SOCKET, SO_RCVTIMEO, native_timeout);
    (void) socket.set_optval(SOL_SOCKET, SO_SNDTIMEO, native_timeout);
}

inline bool
set_tls_blocking_io_timeout(tcp::ssl::socket &socket, std::chrono::steady_clock::duration timeout) {
#if defined(_WIN32)
    const auto timeout_ms     = std::chrono::ceil<std::chrono::milliseconds>(timeout).count();
    const auto max_timeout_ms = static_cast<decltype(timeout_ms)>((std::numeric_limits<DWORD>::max)());
    const auto native_timeout = static_cast<DWORD>(std::clamp(timeout_ms, decltype(timeout_ms){1}, max_timeout_ms));
#else
    const auto    timeout_us = std::max<std::chrono::microseconds::rep>(1, std::chrono::ceil<std::chrono::microseconds>(timeout).count());
    const timeval native_timeout{
        static_cast<decltype(timeval::tv_sec)>(timeout_us / std::micro::den),
        static_cast<decltype(timeval::tv_usec)>(timeout_us % std::micro::den)
    };
#endif
    if (socket.set_optval(SOL_SOCKET, SO_RCVTIMEO, native_timeout) != 0)
        return false;
    if (socket.set_optval(SOL_SOCKET, SO_SNDTIMEO, native_timeout) == 0)
        return true;
    const int timeout_error = qb::io::socket::get_last_errno();
    clear_tls_blocking_io_timeout(socket);
    qb::io::socket::set_last_errno(timeout_error);
    return false;
}

// Own the connection and its callback on the calling thread's otherwise-empty
// loop. Clearing an unfinished connector before returning keeps its callback's
// borrowed state alive through cancellation. No blocking TLS call reaches a join.
inline bool
connect_tls_worker_until(tcp::ssl::socket &destination, std::uint16_t port, const std::atomic<bool> &stop,
                         std::chrono::steady_clock::time_point deadline) {
    if (stop.load() || std::chrono::steady_clock::now() >= deadline)
        return false;
    bool connected = false;
    bool finished  = false;
    async::tcp::connect<tcp::ssl::socket>(
        uri{"tcp://127.0.0.1:" + std::to_string(port)},
        [&](tcp::ssl::socket &&socket) {
            connected = socket.is_open() && socket.handshake_complete();
            if (connected)
                destination = std::move(socket);
            finished = true;
        },
        deadline - std::chrono::steady_clock::now(), false);
    while (!finished && !stop.load() && std::chrono::steady_clock::now() < deadline)
        async::listener::current.run(EVRUN_NOWAIT);
    if (!connected || stop.load() || std::chrono::steady_clock::now() >= deadline) {
        async::listener::current.clear();
        destination.disconnect();
        return false;
    }
    return true;
}

// Keep synchronous connect/handshake coverage while bounding the fixture. The
// pre-opened descriptor is never replaced by connect's address fallback. Its
// owner outlives the watchdog, which only invokes native shutdown: it neither
// closes/recycles the descriptor nor accesses SSL or the socket's mutable state.
// The socket I/O timeouts release an SSL_do_handshake call already blocked in
// Winsock recv/send: Windows shutdown disables later I/O but does not cancel the
// call in progress. Repeating shutdown still covers preemption before TCP connects.
inline int
connect_tls_blocking_until(tcp::ssl::socket &socket, std::uint16_t port, std::chrono::steady_clock::time_point deadline) {
    if (std::chrono::steady_clock::now() >= deadline)
        return -1;
    if (socket.tcp::socket::init(AF_INET) != 0)
        return -1;
    const auto budget = std::max(std::chrono::steady_clock::duration::zero(), deadline - std::chrono::steady_clock::now());
    if (!set_tls_blocking_io_timeout(socket, budget))
        return -1;
    const auto              native = socket.native_handle();
    const uri               target{"tcp://127.0.0.1:" + std::to_string(port)};
    std::mutex              mutex;
    std::condition_variable wake;
    bool                    done    = false;
    bool                    expired = false;
    std::thread             watchdog([&] {
        std::unique_lock lock{mutex};
        if (wake.wait_until(lock, deadline, [&] { return done; }))
            return;
        expired = true;
        do {
            lock.unlock();
            (void) ::shutdown(native, QB_SD_BOTH);
            lock.lock();
        } while (!wake.wait_for(lock, std::chrono::milliseconds{1}, [&] { return done; }));
    });
    // This overload bounds TCP; the watchdog also bounds its blocking TLS phase.
    const int result = socket.connect(target, budget);
    {
        std::lock_guard lock{mutex};
        done = true;
    }
    wake.notify_one();
    watchdog.join();
    clear_tls_blocking_io_timeout(socket);
    return expired || std::chrono::steady_clock::now() >= deadline ? -1 : result;
}

} // namespace qb::io::test

#endif // QB_IO_TESTS_SHARED_TLS_DEADLINE_H
