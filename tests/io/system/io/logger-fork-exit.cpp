/**
 * @file system/io/logger-fork-exit.cpp
 * @brief A child of `fork()` that calls `exit()` exits promptly, whether or not it logs, and the parent's logger still
 *        writes every line of its own (Huly QB-1246).
 *
 * `fork()` copies one thread: the calling one. The logger the child inherits is published with a writer thread that
 * exists only in the parent, and at the child's `exit()` the logger was stopped the way it is in the parent -- its
 * writer joined. A join of a thread the process does not have is undefined: libc++ on macOS refuses it and the child
 * dies of std::terminate (measured, signal 6); other C libraries return or wait. The child now leaves a logger of its
 * parent alone: no join, no drain, no flush of the parent's buffered bytes, and no reuse by a later `init`. Where the
 * C library's join returns, the base fails these cases only by log_file_opened()'s answer (exit 5); the death test of
 * qb-io-test-unit-guaranteed-logger-buffer catches the stop itself, on every C library.
 *
 * The parent's file is read as soon as the child is gone, before the parent writes into it again: the parent's stream
 * writes at the offset it remembers, so a line the child appended would be overwritten by the parent's next one.
 *
 * Each case runs the child under a budget and kills it when it is spent, so a hang fails with a message instead of
 * holding the binary until the ctest TIMEOUT. The parent waits for its writer to be idle before it forks: a fork that
 * meets the writer inside a line can hang in `fork()` itself under ThreadSanitizer on macOS (Huly QB-1193).
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

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>

#if !defined(_WIN32)
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <gtest/gtest.h>
#include <qb/io.h>

namespace logger_fork_exit_test {

#if defined(QB_WITH_LOGGING) && !defined(_WIN32)

constexpr int lines_per_phase = 200;

// A unique, empty directory per test, removed afterwards -- after the logger is sent back to LogInitializer's file.
class LogDir {
public:
    LogDir() {
        auto const *info = ::testing::UnitTest::GetInstance()->current_test_info();
        _path            = std::filesystem::temp_directory_path()
                           / (std::string("qb-logger-fork-exit-") + info->name() + "-" + std::to_string(std::random_device{}()));
        std::error_code ec;
        std::filesystem::remove_all(_path, ec);
        std::filesystem::create_directories(_path);
    }
    ~LogDir() {
        qb::io::log::init("./qb", 512); // LogInitializer's own file and roll size, as the other tests leave it
        std::error_code ec;
        std::filesystem::remove_all(_path, ec);
    }
    std::filesystem::path const &
    path() const noexcept {
        return _path;
    }

private:
    std::filesystem::path _path;
};

std::string
read_file(std::filesystem::path const &path) {
    std::ifstream      in(path, std::ios::binary);
    std::ostringstream content;
    content << in.rdbuf();
    return content.str();
}

// How many lines of `text` contain `needle`.
int
count_lines(std::string const &text, std::string const &needle) {
    std::istringstream in(text);
    int                found = 0;
    for (std::string line; std::getline(in, line);)
        found += line.find(needle) != std::string::npos;
    return found;
}

// How many lines of `text` end with `needle`.
int
count_line_endings(std::string const &text, std::string const &needle) {
    std::istringstream in(text);
    int                found = 0;
    for (std::string line; std::getline(in, line);)
        found += line.size() >= needle.size() && line.compare(line.size() - needle.size(), needle.size(), needle) == 0;
    return found;
}

// Logs `count` lines `<tag> <n>`, then waits until the last one is in `file`: the writer is idle when this returns.
void
log_and_wait_written(std::filesystem::path const &file, char const *tag, int count) {
    for (int n = 0; n < count; ++n)
        QB_LOG_CRIT(tag << ' ' << n);
    std::string const last = std::string(tag) + ' ' + std::to_string(count - 1);
    for (int i = 0; i < 5000 && read_file(file).find(last) == std::string::npos; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_NE(read_file(file).find(last), std::string::npos) << "the parent's writer never wrote " << last;
}

// The child's exit status, or -1 when it was still running after `budget` (then killed).
int
wait_or_kill(pid_t child, std::chrono::milliseconds budget) {
    auto const deadline = std::chrono::steady_clock::now() + budget;
    int        status   = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        pid_t const done = ::waitpid(child, &status, WNOHANG);
        if (done == child)
            return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        if (done < 0 && errno != EINTR)
            return -2;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ::kill(child, SIGKILL);
    ::waitpid(child, &status, 0);
    return -1;
}

enum class Child { exits, logs_then_exits, initializes_logs_then_exits };

// Forks; the child does what `child` says, then calls exit() -- which runs the static destructors: the logger is stopped
// there. Returns the child's status (wait_or_kill): 0, or 5 when the child was told its inherited logger writes its
// file, 6 when it was told its own does not, 7 when something threw in it.
int
fork_and_exit(Child child, std::filesystem::path const &child_log) {
    pid_t const pid = ::fork();
    if (pid == 0) {
        int code = 0;
        try {
            if (nanolog::log_file_opened())
                code = 5; // the inherited logger writes nothing here: every line the child logs into it is lost
            if (child == Child::initializes_logs_then_exits) {
                QB_LOG_CRIT("logged into the inherited logger"); // stays in the parent's queue: never written
                qb::io::log::init(child_log.string() + "-a", 1);
                if (code == 0 && !nanolog::log_file_opened())
                    code = 6; // the child's own logger opened its file
                QB_LOG_CRIT("from the child");
                qb::io::log::init(child_log.string() + "-b", 1); // the child's own logger is drained into child-a
                qb::io::log::init(child_log.string() + "-c", 1); // takes the child's own back, never the inherited one
            } else if (child == Child::logs_then_exits)
                QB_LOG_CRIT("from the child");
        } catch (...) {
            std::_Exit(7); // never back into gtest, which would run the remaining tests in the child
        }
        std::exit(code);
    }
    return pid < 0 ? -3 : wait_or_kill(pid, std::chrono::seconds(10));
}

// The parent's file holds each of its lines exactly once, and nothing of the child's.
void
expect_parent_file_complete(std::filesystem::path const &dir) {
    qb::io::log::init((dir / "after").string(), 1); // stops the parent's logger: its writer joined, its file closed
    std::string const text = read_file(dir / "parent.1.log");
    for (char const *tag : {"before", "after"})
        for (int n = 0; n < lines_per_phase; ++n)
            ASSERT_EQ(count_line_endings(text, std::string("] ") + tag + ' ' + std::to_string(n)), 1)
                << tag << ' ' << n << " is not in the parent's file exactly once";
    EXPECT_EQ(count_lines(text, "from the child"), 0) << "the child wrote into its parent's logger";
}

void
run_case(Child child) {
    LogDir const dir;
    qb::io::log::init((dir.path() / "parent").string(), 1);
    log_and_wait_written(dir.path() / "parent.1.log", "before", lines_per_phase);
    if (::testing::Test::HasFatalFailure())
        return;
    EXPECT_EQ(fork_and_exit(child, dir.path() / "child"), 0)
        << "the child did not exit cleanly (-1: hung, killed; -2, -3: waitpid, fork failed; 5, 6: log_file_opened() "
           "answered wrong; 7: a throw; 128 + n: signal n -- 134 is std::terminate)";
    // Read before the parent writes again: its next line is written at the offset its stream remembers, over a line
    // the child would have appended to the file they share.
    std::string const at_exit = read_file(dir.path() / "parent.1.log");
    EXPECT_EQ(count_lines(at_exit, "from the child") + count_lines(at_exit, "logged into the inherited logger"), 0)
        << "the child wrote into its parent's file";
    log_and_wait_written(dir.path() / "parent.1.log", "after", lines_per_phase);
    if (::testing::Test::HasFatalFailure())
        return;
    expect_parent_file_complete(dir.path());
    if (child == Child::initializes_logs_then_exits) {
        std::string const files[] = {
            read_file(dir.path() / "child-a.1.log"), read_file(dir.path() / "child-b.1.log"), read_file(dir.path() / "child-c.1.log")
        };
        EXPECT_EQ(count_lines(files[0], "from the child"), 1) << "the child's own logger did not write its line";
        for (std::string const &text : files)
            EXPECT_EQ(count_lines(text, "logged into the inherited logger"), 0)
                << "the child took its parent's logger back and wrote what the child had queued in it";
        EXPECT_EQ(count_lines(read_file(dir.path() / "parent.1.log"), "logged into the inherited logger"), 0);
    }
}

/**
 * @test A child that only calls exit() exits promptly.
 * @brief At exit the child stopped the logger it inherited: it joined the parent's writer thread, which the child does
 *        not have -- on macOS the join failed and the child ended in std::terminate.
 */
TEST(LoggerForkExit, AChildThatExitsDoesNotWaitForItsParentsWriter) {
    run_case(Child::exits);
}

/**
 * @test A child that logs, then calls exit(), exits promptly; its line goes nowhere, the parent's file is untouched.
 */
TEST(LoggerForkExit, AChildThatLogsThenExitsDoesNotWaitForItsParentsWriter) {
    run_case(Child::logs_then_exits);
}

/**
 * @test A child that calls `qb::io::log::init()` gets a logger of its own, which writes its line before it exits.
 * @brief init() stopped the logger it replaced -- the inherited one -- by joining the parent's writer: the child died in
 *        init() itself on macOS. A logger of the parent is never taken back by the child either: its queue and its writer's
 *        state are the parent's, copied at the fork. The child's second init would take back the inherited logger,
 *        then the only stopped one not published, if the child did, and the third would write what the child queued in it.
 */
TEST(LoggerForkExit, AChildThatInitializesGetsALoggerOfItsOwn) {
#if defined(__SANITIZE_THREAD__)
    GTEST_SKIP() << "ThreadSanitizer kills a forked child that starts a thread (die_after_fork)";
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
    GTEST_SKIP() << "ThreadSanitizer kills a forked child that starts a thread (die_after_fork)";
#endif
#endif
    run_case(Child::initializes_logs_then_exits);
}

#else

TEST(LoggerForkExit, NeedsLoggingAndFork) {
    GTEST_SKIP() << "QB_WITH_LOGGING is off, or this platform has no fork()";
}

#endif

} // namespace logger_fork_exit_test
