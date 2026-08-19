/**
 * @file system/async/kernel-events.cpp
 * @brief Kernel-event dispatch through the qb-io libev listener — high-level and raw watchers.
 *
 * These are SYSTEM tests for the lowest layer of the async stack: how `qb::io::async::listener`
 * surfaces kernel events to a handler. Two registration styles are exercised against a real (but
 * socket-free) event loop:
 *
 *   - the high-level `listener::registerEvent<event::{signal,timer,file,io}>` path, dispatched onto a
 *     plain `FakeActor::on(...)` overload set (signal delivery, repeating timer, `ev_stat` file-size
 *     notification, and a readable-fd `ev_io` wakeup);
 *   - the RAW libev watcher path used directly by framework internals — a self-managed `ev::timer`
 *     repeating watcher and a self-managed `ev::stat` file watcher attached straight to
 *     `listener::current.loop()` (absorbed from the dissolved system/test-async-io.cpp PeriodicTimer
 *     and FileWatcherFunctionality cases, which belong with the other kernel-watcher tests rather than
 *     in the timer/callback wrapper suites).
 *
 * The raw-watcher cases are de-flaked: their open-ended `for(i){run(EVRUN_ONCE)}` polls become bounded
 * deadline pumps so a dead watcher fails loudly instead of passing on a smoke `GE` count; the periodic
 * timer asserts it fired several times AND that it stops firing once stopped.
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
 * @ingroup Tests
 */

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <qb/io/async/event/all.h>
#include <qb/io/async/listener.h>
#include <qb/io/system/file.h>
#include <qb/utility/build_macros.h>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <process.h> // _getpid
#else
#include <unistd.h> // ::getpid
#endif

#include "../../shared/coroutine_test_support.h"

namespace {

// Process id, portably. Uniqueness ACROSS processes is the point: two ctest presets can have the
// same binary in flight at once, and a pid is the only qualifier here that is guaranteed to differ
// between them. (Sibling suites key on gtest's random_seed() instead — see io/unit/file/file-sys.cpp
// — which is fine within one process but is NOT load-bearing for cross-process isolation, since two
// runs are only as distinct as their seeds happen to be.)
[[nodiscard]] inline unsigned long long
current_pid() noexcept {
#if defined(_WIN32)
    return static_cast<unsigned long long>(::_getpid());
#else
    return static_cast<unsigned long long>(::getpid());
#endif
}

// Every file these cases touch lives in a private directory under the OS temp area, qualified by pid
// AND the running test's name — never the CWD.
//
// This suite used to write `./test.file`, `./test.file.tmp` and `./raw_file_watcher.tmp` relative to
// wherever the binary happened to start. Under ctest that is the shared `bin/tests` directory, so
// every one of those names was a cross-binary collision waiting to happen, and running the binary by
// hand dropped the files into the user's working directory. It has already cost one segfault:
// `KernelEvents.BasicIO` used to read the `test.file` that `KernelEvents.File` happened to leave in
// the CWD, so whenever it ran ALONE the file was absent, `sys::file` reported the failed open as
// `native_handle() == -1`, and `ev_io_start` indexed its `anfds` array with that negative fd (see
// the comment in that case). Both now create what they read, where nothing else can see it.
//
// The directory is created on first use per test and removed by ScopedTempDir's destructor.
class ScopedTempDir {
public:
    ScopedTempDir() {
        const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
        _dir             = std::filesystem::temp_directory_path()
                           / ("qb-kernel-events-" + std::to_string(current_pid()) + "-" + (info ? info->name() : "anon"));
        std::error_code ec;
        std::filesystem::remove_all(_dir, ec); // start clean
        std::filesystem::create_directories(_dir, ec);
    }

    ScopedTempDir(const ScopedTempDir &)            = delete;
    ScopedTempDir &operator=(const ScopedTempDir &) = delete;

    ~ScopedTempDir() {
        std::error_code ec;
        std::filesystem::remove_all(_dir, ec);
    }

    // Absolute path of `leaf` inside this test's private directory.
    [[nodiscard]] std::string
    operator()(std::string_view leaf) const {
        return (_dir / leaf).string();
    }

private:
    std::filesystem::path _dir;
};

} // namespace

struct FakeActor {
    int           nb_events          = 0;
    int           fd_test            = 0;
    std::intmax_t expected_file_size = -1;
    std::intmax_t observed_file_size = -1;

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
        observed_file_size = static_cast<std::intmax_t>(event.attr.st_size);
        if (expected_file_size >= 0 && observed_file_size != expected_file_size)
            return;
#ifdef _WIN32
        EXPECT_EQ(observed_file_size, 7);
#else
        EXPECT_EQ(observed_file_size, 5);
#endif // _WIN32
        ++nb_events;
    }

    void
    on(qb::io::async::event::timer const &) {
        ++nb_events;
    }
};

// FakeActor is NOT an async::base, so it does not auto-unregister its kernel events the way a real
// actor's ~base does. listener::clear() only DETACHES non-owned wrappers (it relies on the owner to
// free them), so a raw registerEvent here leaks its RegisteredKernelEvent wrapper. This guard
// restores the owner-frees contract for the harness: it captures the wrapper (event._interface, set
// by registerEvent) and unregisters + frees it when the registration leaves scope.
struct ScopedEvent {
    qb::io::async::listener               &loop;
    qb::io::async::IRegisteredKernelEvent *wrapper;
    ScopedEvent(qb::io::async::listener &l, qb::io::async::IRegisteredKernelEvent *w) noexcept
        : loop(l)
        , wrapper(w) {}
    ScopedEvent(const ScopedEvent &)            = delete;
    ScopedEvent &operator=(const ScopedEvent &) = delete;
    ~ScopedEvent() {
        loop.unregisterEvent(wrapper);
    }
};

TEST(KernelEvents, Signal) {
    qb::io::async::init();
    qb::io::async::listener handler;
    FakeActor               actor;

    auto       &ev = handler.registerEvent<qb::io::async::event::signal<SIGINT>>(actor);
    ScopedEvent guard{handler, ev._interface};
    ev.start();

#ifdef _WIN32
    // On Windows, CRT signal() is per-thread: std::raise() from a background thread
    // is never delivered to libev's ev_sig watcher registered on this (main) thread.
    // Raise from the same thread as the event loop so the signal handler fires here.
    std::raise(SIGINT);
    for (auto i = 0; i < 10 && !actor.nb_events; ++i)
        handler.run(EVRUN_ONCE);
#else
    std::thread t([]() { std::raise(SIGINT); });
    for (auto i = 0; i < 10 && !actor.nb_events; ++i)
        handler.run(EVRUN_ONCE);
    t.join();
#endif
    EXPECT_EQ(actor.nb_events, 1);
}

TEST(KernelEvents, Timer) {
    qb::io::async::listener handler;
    FakeActor               actor;

    auto       &ev = handler.registerEvent<qb::io::async::event::timer>(actor, 1, 1);
    ScopedEvent guard{handler, ev._interface};
    ev.start();

    for (auto i = 0; i < 10 && actor.nb_events < 2; ++i)
        handler.run(EVRUN_ONCE);
    EXPECT_EQ(actor.nb_events, 2);
}

TEST(KernelEvents, File) {
    const ScopedTempDir tmp;
    const std::string   file     = tmp("test.file");
    const std::string   file_tmp = tmp("test.file.tmp");

#ifndef _WIN32
    {
        std::ofstream ofs(file, std::ios::binary);
        ofs << "old\n";
    }
#endif

    qb::io::async::listener handler;
    FakeActor               actor;
#ifdef _WIN32
    actor.expected_file_size = 7;
#else
    actor.expected_file_size = 5;
#endif

#if QB_PLATFORM_MACOS
    constexpr double kFileEventInterval = 0.1;
#else
    constexpr int kFileEventInterval = 0;
#endif

    auto       &ev = handler.registerEvent<qb::io::async::event::file>(actor, file.c_str(), kFileEventInterval);
    ScopedEvent guard{handler, ev._interface};
    ev.start();

    std::thread t([&file, &file_tmp]() {
#ifndef _WIN32
        // Write atomically: produce the full content in a temporary file,
        // close it (flushing all OS buffers), then rename it into place.
        // rename(2) is atomic on POSIX — the stat watcher will see the file
        // appear with its final size in a single notification, eliminating
        // the race where ev_stat fires on creation before the write completes
        // (which would give st_size == 0 instead of the expected 5).
        {
            std::ofstream ofs(file_tmp, std::ios::binary);
            ofs << "test\n"; // exactly 5 bytes — matches EXPECT_EQ below
        }
        EXPECT_EQ(std::rename(file_tmp.c_str(), file.c_str()), 0);
#else
        // On Windows CMD, "echo test > file" produces "test \r\n" = 7 bytes (the space before the
        // redirect is echoed). The path is quoted because temp_directory_path() may contain spaces.
        (void) file_tmp;
        const std::string cmd = "echo test > \"" + file + "\"";
        EXPECT_EQ(system(cmd.c_str()), 0);
#endif
    });

#ifndef _WIN32
#if QB_PLATFORM_MACOS
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
#else
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
#endif
    while (std::chrono::steady_clock::now() < deadline && !actor.nb_events) {
        handler.run(EVRUN_NOWAIT);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
#else
    for (auto i = 0; i < 10 && !actor.nb_events; ++i)
        handler.run(EVRUN_ONCE);
#endif
    EXPECT_EQ(actor.nb_events, 1);
    EXPECT_EQ(actor.observed_file_size, actor.expected_file_size);
    t.join();
    // ScopedTempDir removes the whole directory — nothing is left anywhere, CWD included.
}

#ifndef _WIN32

TEST(KernelEvents, BasicIO) {
    // Self-sufficient: create the file this case reads instead of inheriting the one
    // `KernelEvents.File` happens to leave in the CWD. Relying on that ordering made this
    // case SEGFAULT whenever it ran alone (`--gtest_filter`, `--gtest_shuffle`, a sharded
    // run, or simply a clean working directory): `sys::file` reports a failed open as
    // `native_handle() == -1`, and `ev_io_start` indexes its `anfds` array by fd, so a
    // negative fd is an out-of-bounds access. libev guards it with
    // `EV_ASSERT_MSG(fd >= 0, ...)` (`src/qb/ev/qev.c`), but assertions are compiled out
    // under NDEBUG — so Debug asserted while Release crashed with EXC_BAD_ACCESS.
    const ScopedTempDir tmp;
    const std::string   file = tmp("test.file");
    {
        std::ofstream seed(file, std::ios::binary);
        seed << "test\n";
    }

    qb::io::async::listener handler;
    qb::io::sys::file       f(file);
    ASSERT_TRUE(f.is_open()) << "cannot open " << file
                             << " — registering an io watcher on a "
                                "negative fd is undefined behaviour, not a test failure";
    FakeActor actor;

    actor.fd_test = f.native_handle();

    auto       &ev = handler.registerEvent<qb::io::async::event::io>(actor, f.native_handle(), EV_READ);
    ScopedEvent guard{handler, ev._interface};
    ev.start();

    for (auto i = 0; i < 10 && !actor.nb_events; ++i)
        handler.run(EVRUN_ONCE);
    EXPECT_EQ(actor.nb_events, 1);

    f.close();
}

#endif

// ===========================================================================
// Raw libev watchers (absorbed from the dissolved test-async-io.cpp).
//
// These attach a self-managed ev::* watcher directly to the thread-local
// listener loop — the path framework internals use — rather than going through
// listener::registerEvent. They run on listener::current, so they init() it.
// ===========================================================================

namespace {

// A self-managed repeating ev::timer that counts every fire.
class RawPeriodicTimer {
public:
    std::atomic<int> count{0};

    explicit RawPeriodicTimer(double interval) {
        _watcher = new ev::timer(qb::io::async::listener::current.loop());
        _watcher->set<RawPeriodicTimer, &RawPeriodicTimer::on_timer>(this);
        _watcher->start(0.0, interval); // immediate first fire, then `interval` repeat
    }

    ~RawPeriodicTimer() {
        if (_watcher) {
            _watcher->stop();
            delete _watcher;
        }
    }

    void
    stop() noexcept {
        if (_watcher)
            _watcher->stop();
    }

    void
    on_timer(ev::timer &, int) {
        count.fetch_add(1);
    }

private:
    ev::timer *_watcher = nullptr;
};

// A self-managed ev::stat watcher that flips a flag when the watched file changes.
class RawFileWatcher {
public:
    std::atomic<bool> changed{false};

    explicit RawFileWatcher(std::string path)
        : _path(std::move(path)) {
        _watcher = new ev::stat(qb::io::async::listener::current.loop());
        _watcher->set<RawFileWatcher, &RawFileWatcher::on_change>(this);
        // Poll explicitly instead of taking libev's default. `ev_stat_set(..., 0.)` means
        // DEF_STAT_INTERVAL = 5.0074 s (ev.c), and with no inotify on macOS that default IS the
        // detection latency — this case spent 5.0 s of the suite waiting out one poll tick. 0.1 is
        // clamped up to MIN_STAT_INTERVAL (0.1075 s), which is what the watcher actually polls at.
        // The contract under test is unchanged: ev::stat still has to notice the rename.
        _watcher->set(_path.c_str(), 0.1);
        _watcher->start();
    }

    ~RawFileWatcher() {
        if (_watcher) {
            _watcher->stop();
            delete _watcher;
        }
    }

    void
    on_change(ev::stat &, int) {
        changed.store(true);
    }

private:
    std::string _path;
    ev::stat   *_watcher = nullptr;
};

} // namespace

// A raw repeating ev::timer fires multiple times, then stops firing once stopped.
TEST(KernelEvents, RawPeriodicTimerFiresThenStops) {
    qb::io::async::init();

    RawPeriodicTimer timer(0.02); // 20ms period

    // It must fire several times within a bounded budget.
    EXPECT_TRUE(qb::io::test::pump_until([&] { return timer.count.load() >= 3; })) << "raw ev::timer did not fire repeatedly";

    timer.stop();
    const int frozen = timer.count.load();

    // Once stopped, the count must not advance further.
    EXPECT_FALSE(qb::io::test::pump_until([&] { return timer.count.load() > frozen; }, std::chrono::milliseconds(200)))
        << "raw ev::timer kept firing after stop()";

    qb::io::async::listener::current.clear();
}

#ifndef _WIN32
// A raw ev::stat watcher detects an mtime/size change on its watched file.
TEST(KernelEvents, RawFileWatcherDetectsChange) {
    qb::io::async::init();

    const ScopedTempDir tmp;
    const std::string   path = tmp("raw_file_watcher.tmp");
    {
        std::ofstream ofs(path, std::ios::binary);
        ofs << "initial";
    }

    RawFileWatcher watcher(path);

    // Settle the watcher; it must not have fired on its own yet.
    qb::io::async::run_for(std::chrono::milliseconds(20));
    EXPECT_FALSE(watcher.changed.load());

    // ev_stat polls mtime, whose granularity is coarse — wait past one tick, then
    // rewrite atomically (write temp + rename) so the watcher sees a single change.
    std::this_thread::sleep_for(std::chrono::seconds(1));
    {
        std::ofstream ofs(path + ".tmp", std::ios::binary);
        ofs << "modified content";
    }
    ASSERT_EQ(std::rename((path + ".tmp").c_str(), path.c_str()), 0);

    EXPECT_TRUE(qb::io::test::pump_until([&] { return watcher.changed.load(); }, std::chrono::seconds(5)))
        << "raw ev::stat watcher never detected the file change";

    qb::io::async::listener::current.clear();
}
#endif
