/**
 * @file qb/io/tests/system/test-event.cpp
 * @brief Unit tests for asynchronous event handling
 *
 * This file contains tests for the asynchronous event handling capabilities of the
 * QB framework, including signal events, timer events, file events, and I/O events.
 * It verifies that events are properly registered, triggered, and handled.
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
 * @ingroup Tests
 */

#include <chrono>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <gtest/gtest.h>
#include <qb/io/async/event/all.h>
#include <qb/io/async/listener.h>
#include <qb/io/system/file.h>
#include <thread>

struct FakeActor {
    int nb_events = 0;
    int fd_test   = 0;

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
    FakeActor               actor;

    handler.registerEvent<qb::io::async::event::signal<SIGINT>>(actor).start();

    std::thread t([]() { std::raise(SIGINT); });
    for (auto i = 0; i < 10 && !actor.nb_events; ++i)
        handler.run(EVRUN_ONCE);
    EXPECT_EQ(actor.nb_events, 1);
    t.join();
}

TEST(KernelEvents, Timer) {
    qb::io::async::listener handler;
    FakeActor               actor;

    handler.registerEvent<qb::io::async::event::timer>(actor, 1, 1).start();

    for (auto i = 0; i < 10 && actor.nb_events < 2; ++i)
        handler.run(EVRUN_ONCE);
    EXPECT_EQ(actor.nb_events, 2);
}

TEST(KernelEvents, File) {
    // Remove any leftover file from a previous run so the watcher can
    // detect a creation event (not a stale modification event).
    std::remove("./test.file");

    qb::io::async::listener handler;
    FakeActor               actor;

    handler.registerEvent<qb::io::async::event::file>(actor, "./test.file", 0).start();

    std::thread t([]() {
#ifndef _WIN32
        // Write atomically: produce the full content in a temporary file,
        // close it (flushing all OS buffers), then rename it into place.
        // rename(2) is atomic on POSIX — the stat watcher will see the file
        // appear with its final size in a single notification, eliminating
        // the race where ev_stat fires on creation before the write completes
        // (which would give st_size == 0 instead of the expected 5).
        {
            std::ofstream ofs("./test.file.tmp", std::ios::binary);
            ofs << "test\n"; // exactly 5 bytes — matches EXPECT_EQ below
        }
        EXPECT_EQ(std::rename("./test.file.tmp", "./test.file"), 0);
#else
        // On Windows CMD, "echo test > file" produces "test \r\n" = 7 bytes.
        EXPECT_EQ(system("echo test > test.file"), 0);
#endif
    });

    for (auto i = 0; i < 10 && !actor.nb_events; ++i)
        handler.run(EVRUN_ONCE);
    EXPECT_EQ(actor.nb_events, 1);
    t.join();
}

#ifndef _WIN32

TEST(KernelEvents, BasicIO) {
    qb::io::async::listener handler;
    qb::io::sys::file       f("test.file");
    FakeActor               actor;

    actor.fd_test = f.native_handle();

    handler.registerEvent<qb::io::async::event::io>(actor, f.native_handle(), EV_READ)
        .start();

    for (auto i = 0; i < 10 && !actor.nb_events; ++i)
        handler.run(EVRUN_ONCE);
    EXPECT_EQ(actor.nb_events, 1);
}

#endif