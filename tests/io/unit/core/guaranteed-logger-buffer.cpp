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

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace guaranteed_logger_buffer_test {
enum Phase { before_publish, after_publish, destroyed };
void hook(int phase, unsigned int index);
} // namespace guaranteed_logger_buffer_test

// Compile the private queue in this test translation unit so the pause lands exactly between
// publishing the final slot and the producer's next access to the old Buffer. No logger global,
// file writer, or scheduling guess is involved. The production build compiles these hooks away.
#define NANOLOG_TEST_HOOK(phase, index) ::guaranteed_logger_buffer_test::hook(::guaranteed_logger_buffer_test::phase, index)
#include <qb/vendor/nanolog/nanolog.cpp>

namespace guaranteed_logger_buffer_test {
struct Gate {
    std::mutex              mutex;
    std::condition_variable cv;
    Phase                   pause_phase = after_publish;
    unsigned int            pause_index = nanolog::Buffer::size - 1;
    bool                    paused      = false;
    bool                    released    = false;
    std::atomic<int>        destroyed_count{0};
};

std::atomic<Gate *> active_gate{nullptr};

void
hook(int phase, unsigned int index) {
    Gate *gate = active_gate.load(std::memory_order_acquire);
    if (gate == nullptr)
        return;
    if (phase == destroyed) {
        gate->destroyed_count.fetch_add(1, std::memory_order_release);
        return;
    }
    if (phase != gate->pause_phase || index != gate->pause_index)
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
} // namespace guaranteed_logger_buffer_test
