/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file shared/ChecksumEvent.h
 * @brief Self-validating payload event shared by the messaging / event delivery tests.
 *
 * `TestEvent` carries 32 random bytes plus their precomputed checksum, so a receiver can
 * prove the framework delivered the *exact bytes* — not merely an event of the right type.
 * `has_extra_data` + `copyAllocatedPayload()` additionally exercise the `allocated_push`
 * tail-payload path: `allocated_push<TestEvent>(32)` reserves 32 bytes immediately after the
 * event object; we mirror `_data` into that tail and `checkSum()` `memcmp`s it back, proving
 * the allocated region travelled intact through the mailbox.
 *
 * Hoisted verbatim from the former byte-for-byte clones in test-actor-event.cpp and
 * test-actor-service-event.cpp (single source of truth).
 */

#ifndef QB_CORE_TESTS_SHARED_CHECKSUM_EVENT_H
#define QB_CORE_TESTS_SHARED_CHECKSUM_EVENT_H

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <random>
#include <qb/actor.h>

namespace qb::test {

/// Event whose 32-byte payload self-validates via a stored byte sum (+ optional allocated tail).
struct TestEvent : public qb::Event {
    std::uint8_t  _data[32];
    std::uint32_t _sum           = 0;
    bool          has_extra_data = false;

    TestEvent() {
        std::random_device                 rand_dev;
        std::mt19937                       generator(rand_dev());
        std::uniform_int_distribution<int> random_number(0, 255);
        std::generate(std::begin(_data), std::end(_data), [&]() {
            auto number = static_cast<std::uint8_t>(random_number(generator));
            _sum += number;
            return number;
        });
    }

    /// True iff the 32 bytes still sum to `_sum` and (when allocated) the tail matches `_data`.
    [[nodiscard]] bool
    checkSum() const {
        bool tail_ok = true;
        if (has_extra_data) {
            tail_ok = std::memcmp(_data, reinterpret_cast<const std::uint8_t *>(this) + sizeof(TestEvent), sizeof(_data)) == 0;
        }
        return std::accumulate(std::begin(_data), std::end(_data), 0u) == _sum && tail_ok;
    }
};

/// Copy `_data` into the 32-byte region the matching `allocated_push<TestEvent>(32)` reserved.
inline void
copyAllocatedPayload(TestEvent &event) {
    auto *payload = reinterpret_cast<std::uint8_t *>(&event) + sizeof(TestEvent);
    std::memcpy(payload, event._data, sizeof(event._data));
}

} // namespace qb::test

#endif // QB_CORE_TESTS_SHARED_CHECKSUM_EVENT_H
