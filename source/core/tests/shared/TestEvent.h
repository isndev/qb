/**
 * @file qb/source/core/tests/shared/TestEvent.h
 * @brief Shared event payloads for the qb-core latency / throughput benchmarks.
 *
 * \c LightEvent / \c TestEvent are the timestamp-carrying latency-sample events (steady- and
 * high-resolution-clock \c _timepoint) consumed by the ping-pong / multicast / pipeline
 * latency benches via \c ConsumerActor / \c ProducerActor.
 *
 * The payload-size axis types are hoisted here from the former per-bench local
 * re-declarations (bm-ping-pong.cpp, bm-payload-throughput.cpp) so the "event size" sweep
 * has a single source of truth:
 *   - \c BigEvent     — ~1 KiB inline (\c padding[127] of \c uint64_t after \c _ttl), trivially
 *                       relocatable, exercises the large-but-inline copy path;
 *   - \c DynamicEvent — \c std::vector<int>(512) after \c _ttl: a non-trivial (heap-owning)
 *                       payload that forces real ctor/dtor + move on every hop;
 *   - \c SizedPingEvent<ExtraWords> — compile-time-parametric padding (\c _ttl plus
 *                       \c std::array<uint64_t, ExtraWords>) for the deterministic payload-volume
 *                       sweep (\c approx_payload_bytes proxy = \c sizeof(SizedPingEvent<...>)).
 *
 * \c TinyEvent stays local to bm-ping-pong.cpp (it is the zero-axis baseline of that bench
 * only, not a shared building block).
 */

#include <array>
#include <chrono>
#include <cstring>
#include <numeric>
#include <qb/event.h>
#include <qb/system/time.h>
#include <random>
#include <vector>

#ifndef QB_TESTEVENT_H
#define QB_TESTEVENT_H

struct LightEvent : public qb::Event {
    std::chrono::steady_clock::time_point _timepoint;
    uint32_t                              _ttl;

    LightEvent()
        : _timepoint(std::chrono::steady_clock::now())
        , _ttl(0) {}

    explicit LightEvent(uint32_t const ttl)
        : _timepoint(std::chrono::steady_clock::now())
        , _ttl(ttl) {}
};

struct TestEvent : public qb::Event {
    uint8_t                                        _data[32];
    uint32_t                                       _sum = 0;
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
            ret = !memcmp(_data, reinterpret_cast<const uint8_t *>(this) + sizeof(TestEvent), sizeof(_data));
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

// Payload-size axis (hoisted verbatim from the former per-bench local re-declarations).

/// ~1 KiB inline payload: `padding[127]` of `uint64_t` after `_ttl` (large but trivially copyable).
struct BigEvent : public qb::Event {
    std::uint64_t _ttl;
    std::uint64_t padding[127];
    explicit BigEvent(std::uint64_t ttl)
        : _ttl(ttl)
        , padding() {}
};

/// Non-trivial payload: `std::vector<int>(512)` after `_ttl` forces real ctor/dtor + move per hop.
struct DynamicEvent : public qb::Event {
    std::uint64_t    _ttl;
    std::vector<int> vec;
    explicit DynamicEvent(std::uint64_t ttl)
        : _ttl(ttl)
        , vec(512, 8) {}
};

/// Compile-time-parametric padding: `_ttl` plus `ExtraWords` trailing `uint64_t` words.
template <std::size_t ExtraWords>
struct SizedPingEvent final : public qb::Event {
    std::uint64_t                         _ttl = 0;
    std::array<std::uint64_t, ExtraWords> _pad{};

    SizedPingEvent() = default;
    explicit SizedPingEvent(std::uint64_t const ttl)
        : _ttl(ttl) {}
};

#endif // QB_TESTEVENT_H
