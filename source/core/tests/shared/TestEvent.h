

#include <chrono>
#include <cstring>
#include <numeric>
#include <qb/event.h>
#include <qb/system/timestamp.h>
#include <random>

#ifndef QB_TESTEVENT_H
#define QB_TESTEVENT_H

struct LightEvent : public qb::Event {
    std::chrono::high_resolution_clock::time_point _timepoint;
    uint32_t                                       _ttl;

    LightEvent()
        : _timepoint(std::chrono::high_resolution_clock::now())
        , _ttl(0) {}

    explicit LightEvent(uint32_t const ttl)
        : _timepoint(std::chrono::high_resolution_clock::now())
        , _ttl(ttl) {}
};

struct TestEvent : public qb::Event {
    uint8_t                                        _data[32];
    uint32_t                                       _sum;
    std::chrono::high_resolution_clock::time_point _timepoint;
    uint32_t                                       _ttl;
    bool                                           has_extra_data = false;

    TestEvent() {
        __init__();
    }

    explicit TestEvent(uint32_t const ttl) {
        __init__();
        _ttl = ttl;
    }

    [[nodiscard]] bool
    checkSum() const {
        auto ret = true;
        if (has_extra_data) {
            ret = !memcmp(_data,
                          reinterpret_cast<const uint8_t *>(this) + sizeof(TestEvent),
                          sizeof(_data));
        }

        return std::accumulate(std::begin(_data), std::end(_data), 0u) == _sum && ret;
    }

private:
    void
    __init__() {
        std::random_device rand_dev;
        std::mt19937       generator(rand_dev());

        std::uniform_int_distribution<int> random_number(0, 255);
        std::generate(std::begin(_data), std::end(_data), [&]() {
            auto number = static_cast<uint8_t>(random_number(generator));
            _sum += number;
            return number;
        });
        _timepoint = std::chrono::high_resolution_clock::now();
    }
};

#endif // QB_TESTEVENT_H
