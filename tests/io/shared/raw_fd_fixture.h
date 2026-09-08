/**
 * @file qb/tests/io/shared/raw_fd_fixture.h
 * @brief The smallest fd the current loop can own, as a raw `ev_io` that counts its deliveries.
 *
 * A pipe on POSIX; on Windows a loopback TCP pair -- the one shape wepoll can watch (sockets
 * only), and the shape a core actually owns. AFD does not make a loopback byte readable
 * synchronously with the send, so `put()` waits (select, bounded) until the kernel reports the
 * reader readable BEFORE the pass being counted: what a case asserts is whether that pass LOOKED,
 * never whether the byte had arrived. Shared by the io-cadence (Huly QB-191) and the timer-gate
 * (QB-190) tests, so the two cannot disagree about what a delivery is.
 *
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 * Licensed under the Apache License, Version 2.0. See LICENSE for details.
 */

#ifndef QB_IO_TESTS_SHARED_RAW_FD_FIXTURE_H
#define QB_IO_TESTS_SHARED_RAW_FD_FIXTURE_H

#include <gtest/gtest.h>
#include <qb/io/async.h>

#ifdef _WIN32
#include <qb/io/tcp/listener.h>
#include <qb/io/tcp/socket.h>
#else
#include <unistd.h>
#endif

namespace qb::io::test {

using qb::io::async::listener;

#ifdef _WIN32
/// The Windows shape: a loopback TCP pair through wepoll (see the file header). The callback counts.
struct Pipe {
    qb::io::tcp::listener acceptor;
    qb::io::tcp::socket   writer;
    qb::io::tcp::socket   reader;
    ev_io                 w{};
    int                   hits = 0;
    Pipe() {
        EXPECT_EQ(acceptor.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
        EXPECT_EQ(writer.connect_v4("127.0.0.1", acceptor.local_endpoint().port()), qb::io::SocketStatus::Done);
        EXPECT_EQ(acceptor.accept(reader), qb::io::SocketStatus::Done);
        reader.set_nonblocking(true);
        ev_init(&w, &Pipe::cb);
        ev_io_set_sock(&w, static_cast<uintptr_t>(reader.native_handle()), EV_READ);
        w.data = this;
        ev_io_start(static_cast<struct ev_loop *>(listener::current.loop()), &w);
    }
    ~Pipe() {
        ev_io_stop(static_cast<struct ev_loop *>(listener::current.loop()), &w);
    }
    void
    put() const {
        char c = 'x';
        EXPECT_EQ(writer.write(&c, 1), 1);
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(reader.native_handle(), &rs);
        timeval tv{1, 0};
        EXPECT_EQ(::select(0, &rs, nullptr, nullptr, &tv), 1) << "the loopback byte is readable within a second";
    }
    static void
    cb(struct ev_loop *, ev_io *w, int) {
        auto *self = static_cast<Pipe *>(w->data);
        char  buf[8];
        (void) self->reader.read(buf, sizeof buf);
        ++self->hits;
    }
};
#else
/// A raw `ev_io` on a pipe: the smallest fd the loop can own. The callback counts.
struct Pipe {
    int   fds[2]{-1, -1};
    ev_io w{};
    int   hits = 0;
    Pipe() {
        EXPECT_EQ(::pipe(fds), 0);
        ev_io_init(&w, &Pipe::cb, fds[0], EV_READ);
        w.data = this;
        ev_io_start(static_cast<struct ev_loop *>(listener::current.loop()), &w);
    }
    ~Pipe() {
        ev_io_stop(static_cast<struct ev_loop *>(listener::current.loop()), &w);
        ::close(fds[0]);
        ::close(fds[1]);
    }
    void
    put() const {
        char c = 'x';
        EXPECT_EQ(::write(fds[1], &c, 1), 1);
    }
    static void
    cb(struct ev_loop *, ev_io *w, int) {
        auto *self = static_cast<Pipe *>(w->data);
        char  buf[8];
        (void) ::read(self->fds[0], buf, sizeof buf);
        ++self->hits;
    }
};
#endif

} // namespace qb::io::test

#endif // QB_IO_TESTS_SHARED_RAW_FD_FIXTURE_H
