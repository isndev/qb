/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
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
 * limitations under the License.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#include <gtest/gtest.h>

namespace guaranteed_logger_buffer_test {
enum Phase { before_publish, after_publish, destroyed, writer_draining, writer_wrote, stopping };
void hook(int phase, unsigned int index);

// Logs from its destructor once armed (Huly QB-1207). Defined BEFORE nanolog.cpp's globals below, so it is
// destroyed after them, at exit: what a static destructor of a program that logs looks like to the logger.
struct LogsAtExit {
    bool armed = false;
    ~LogsAtExit();
};
LogsAtExit logs_at_exit;
} // namespace guaranteed_logger_buffer_test

// Compile the private queue in this test translation unit so the pause lands exactly between
// publishing the final slot and the producer's next access to the old Buffer. The GuaranteedLoggerBuffer cases use no
// logger global, file writer or scheduling guess; the GuaranteedLoggerReplacement cases (Huly QB-1207) drive this copy's
// global logger, never qb-io's. The production build compiles these hooks away.
#define NANOLOG_TEST_HOOK(phase, index) ::guaranteed_logger_buffer_test::hook(::guaranteed_logger_buffer_test::phase, index)
#include <qb/vendor/nanolog/nanolog.cpp>

namespace guaranteed_logger_buffer_test {
// Every nanolog::Buffer destroyed so far, gate or no gate (the `destroyed` hook).
std::atomic<int> buffers_destroyed{0};

// The fork test's parent (Huly QB-1246): a child of it that reaches `stopping` stops a logger of its parent the way
// the parent does -- before the join of a thread the child does not have. Exits 4 there, on every C library.
std::atomic<long> fork_test_parent{0};

// The exit test's hold (Huly QB-1207): armed, the writer waits after its first write until a logger begins to stop.
// Atomics only, so trivially destructible: defined after nanolog.cpp's statics, they are destroyed before StopAtExit
// runs, which still releases through them.
std::atomic<bool> hold_writer{false};
std::atomic<bool> writer_released{false};

LogsAtExit::~LogsAtExit() {
    if (!armed)
        return;
    if (buffers_destroyed.load(std::memory_order_acquire) != 0)
        std::_Exit(3); // the logger was destroyed at exit: what follows would write into it, in any build
    NANO_LOG(nanolog::LogLevel::INFO) << "logged by a static destructor after the logger stopped";
}

struct Gate {
    std::mutex              mutex;
    std::condition_variable cv;
    Phase                   pause_phase = after_publish;
    unsigned int            pause_index = nanolog::Buffer::size - 1;
    bool                    paused      = false;
    bool                    released    = false;
    bool                    marked_only = false; // pause the thread that marked itself, at any index (Huly QB-1207)
    std::atomic<int>        destroyed_count{0};
};

std::atomic<Gate *> active_gate{nullptr};
thread_local bool   marked_thread = false;

void
hook(int phase, unsigned int index) {
#if !defined(_WIN32)
    if (phase == stopping && fork_test_parent.load(std::memory_order_relaxed) != 0
        && fork_test_parent.load(std::memory_order_relaxed) != static_cast<long>(::getpid()))
        std::_Exit(4);
#endif
    if (phase == stopping || phase == writer_wrote) {
        if (!hold_writer.load(std::memory_order_acquire))
            return;
        if (phase == stopping)
            writer_released.store(true, std::memory_order_release);
        while (!writer_released.load(std::memory_order_acquire))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return;
    }
    if (phase == destroyed)
        buffers_destroyed.fetch_add(1, std::memory_order_release);
    Gate *gate = active_gate.load(std::memory_order_acquire);
    if (gate == nullptr)
        return;
    if (phase == destroyed) {
        gate->destroyed_count.fetch_add(1, std::memory_order_release);
        return;
    }
    if (phase != gate->pause_phase || (gate->marked_only ? !marked_thread : index != gate->pause_index))
        return;
    std::unique_lock lock(gate->mutex);
    gate->paused = true;
    gate->cv.notify_one();
    gate->cv.wait(lock, [&] { return gate->released; });
}

nanolog::NanoLogLine
line(std::uint64_t sequence = 0) {
    nanolog::NanoLogLine result{nanolog::LogLevel::INFO, "guaranteed-logger-buffer.cpp", "line", 0};
    result << sequence;
    return result;
}

std::uint64_t
sequence_of(nanolog::NanoLogLine &record) {
    std::ostringstream output;
    record.stringify(output);
    std::string const text = output.str();
    return std::stoull(text.substr(text.rfind("] ") + 2));
}

TEST(GuaranteedLoggerBuffer, LastRecordCannotReachFreedCompletionCounter) {
    nanolog::QueueBuffer queue;
    nanolog::NanoLogLine out = line();

    // Leave only the final record in the first Buffer. Its consumer can immediately retire it.
    for (std::size_t i = 0; i < nanolog::Buffer::size - 1; ++i) {
        queue.push(line());
        ASSERT_TRUE(queue.try_pop(out));
    }

    Gate gate;
    active_gate.store(&gate, std::memory_order_release);
    std::thread producer([&] { queue.push(line()); });

    {
        std::unique_lock lock(gate.mutex);
        gate.cv.wait(lock, [&] { return gate.paused; });
    }
    EXPECT_TRUE(queue.try_pop(out));
    EXPECT_EQ(gate.destroyed_count.load(std::memory_order_acquire), 1);

    {
        std::lock_guard lock(gate.mutex);
        gate.released = true;
    }
    gate.cv.notify_one();
    producer.join();
    active_gate.store(nullptr, std::memory_order_release);
}

TEST(GuaranteedLoggerBuffer, RolloverPreservesRecordsInOrder) {
    nanolog::QueueBuffer  queue;
    constexpr std::size_t count = nanolog::Buffer::size * 2 + 17;
    for (std::size_t i = 0; i < count; ++i)
        queue.push(line(i));

    nanolog::NanoLogLine out = line();
    for (std::size_t i = 0; i < count; ++i) {
        ASSERT_TRUE(queue.try_pop(out)) << "missing record " << i;
        ASSERT_EQ(sequence_of(out), i);
    }
    EXPECT_FALSE(queue.try_pop(out));
}

TEST(GuaranteedLoggerBuffer, EarlierSlowProducerBlocksRetirement) {
    nanolog::QueueBuffer queue;
    Gate                 gate;
    gate.pause_phase = before_publish;
    gate.pause_index = 0;
    active_gate.store(&gate, std::memory_order_release);
    std::thread earlier([&] { queue.push(line(0)); });

    {
        std::unique_lock lock(gate.mutex);
        gate.cv.wait(lock, [&] { return gate.paused; });
    }
    for (std::size_t i = 1; i < nanolog::Buffer::size; ++i)
        queue.push(line(i));

    nanolog::NanoLogLine out = line();
    EXPECT_FALSE(queue.try_pop(out));
    EXPECT_EQ(gate.destroyed_count.load(std::memory_order_acquire), 0);

    {
        std::lock_guard lock(gate.mutex);
        gate.released = true;
    }
    gate.cv.notify_one();
    earlier.join();

    for (std::size_t i = 0; i < nanolog::Buffer::size; ++i) {
        ASSERT_TRUE(queue.try_pop(out));
        ASSERT_EQ(sequence_of(out), i);
    }
    EXPECT_EQ(gate.destroyed_count.load(std::memory_order_acquire), 1);
    active_gate.store(nullptr, std::memory_order_release);
}

TEST(GuaranteedLoggerBuffer, ConcurrentProducersCrossRolloverWithoutLoss) {
    nanolog::QueueBuffer  queue;
    constexpr std::size_t producers  = 4;
    constexpr std::size_t per_thread = nanolog::Buffer::size / 2 + 31;
    constexpr std::size_t total      = producers * per_thread;
    std::vector<bool>     seen(total, false);
    std::size_t           received = 0;
    std::atomic<bool>     start{false};

    std::thread consumer([&] {
        nanolog::NanoLogLine out      = line();
        auto const           deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (received < total && std::chrono::steady_clock::now() < deadline) {
            if (!queue.try_pop(out)) {
                std::this_thread::yield();
                continue;
            }
            auto const sequence = sequence_of(out);
            if (sequence >= total || seen[sequence]) {
                ADD_FAILURE() << "duplicate or invalid record " << sequence;
            } else {
                seen[sequence] = true;
            }
            ++received;
        }
    });

    std::vector<std::thread> writers;
    writers.reserve(producers);
    for (std::size_t t = 0; t < producers; ++t) {
        writers.emplace_back([&, t] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            for (std::size_t i = 0; i < per_thread; ++i)
                queue.push(line(t * per_thread + i));
        });
    }
    start.store(true, std::memory_order_release);
    for (auto &writer : writers)
        writer.join();
    consumer.join();

    ASSERT_EQ(received, total);
    for (std::size_t i = 0; i < total; ++i)
        ASSERT_TRUE(seen[i]) << "missing record " << i;
}

// =============================================================================
// THE LOGGER initialize() REPLACES (Huly QB-1207)
// =============================================================================
// initialize() destroyed the logger it replaced -- its writer thread, its queue and the queue's Buffers -- and only
// THEN published the new one. A thread that had read the old logger and was still inside its log statement wrote into
// the freed queue; and while the old one drained, every other thread kept pushing into it, so a thread logging faster
// than the writer drained kept initialize() from ever returning. These cases drive the global logger of this private
// copy of nanolog.cpp (no qb-io archive, no LogInitializer), through NANO_LOG as a program does.

// A unique, empty directory per test and per run, removed afterwards.
class LogDir {
public:
    LogDir() {
        const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
        _path            = std::filesystem::temp_directory_path()
                           / (std::string("qb-guaranteed-logger-") + info->name() + "-" + std::to_string(std::random_device{}()));
        std::error_code ec;
        std::filesystem::remove_all(_path, ec);
        std::filesystem::create_directories(_path);
    }
    ~LogDir() {
        // The published logger writes into this directory: point it elsewhere first, or its open file keeps the
        // directory from being removed on Windows. The sink is one file per machine, truncated each time.
        std::error_code ec;
        auto const      sink = std::filesystem::temp_directory_path() / "qb-guaranteed-logger-sink";
        std::filesystem::create_directories(sink, ec);
        nanolog::initialize(nanolog::GuaranteedLogger(), sink / "sink", 1);
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

// How many times each `line <n>`, n < count, was written to the files of `dir`; a line of another shape counts at count.
std::vector<int>
lines_written(std::filesystem::path const &dir, std::size_t const count) {
    std::vector<int> seen(count + 1, 0);
    for (auto const &entry : std::filesystem::directory_iterator(dir)) {
        std::istringstream text(read_file(entry.path()));
        for (std::string line; std::getline(text, line);) {
            auto const at = line.rfind("] line ");
            ++seen[at == std::string::npos ? count : std::min<std::size_t>(std::stoull(line.substr(at + 7)), count)];
        }
    }
    return seen;
}

/**
 * @test A thread inside its log statement when initialize() replaces the logger finishes into memory that is still
 *       there, and its line is not lost.
 * @brief The hook holds the thread in the replaced logger's Buffer, its slot filled and not yet published -- what a
 *        thread preempted inside NANO_LOG looks like. initialize() used to free that Buffer (`destroyed` fired) before
 *        it returned, and the thread then published into freed memory: AddressSanitizer reports the use after free.
 *        The held line is written by the next initialize() of the same kind, which takes the replaced logger back.
 */
TEST(GuaranteedLoggerReplacement, AThreadStillInsideTheReplacedLoggerNeverReachesFreedMemory) {
    LogDir const dir;
    nanolog::initialize(nanolog::GuaranteedLogger(), dir.path() / "first", 1);
    // Once it is in the file (every line is flushed as it is written), the writer has passed its slot: a Buffer that
    // slot retired is gone before the gate counts.
    NANO_LOG(nanolog::LogLevel::INFO) << "before the replacement";
    for (int i = 0; i < 10000 && read_file(dir.path() / "first.1.log").find("before the replacement") == std::string::npos; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_NE(read_file(dir.path() / "first.1.log").find("before the replacement"), std::string::npos) << "the writer never wrote";

    Gate gate;
    gate.pause_phase = before_publish; // its slot filled, not yet published
    gate.marked_only = true;
    active_gate.store(&gate, std::memory_order_release);
    std::thread straggler([] {
        marked_thread = true;
        NANO_LOG(nanolog::LogLevel::INFO) << "held inside the replaced logger";
    });
    {
        std::unique_lock lock(gate.mutex);
        gate.cv.wait(lock, [&] { return gate.paused; });
    }

    nanolog::initialize(nanolog::GuaranteedLogger(), dir.path() / "second", 1);
    EXPECT_EQ(gate.destroyed_count.load(std::memory_order_acquire), 0)
        << "initialize() freed the replaced logger's Buffer under a thread still writing into it";

    {
        std::lock_guard lock(gate.mutex);
        gate.released = true;
    }
    gate.cv.notify_one();
    straggler.join();
    active_gate.store(nullptr, std::memory_order_release);

    nanolog::initialize(nanolog::GuaranteedLogger(), dir.path() / "third", 1);  // takes the first logger back
    nanolog::initialize(nanolog::GuaranteedLogger(), dir.path() / "fourth", 1); // completes third.1.log
    EXPECT_NE(read_file(dir.path() / "third.1.log").find("held inside the replaced logger"), std::string::npos)
        << "the line of the thread held across the replacement was lost";
    EXPECT_EQ(read_file(dir.path() / "second.1.log"), "") << "nothing was logged while the second logger was published";
}

/**
 * @test A line logged while initialize() waits for the replaced logger to drain goes to the new logger.
 * @brief The hook holds the replaced logger's writer at the start of its last drain -- inside initialize(). The
 *        replaced logger used to stay published until it was destroyed, so every thread kept pushing into it while
 *        it drained: a thread that logged faster than the writer wrote kept initialize() from returning at all
 *        (measured on the base: one call took 2.2 s, and returned only when its producer stopped). The new logger is
 *        published first, so this line lands in it, and the replaced one drains only what it already held.
 */
TEST(GuaranteedLoggerReplacement, ALineLoggedWhileTheReplacedLoggerDrainsGoesToTheNewOne) {
    LogDir const dir;
    nanolog::initialize(nanolog::GuaranteedLogger(), dir.path() / "old", 1);

    Gate gate;
    gate.pause_phase = writer_draining;
    gate.pause_index = 0;
    active_gate.store(&gate, std::memory_order_release);
    std::thread replacer([&] { nanolog::initialize(nanolog::GuaranteedLogger(), dir.path() / "new", 1); });
    {
        std::unique_lock lock(gate.mutex);
        gate.cv.wait(lock, [&] { return gate.paused; });
    }
    NANO_LOG(nanolog::LogLevel::INFO) << "logged while the replaced logger drains";
    {
        std::lock_guard lock(gate.mutex);
        gate.released = true;
    }
    gate.cv.notify_one();
    replacer.join();
    active_gate.store(nullptr, std::memory_order_release);

    nanolog::initialize(nanolog::GuaranteedLogger(), dir.path() / "after", 1); // completes new.1.log
    EXPECT_NE(read_file(dir.path() / "new.1.log").find("logged while the replaced logger drains"), std::string::npos)
        << "a line logged during the replacement went to the logger being replaced";
    EXPECT_EQ(read_file(dir.path() / "old.1.log"), "");
}

/**
 * @test Replacing the logger again and again, from two threads, while a third logs, loses no line and writes none
 *       twice.
 * @brief The producer logs slower than the writer writes, so each replaced logger drains at once and the producer is
 *        still logging when the replaced logger used to be destroyed: its next line went into freed memory
 *        (AddressSanitizer, ThreadSanitizer). Two replacing threads also race initialize() against itself, which
 *        reset one std::unique_ptr from both. The count is exact: a line a producer left in a replaced logger is
 *        written when that logger is taken back, as the last two replacements do once nobody logs.
 */
TEST(GuaranteedLoggerReplacement, ReplacementsUnderLoggingLoseNoLine) {
    LogDir const dir;
    nanolog::initialize(nanolog::GuaranteedLogger(), dir.path() / "0", 1);

    std::atomic<bool>          stop{false};
    std::atomic<std::uint64_t> logged{0};
    std::thread                producer([&] {
        for (std::uint64_t n = 0; n < 100000 && !stop.load(std::memory_order_relaxed); ++n) {
            NANO_LOG(nanolog::LogLevel::INFO) << "line " << n;
            logged.fetch_add(1, std::memory_order_release);
            std::this_thread::sleep_for(std::chrono::microseconds(20)); // slower than the writer: it never lags
        }
    });
    while (logged.load(std::memory_order_acquire) == 0)
        std::this_thread::yield();

    std::atomic<int> next_file{1};
    auto const       replace = [&] {
        for (int i = 0; i < 8; ++i) {
            nanolog::initialize(nanolog::GuaranteedLogger(), dir.path() / std::to_string(next_file.fetch_add(1)), 1);
            std::this_thread::sleep_for(std::chrono::milliseconds(1)); // let the producer log into each one
        }
    };
    std::thread first(replace);
    std::thread second(replace);
    first.join();
    second.join();
    stop.store(true, std::memory_order_relaxed);
    producer.join();

    nanolog::initialize(nanolog::GuaranteedLogger(), dir.path() / "flush-a", 1);
    nanolog::initialize(nanolog::GuaranteedLogger(), dir.path() / "flush-b", 1);
    std::uint64_t const count = logged.load();
    auto const          seen  = lines_written(dir.path(), count);
    for (std::uint64_t n = 0; n < count; ++n)
        ASSERT_EQ(seen[n], 1) << "line " << n << " was written " << seen[n] << " times";
    EXPECT_EQ(seen[count], 0) << "lines that are none of the producer's";
}

/**
 * @test A logger initialize() takes back reports whether ITS file opened, not the file of its first life.
 * @brief log_file_opened() is how qb::io::log::init() knows to warn that every line is discarded (Huly QB-327); the
 *        third initialize() below takes back the first logger, whose own first file opened, for a path that cannot be
 *        opened -- a directory of that name.
 */
TEST(GuaranteedLoggerReplacement, ALoggerTakenBackReportsWhetherItsNewFileOpened) {
    LogDir const dir;
    nanolog::initialize(nanolog::GuaranteedLogger(), dir.path() / "a", 1);
    nanolog::initialize(nanolog::GuaranteedLogger(), dir.path() / "b", 1);
    EXPECT_TRUE(nanolog::log_file_opened());
    std::filesystem::create_directories(dir.path() / "blocked.1.log"); // a directory where the file would go
    nanolog::initialize(nanolog::GuaranteedLogger(), dir.path() / "blocked", 1);
    EXPECT_FALSE(nanolog::log_file_opened()) << "a logger taken back reported the file of its previous life";
    nanolog::initialize(nanolog::GuaranteedLogger(), dir.path() / "c", 1);
    EXPECT_TRUE(nanolog::log_file_opened());
}

/**
 * @test log_file_opened() answers, on a thread that called initialize(), for that thread's last call -- not for a call
 *       another thread made since.
 * @brief qb::io::log::init() opens, then asks: with two init calls racing, the answer was the other call's, so one init
 *        warned about the wrong file and the other's failure went unreported (the first thread's open failed, the
 *        second's succeeded, and the first was told its file opened).
 */
TEST(GuaranteedLoggerReplacement, LogFileOpenedAnswersForTheCallingThreadsLastInitialize) {
    LogDir const dir;
    std::filesystem::create_directories(dir.path() / "blocked.1.log"); // a directory where the file would go
    bool        reported = true;
    std::thread first([&] {
        nanolog::initialize(nanolog::GuaranteedLogger(), dir.path() / "blocked", 1); // cannot be opened
        std::thread second([&] { nanolog::initialize(nanolog::GuaranteedLogger(), dir.path() / "fine", 1); });
        second.join(); // another thread's initialize(), which opened its file, since
        reported = nanolog::log_file_opened();
    });
    first.join();
    EXPECT_FALSE(reported) << "log_file_opened() answered for another thread's initialize()";
}

/**
 * @test On a thread that never called initialize(), log_file_opened() answers for the published logger.
 * @brief The calling thread's own answer is a thread_local; any other thread reads the published logger, under the lock.
 */
TEST(GuaranteedLoggerReplacement, LogFileOpenedAnswersForThePublishedLoggerOnAnotherThread) {
    LogDir const dir;
    nanolog::initialize(nanolog::GuaranteedLogger(), dir.path() / "open", 1);
    bool answer = false;
    std::thread([&] { answer = nanolog::log_file_opened(); }).join();
    EXPECT_TRUE(answer) << "the published logger's file opened";
    std::filesystem::create_directories(dir.path() / "blocked.1.log"); // a directory where the file would go
    nanolog::initialize(nanolog::GuaranteedLogger(), dir.path() / "blocked", 1);
    std::thread([&] { answer = nanolog::log_file_opened(); }).join();
    EXPECT_FALSE(answer) << "another thread was told the published logger's file opened";
}

/**
 * @test log_file_opened() on a thread that never called initialize() races no initialize() (ThreadSanitizer).
 * @brief Such a thread reads the published logger's opened flag, which restart() writes when it takes a logger back --
 *        perhaps the one that thread read just before: the read is under initialize()'s lock, so ordered after it.
 */
TEST(GuaranteedLoggerReplacement, LogFileOpenedOnAnotherThreadRacesNoInitialize) {
    LogDir const dir;
    nanolog::initialize(nanolog::GuaranteedLogger(), dir.path() / "start", 1); // a logger whose file opened, alone too
    ASSERT_TRUE(nanolog::log_file_opened()) << "the setup's own file did not open";
    std::atomic<bool> done{false};
    std::atomic<int>  wrong{0};
    std::thread       reader([&] {
        do { // at least once, whatever the main thread has done by then
            if (!nanolog::log_file_opened())
                wrong.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        } while (!done.load(std::memory_order_acquire));
    });
    for (int i = 0; i < 100; ++i)
        nanolog::initialize(nanolog::GuaranteedLogger(), dir.path() / std::to_string(i), 1);
    done.store(true, std::memory_order_release);
    reader.join();
    EXPECT_EQ(wrong.load(), 0) << "the published logger's file opened every time";
}

/**
 * @test The stopped logger taken back first is the one stopped longest ago, so what a late thread left in it is written
 *       even while other initialize() calls alternate queues.
 * @brief A thread is held inside logger P when initialize() replaces it by Q; a ring logger then replaces Q. The next
 *        guaranteed initialize() must take P back -- stopped first -- and write the held line; taking the newest
 *        stopped logger (Q) instead would leave P, and its line, waiting for ever while calls alternate. The second
 *        round holds a line in Q, created after P: it rejects a choice by creation order, either way.
 */
TEST(GuaranteedLoggerReplacement, TheLoggerStoppedLongestAgoIsTakenBackFirst) {
    LogDir const dir;
    nanolog::initialize(nanolog::GuaranteedLogger(), dir.path() / "p", 1);
    Gate gate;
    gate.pause_phase = before_publish;
    gate.marked_only = true;
    active_gate.store(&gate, std::memory_order_release);
    std::thread straggler([] {
        marked_thread = true;
        NANO_LOG(nanolog::LogLevel::INFO) << "held inside P";
    });
    {
        std::unique_lock lock(gate.mutex);
        gate.cv.wait(lock, [&] { return gate.paused; });
    }
    nanolog::initialize(nanolog::GuaranteedLogger(), dir.path() / "q", 1); // P stopped, holding the line
    {
        std::lock_guard lock(gate.mutex);
        gate.released = true;
    }
    gate.cv.notify_one();
    straggler.join();
    active_gate.store(nullptr, std::memory_order_release);

    nanolog::initialize(nanolog::NonGuaranteedLogger(1), dir.path() / "ring", 1); // Q stopped, after P
    nanolog::initialize(nanolog::GuaranteedLogger(), dir.path() / "c", 1);        // takes P back
    nanolog::initialize(nanolog::GuaranteedLogger(), dir.path() / "d", 1);        // takes Q back; completes c.1.log
    EXPECT_NE(read_file(dir.path() / "c.1.log").find("held inside P"), std::string::npos)
        << "the newest stopped logger was taken back, and the line held in the older one is still waiting";

    Gate again;
    again.pause_phase = before_publish;
    again.marked_only = true;
    active_gate.store(&again, std::memory_order_release);
    std::thread late([] {
        marked_thread = true;
        NANO_LOG(nanolog::LogLevel::INFO) << "held inside Q";
    });
    {
        std::unique_lock lock(again.mutex);
        again.cv.wait(lock, [&] { return again.paused; });
    }
    nanolog::initialize(nanolog::GuaranteedLogger(), dir.path() / "e", 1); // takes P back; Q stopped, holding the line
    {
        std::lock_guard lock(again.mutex);
        again.released = true;
    }
    again.cv.notify_one();
    late.join();
    active_gate.store(nullptr, std::memory_order_release);

    nanolog::initialize(nanolog::NonGuaranteedLogger(1), dir.path() / "ring-again", 1); // P stopped, after Q
    nanolog::initialize(nanolog::GuaranteedLogger(), dir.path() / "f", 1);              // takes Q back
    nanolog::initialize(nanolog::GuaranteedLogger(), dir.path() / "g", 1);              // completes f.1.log
    EXPECT_NE(read_file(dir.path() / "f.1.log").find("held inside Q"), std::string::npos)
        << "the oldest logger was taken back instead of the one stopped longest ago";
}

// `line <n>` for each n < count, through NANO_LOG -- out of EXPECT_EXIT, whose statement would leave the
// comparison NANO_LOG expands to unused (-Wunused-comparison).
void
log_lines(std::uint64_t const count) {
    for (std::uint64_t n = 0; n < count; ++n)
        NANO_LOG(nanolog::LogLevel::INFO) << "line " << n;
}

/**
 * @test At exit the published logger's file is complete, and a static destructor that logs after it is not a crash.
 * @brief The global logger was a std::unique_ptr, destroyed at exit while `atomic_nanologger` still named it: a static
 *        destructor that ran later and logged -- `logs_at_exit`, constructed before nanolog's globals -- pushed into the
 *        destroyed logger. It is now stopped at exit, as destroying it did (its file is complete), and not freed: the
 *        later line goes into a queue nobody writes. The child's writer is held after its first line until a logger
 *        begins to stop, and the child logs a Buffer's worth less one (none retires, none is destroyed): every line is
 *        in the files only if the logger was drained at exit. The child process inherits the directory through the
 *        environment (a "threadsafe" death test runs the test body again in a fresh process).
 */
TEST(GuaranteedLoggerReplacementDeathTest, AtExitTheLoggerFileIsCompleteAndALaterLineIsNoCrash) {
    std::string const style = GTEST_FLAG_GET(death_test_style);
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    constexpr std::uint64_t count = nanolog::Buffer::size - 1; // one Buffer: none is retired, none destroyed
    // Wide on Windows, so a temporary path outside the ANSI code page goes through.
#ifdef _WIN32
    constexpr wchar_t const *variable  = L"QB_GUARANTEED_LOGGER_EXIT_DIR";
    wchar_t const *const     inherited = ::_wgetenv(variable);
#else
    constexpr char const *variable  = "QB_GUARANTEED_LOGGER_EXIT_DIR";
    char const *const     inherited = std::getenv(variable);
#endif
    std::unique_ptr<LogDir>     owned;
    std::filesystem::path const dir = inherited ? std::filesystem::path(inherited) : (owned = std::make_unique<LogDir>())->path();
#ifdef _WIN32
    ::_wputenv_s(variable, dir.c_str());
#else
    ::setenv(variable, dir.c_str(), 1);
#endif
    EXPECT_EXIT(
        {
            hold_writer.store(true, std::memory_order_release);
            nanolog::initialize(nanolog::GuaranteedLogger(), dir / "exit", 1);
            log_lines(count);
            logs_at_exit.armed = true;
            std::exit(0);
        },
        ::testing::ExitedWithCode(0), "");
#ifdef _WIN32
    ::_wputenv_s(variable, L"");
#else
    ::unsetenv(variable);
#endif
    GTEST_FLAG_SET(death_test_style, style);
    auto const seen = lines_written(dir, count);
    for (std::uint64_t n = 0; n < count; ++n)
        ASSERT_EQ(seen[n], 1) << "line " << n << " was written " << seen[n] << " times: the logger was not drained at exit";
    EXPECT_EQ(seen[count], 0) << "a line logged after the logger stopped was written";
}

#if !defined(_WIN32)
// The forked child's whole life in the test below: destroy the logger its parent built directly, log one line into
// the published one it inherited, then exit().
void
destroy_log_and_exit(nanolog::NanoLogger *direct) {
    delete direct; // its writer is the parent's: a std::thread destroyed joinable would terminate
    NANO_LOG(nanolog::LogLevel::INFO) << "logged by the forked child";
    std::exit(0);
}

/**
 * @test A child of fork() that calls exit() exits without joining its parent's writer (Huly QB-1246).
 * @brief The "fast" death-test style forks and does not exec: the child inherits the published logger, whose writer
 *        thread exists only in the parent, and its exit() ran StopAtExit, which joined that thread -- undefined; libc++
 *        on macOS refuses the join and terminates. The hook makes the stop itself the failure, on every C library: a
 *        child that reaches it exits 4. The parent's writer is idle when the child is forked
 *        (its line is in the file), so the fork meets no line being written (Huly QB-1193). The child writes nothing
 *        into its parent's file, and the parent's logger still writes its own lines. The child also destroys a logger
 *        its parent built directly, never published, whose std::thread must not be destroyed joinable there.
 */
TEST(GuaranteedLoggerForkDeathTest, AForkedChildExitsWithoutJoiningItsParentsWriter) {
    std::string const style = GTEST_FLAG_GET(death_test_style);
    LogDir const      dir;
    nanolog::initialize(nanolog::GuaranteedLogger(), dir.path() / "parent", 1);
    NANO_LOG(nanolog::LogLevel::INFO) << "logged before the fork";
    for (int i = 0; i < 10000 && read_file(dir.path() / "parent.1.log").find("logged before the fork") == std::string::npos; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_NE(read_file(dir.path() / "parent.1.log").find("logged before the fork"), std::string::npos) << "the writer never wrote";
    auto *const direct = new nanolog::NanoLogger(nanolog::GuaranteedLogger(), dir.path() / "direct", 1); // never published

    GTEST_FLAG_SET(death_test_style, "fast"); // fork, no exec: the child keeps the parent's logger
    fork_test_parent.store(static_cast<long>(::getpid()), std::memory_order_relaxed);
    EXPECT_EXIT(destroy_log_and_exit(direct), ::testing::ExitedWithCode(0), "") << "4: the child stopped a logger of its parent";
    fork_test_parent.store(0, std::memory_order_relaxed);
    GTEST_FLAG_SET(death_test_style, style);
    delete direct; // here, the writer is this process's: joined
    // Read before this process writes again: its stream writes at the offset it remembers, over a line the child
    // would have appended to the file they share.
    EXPECT_EQ(read_file(dir.path() / "parent.1.log").find("logged by the forked child"), std::string::npos)
        << "the child wrote into its parent's file";

    NANO_LOG(nanolog::LogLevel::INFO) << "logged after the fork";
    nanolog::initialize(nanolog::GuaranteedLogger(), dir.path() / "after", 1); // completes parent.1.log
    std::string const text = read_file(dir.path() / "parent.1.log");
    EXPECT_NE(text.find("logged after the fork"), std::string::npos) << "the parent's logger lost a line after the fork";
    EXPECT_EQ(text.find("logged before the fork"), text.rfind("logged before the fork")) << "a line was written twice";
    EXPECT_EQ(text.find("logged by the forked child"), std::string::npos) << "the child wrote into its parent's file";
}
#endif
} // namespace guaranteed_logger_buffer_test
