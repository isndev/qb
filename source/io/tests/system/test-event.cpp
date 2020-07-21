/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2020 isndev (www.qbaf.io). All rights reserved.
 *
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
 *         limitations under the License.
 */

#include <chrono>
#include <csignal>
#include <gtest/gtest.h>
#include <qb/io/async/event/all.h>
#include <qb/io/async/listener.h>
#include <qb/io/system/file.h>
#include <thread>

struct FakeActor {
    int nb_events = 0;
    int fd_test = 0;

    bool
    is_alive() {
        return true;
    }

    void
    on(qb::io::async::event::signal<SIGINT> const &event) {
        EXPECT_EQ(SIGINT, event.signum);
        ++nb_events;
    }

    void
    on(qb::io::async::event::io &event) {
        EXPECT_EQ(fd_test, event.fd);
        EXPECT_EQ(true, event._revents & EV_READ);
        event.stop();
        ++nb_events;
    }

    void
    on(qb::io::async::event::file const &event) {
#ifdef _WIN32
        EXPECT_EQ(event.attr.st_size, 7);
#else
        EXPECT_EQ(event.attr.st_size, 5);
#endif // _WIN32
        ++nb_events;
    }

    void
    on(qb::io::async::event::timer const &) {
        ++nb_events;
    }
};

TEST(KernelEvents, Signal) {
    qb::io::async::init();
    qb::io::async::listener handler;
    FakeActor actor;

    handler.registerEvent<qb::io::async::event::signal<SIGINT>>(actor).start();

    std::thread t([]() { std::raise(SIGINT); });
    for (auto i = 0; i < 10 && !actor.nb_events; ++i)
        handler.run(EVRUN_ONCE);
    EXPECT_EQ(actor.nb_events, 1);
    t.join();
}

TEST(KernelEvents, Timer) {
    qb::io::async::listener handler;
    FakeActor actor;

    handler.registerEvent<qb::io::async::event::timer>(actor, 1, 1).start();

    for (auto i = 0; i < 10 && actor.nb_events < 2; ++i)
        handler.run(EVRUN_ONCE);
    EXPECT_EQ(actor.nb_events, 2);
}

TEST(KernelEvents, File) {
    qb::io::async::listener handler;
    FakeActor actor;

    handler.registerEvent<qb::io::async::event::file>(actor, "./test.file", 0).start();

    std::thread t([]() { system("echo test > test.file"); });

    for (auto i = 0; i < 10 && !actor.nb_events; ++i)
        handler.run(EVRUN_ONCE);
    EXPECT_EQ(actor.nb_events, 1);
    t.join();
}

#ifndef _WIN32

TEST(KernelEvents, BasicIO) {
    qb::io::async::listener handler;
    qb::io::sys::file f("test.file");
    FakeActor actor;

    actor.fd_test = f.fd();

    handler.registerEvent<qb::io::async::event::io>(actor, f.fd(), EV_READ).start();

    for (auto i = 0; i < 10 && !actor.nb_events; ++i)
        handler.run(EVRUN_ONCE);
    EXPECT_EQ(actor.nb_events, 1);
}

#endif