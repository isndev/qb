/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2019 isndev (www.qbaf.io). All rights reserved.
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

#include <benchmark/benchmark.h>
#include <qb/actor.h>
#include <qb/main.h>
#include "../shared/TestEvent.h"
#include "../shared/TestLatency.h"

class PongActor : public qb::Actor {
public:
    virtual bool onInit() override final {
        registerEvent<LightEvent>(*this);
        return true;
    }

    void on(LightEvent &event) {
        --event._ttl;
        reply(event);
    }

};

class PingActor : public qb::Actor {
    pg::latency<1000 * 1000, 900000> _latency;
public:
    ~PingActor() {
        _latency.generate<std::ostream, std::chrono::nanoseconds>(std::cout, "ns");
    }

    virtual bool onInit() override final {
        registerEvent<qb::RequireEvent>(*this);
        registerEvent<LightEvent>(*this);
        require<PongActor>();
        return true;
    }

    void on(qb::RequireEvent const &event) {
        send<LightEvent>(event.getSource(), 1000000);
    }

    void on(LightEvent const &event) {
        _latency.add(std::chrono::high_resolution_clock::now() - event._timepoint);
        if (event._ttl)
            send<LightEvent>(event.getSource(), event._ttl);
        else {
            kill();
            send<qb::KillEvent>(event.getSource());
        }
    }
};

bool run = true;
void thread_ping(qb::lockfree::spsc::ringbuffer<LightEvent, 4096> *spsc) {
    auto &latency = *new pg::latency<1000 * 1000, 900000>{};
    LightEvent events[4096];

    spsc[1].enqueue(LightEvent(1000000));
    while (qb::likely(run)) {
        // received
        spsc[0].dequeue([&] (auto event, auto nb_events) {
            for (auto i = 0u; i < nb_events; ++i) {
                latency.add(std::chrono::high_resolution_clock::now() - event[i]._timepoint);
                if (event[i]._ttl)
                    spsc[1].enqueue(LightEvent(event[i]._ttl));
                else {
                    run = false;
                }
            }
        }, events, 4096u);
    }
    latency.generate<std::ostream, std::chrono::nanoseconds>(std::cout, "ns");
    delete &latency;
}

void thread_pong(qb::lockfree::spsc::ringbuffer<LightEvent, 4096> *spsc) {
    LightEvent events[4096];

    while (qb::likely(run)) {
        // received
        spsc[1].dequeue([&] (auto event, auto nb_events) {
            for (auto i = 0u; i < nb_events; ++i) {
                --event[i]._ttl;
                spsc[0].enqueue(LightEvent(event[i]._ttl));
            }
        }, events, 4096u);
    }
}

static void BM_Reference_Multi_PingPong_Latency(benchmark::State& state) {
    for (auto _ : state) {
        auto spsc = new qb::lockfree::spsc::ringbuffer<LightEvent, 4096>[2];
        std::thread threads[2];

        threads[0] = std::thread(thread_ping, spsc);
        threads[1] = std::thread(thread_pong, spsc);

        for (auto &thread : threads) {
            if (thread.joinable())
                thread.join();
        }
        delete[] spsc;
    }
}

static void BM_Mono_PingPong_Latency(benchmark::State& state) {
    for (auto _ : state) {
        qb::Main  main({0});

        main.addActor<PingActor>(0);
        main.addActor<PongActor>(0);

        main.start(true);
        main.join();
    }
}

static void BM_Multi_PingPong_Latency(benchmark::State& state) {
    for (auto _ : state) {
        qb::Main  main({0, 1});

        main.addActor<PingActor>(0);
        main.addActor<PongActor>(1);

        main.start(true);
        main.join();
    }
}
// Register the function as a benchmark
BENCHMARK(BM_Reference_Multi_PingPong_Latency);
BENCHMARK(BM_Mono_PingPong_Latency);
BENCHMARK(BM_Multi_PingPong_Latency);

BENCHMARK_MAIN();