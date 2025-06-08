/**
 * @file qb/core/tests/benchmark/bm-producer-consumer.cpp
 * @brief Producer-consumer benchmark for the QB Actor Framework
 *
 * This file contains benchmark tests measuring the throughput of the producer-consumer
 * pattern in the QB Actor Framework. It tests how efficiently a producer actor can
 * send a large number of events to a consumer actor in both mono-core and multi-core
 * scenarios.
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
 * @ingroup Benchmarks
 */

#include <benchmark/benchmark.h>
#include <qb/actor.h>
#include <qb/main.h>

struct TestEvent : public qb::Event {
    uint64_t ttl;
};

class ConsumerActor final : public qb::Actor {
    uint64_t started_at;

public:
    ConsumerActor() {
        registerEvent<TestEvent>(*this);
    }

    void
    on(TestEvent const &event) {
        switch (event.ttl) {
            case 1000000:
                qb::io::cout()
                    << "Thoughput 1 event ~="
                    << (qb::Timestamp::nano() - started_at) / 1000000 << std::endl;
                broadcast<qb::KillEvent>();
                break;
            case 1:
                started_at = qb::Timestamp::nano();
            default:
                break;
        }
    }
};

class ProducerActor final : public qb::Actor {
    const qb::pipe _to_pipe;

public:
    ProducerActor() = delete;
    ProducerActor(qb::ActorId to)
        : _to_pipe(getPipe(to)) {}
    ~ProducerActor() = default;

    bool
    onInit() final {
        for (auto i = 1; i <= 1000000; ++i)
            _to_pipe.push<TestEvent>().ttl = i;
        return true;
    }
};

static void
BM_Mono_Producer_Consumer(benchmark::State &state) {
    for (auto _ : state) {
        qb::Main main;

        main.addActor<ProducerActor>(0, main.addActor<ConsumerActor>(0));

        main.start(true);
        main.join();
    }
}

static void
BM_Multi_Producer_Consumer(benchmark::State &state) {
    for (auto _ : state) {
        qb::Main main;

        main.addActor<ProducerActor>(0, main.addActor<ConsumerActor>(2));

        main.start(true);
        main.join();
    }
}
//// Register the function as a benchmark
BENCHMARK(BM_Mono_Producer_Consumer);
BENCHMARK(BM_Multi_Producer_Consumer);

BENCHMARK_MAIN();