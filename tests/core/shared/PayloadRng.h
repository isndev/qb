/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file shared/PayloadRng.h
 * @brief The one PRNG the test/benchmark event payloads draw from — seeded once per thread.
 *
 * Single source of truth for `shared/ChecksumEvent.h` and `shared/TestEvent.h`.
 *
 * ### Why this exists
 * Both payload events fill 32 random bytes in their constructor. The obvious spelling —
 * a `std::random_device` plus a fresh `std::mt19937` *inside the constructor* — makes every
 * single event pay:
 *
 *  1. **one OS CSPRNG draw.** On macOS libc++ `std::random_device` is backed by
 *     `arc4random()`, and corecrypto serialises every caller in the process behind **one**
 *     `os_unfair_lock`. With one VirtualCore thread per core all emitting events at once,
 *     the run stops measuring qb and starts measuring futex traffic on that global lock:
 *     `__ulock_wait2` / `__ulock_wake` dominated the profile, `sys` time exceeded `user`
 *     time, and wall-clock became a function of whatever else the machine was doing — which
 *     is exactly what made `qb-core-test-system-messaging-api` flaky and eventually blew the
 *     `tier:system` 120 s ctest timeout when the suite runs in parallel.
 *  2. **a full 624-word Mersenne-Twister seeding + twist (~1 µs)** to then consume 32 bytes.
 *
 * Seeding **once per thread** keeps every event's payload distinct and unpredictable — all
 * `checkSum()` needs is that the bytes differ and match their stored sum — while reducing
 * event construction to 32 PRNG draws. Measured on the messaging suite: 21.9 s → 2.8 s for
 * `ActorEventMulti / *`, with `sys` time dropping ~10x.
 *
 * @attention Any new shared payload event MUST draw from here rather than constructing its
 *            own `std::random_device` / `std::mt19937`, or it silently reintroduces the
 *            contention above.
 */

#ifndef QB_CORE_TESTS_SHARED_PAYLOAD_RNG_H
#define QB_CORE_TESTS_SHARED_PAYLOAD_RNG_H

#include <random>

namespace qb::test::detail {

/**
 * @brief The per-thread payload generator, seeded once from the OS entropy source.
 * @return Reference to this thread's `std::mt19937`.
 */
[[nodiscard]] inline std::mt19937 &
payload_rng() noexcept {
    static thread_local std::mt19937 generator{std::random_device{}()};
    return generator;
}

} // namespace qb::test::detail

#endif // QB_CORE_TESTS_SHARED_PAYLOAD_RNG_H
