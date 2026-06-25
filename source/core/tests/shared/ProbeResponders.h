/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file shared/ProbeResponders.h
 * @brief Shared request/response scaffolding for the patterns aggregate/quorum/deadline tests.
 *
 * `Probe` is a `qb::Request<int>` whose responder computes `response = seq + 1`, so every gathered
 * value is framework-computed (not a value the test handed in) — the basis for the EXACT-sum oracles
 * in the scatter / quorum / deadline suites. The two canonical responders are hoisted here to a
 * single source of truth instead of being re-declared byte-for-byte in each file:
 *   - `FastResponder`   answers immediately via `qb::answer` (response = seq + 1);
 *   - `SilentResponder` registers `Probe` but NEVER answers — used to force timeouts / parked waits.
 *
 * Lives in `namespace qb::test`. The `Probe(seq, corr, resp)` constructor is for responders that
 * reply out-of-band via `push_to` (e.g. a slow responder that defers its reply in a coroutine).
 */

#ifndef QB_CORE_TESTS_SHARED_PROBE_RESPONDERS_H
#define QB_CORE_TESTS_SHARED_PROBE_RESPONDERS_H

#include <cstdint>
#include <qb/actor.h>
#include <qb/core/patterns.h>

namespace qb::test {

/// Exchange event: the responder fills `response = seq + 1`, so the value is responder-computed.
struct Probe : public qb::Request<int> {
    int seq{0};
    Probe() = default;
    explicit Probe(int s)
        : seq(s) {}
    /// For out-of-band replies (push_to): carries the echoed correlation id + the computed response.
    Probe(int s, std::uint64_t corr, int resp)
        : seq(s) {
        this->correlation_id = corr;
        this->response       = resp;
    }
};

/// Answers immediately with `seq + 1`.
class FastResponder : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Probe>(*this);
        co_return true;
    }
    void
    on(Probe &p) {
        qb::answer(*this, p, [](Probe const &r) { return r.seq + 1; });
    }
};

/// Registers `Probe` but never answers — forces a timeout or keeps an asker parked.
class SilentResponder : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Probe>(*this);
        co_return true;
    }
    void
    on(Probe &) {}
};

} // namespace qb::test

#endif // QB_CORE_TESTS_SHARED_PROBE_RESPONDERS_H
