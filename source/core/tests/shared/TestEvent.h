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

#include <random>
#include <numeric>
#include <chrono>
#include <qb/event.h>
#include <qb/system/timestamp.h>

#ifndef QB_TESTEVENT_H
#define QB_TESTEVENT_H

struct LightEvent : public qb::Event {
    std::chrono::high_resolution_clock::time_point _timepoint;
    uint32_t _ttl;

    LightEvent()
        : _timepoint(std::chrono::high_resolution_clock::now())
    {}

    LightEvent(uint32_t const ttl)
        : _timepoint(std::chrono::high_resolution_clock::now())
        , _ttl(ttl)
    {}
};

struct TestEvent : public qb::Event
{
    uint8_t  _data[32];
    uint32_t _sum;
    std::chrono::high_resolution_clock::time_point _timepoint;
    uint32_t _ttl;
    bool has_extra_data = false;

    TestEvent() {
        __init__();
    }

    TestEvent(uint32_t const ttl) {
        __init__();
        _ttl = ttl;
    }


    bool checkSum() const {
        auto ret = true;
        if (has_extra_data) {
            ret = !memcmp(_data, reinterpret_cast<const uint8_t *>(this) + sizeof(TestEvent), sizeof(_data));
        }

        return std::accumulate(std::begin(_data), std::end(_data), 0u) == _sum && ret;
    }

private:
    void __init__() {
        std::random_device                  rand_dev;
        std::mt19937                        generator(rand_dev());

        std::uniform_int_distribution<int> random_number(0, 255);
        std::generate(std::begin(_data), std::end(_data), [&](){
            auto number = static_cast<uint8_t>(random_number(generator));
            _sum += number;
            return number;
        });
        _timepoint =  std::chrono::high_resolution_clock::now();
    }
};

#endif //QB_TESTEVENT_H
