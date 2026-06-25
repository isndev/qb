/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file shared/InitFixtures.h
 * @brief Shared scaffolding for the async-actor-init system suite (system/init/).
 *
 * Hoisted verbatim from the three former init monoliths (test-actor-async-init.cpp,
 * test-actor-async-init-torture.cpp, test-actor-init-robustness.cpp) so the split
 * targets share a single source of truth instead of byte-for-byte clones:
 *
 *   - `ScopedDeadline`        — RAII over the process-global `VirtualCore::activation_deadline_ns`
 *                               knob (the activation deadline that bounds a suspended `onInit()`).
 *                               Saves/restores the previous value so tests can lower or disable it
 *                               without leaking the change into the next test.
 *   - `PayloadEvent`          — a `push`'d event carrying a heap `std::string`, with a strict
 *                               construction/destruction balance counter (`PayloadEvent::live`).
 *                               `live` returns to 0 iff every constructed instance is destroyed:
 *                               the qb event layer byte-relocates events (no ctor/dtor on the
 *                               relocation), so the only ctor is the placement-new at push and the
 *                               only dtor is the framework disposer — `live != 0` after the engine
 *                               drains means an event payload leaked. The suite runs under
 *                               ASAN detect_leaks=0, so LeakSanitizer would NOT catch the leak;
 *                               this counter does, and ASan still catches any double-free of the
 *                               heap `data`.
 *   - `Tick`                  — a trivial int-carrying probe event (stash/replay ordering tests).
 *   - `Cfg` / `CfgService`    — the `Request<int>` exchange + one-shot responder used by every
 *                               in-init `ask*` test (`answer` returns `key * 10`).
 *
 * All helpers live in namespace `qb::test`. The PayloadEvent ctor/dtor balance is the
 * load-bearing teeth of init-stash-lifetime.cpp — do not weaken it.
 */

#ifndef QB_CORE_TESTS_SHARED_INIT_FIXTURES_H
#define QB_CORE_TESTS_SHARED_INIT_FIXTURES_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

#include <qb/actor.h>
#include <qb/core/VirtualCore.h>
#include <qb/core/patterns.h>
#include <qb/io/async/coroutine.h>

namespace qb::test {

// ---------------------------------------------------------------------------
// RAII for the process-global activation-deadline knob (restored after the test).
// A `0` value disables the bound entirely (no force-fail of a slow init).
// ---------------------------------------------------------------------------
struct ScopedDeadline {
    std::uint64_t _saved;
    explicit ScopedDeadline(std::uint64_t ns)
        : _saved(qb::VirtualCore::activation_deadline_ns) {
        qb::VirtualCore::activation_deadline_ns = ns;
    }
    ScopedDeadline(const ScopedDeadline &)            = delete;
    ScopedDeadline &operator=(const ScopedDeadline &) = delete;
    ~ScopedDeadline() {
        qb::VirtualCore::activation_deadline_ns = _saved;
    }
};

// ---------------------------------------------------------------------------
// A `push`'d event with a heap payload + a strict construction/destruction
// balance counter. `live` returns to 0 iff every constructed instance is destroyed.
// (See file header for why this is the only leak oracle that works under
// ASAN detect_leaks=0.)
// ---------------------------------------------------------------------------
struct PayloadEvent : public qb::Event {
    // `inline` so the single counter is shared across every TU that includes this header
    // (multiple init-suite targets do) without an ODR clash or a separate .cpp definition.
    inline static std::atomic<long> live{0};
    std::string                     data; // heap allocation (> SSO) so ASan also guards double-free
    int                      seq{0};

    PayloadEvent() {
        live.fetch_add(1, std::memory_order_relaxed);
    }
    explicit PayloadEvent(int s)
        : data(64, 'x') // force a heap allocation
        , seq(s) {
        live.fetch_add(1, std::memory_order_relaxed);
    }
    PayloadEvent(const PayloadEvent &o)
        : qb::Event(o)
        , data(o.data)
        , seq(o.seq) {
        live.fetch_add(1, std::memory_order_relaxed);
    }
    PayloadEvent(PayloadEvent &&o) noexcept
        : qb::Event(o)
        , data(std::move(o.data))
        , seq(o.seq) {
        live.fetch_add(1, std::memory_order_relaxed);
    }
    ~PayloadEvent() {
        live.fetch_sub(1, std::memory_order_relaxed);
    }
};

// ---------------------------------------------------------------------------
// A trivial int-carrying probe — used for stash/replay ordering tests where the
// payload is irrelevant and only the sequence number + delivery count matter.
// ---------------------------------------------------------------------------
struct Tick : public qb::Event {
    int n{0};
    Tick() = default;
    explicit Tick(int v)
        : n(v) {}
};

// ---------------------------------------------------------------------------
// The `Request<int>` exchange + a one-shot responder used by the in-init `ask*`
// tests. `answer` returns `key * 10`; the responder kills itself after serving
// `kills_after` requests so the engine can drain.
// ---------------------------------------------------------------------------
struct Cfg : public qb::Request<int> {
    int key{0};
    Cfg() = default;
    explicit Cfg(int k)
        : key(k) {}
};

class CfgService : public qb::Actor {
    int _remaining;

public:
    explicit CfgService(int kills_after = 1)
        : _remaining(kills_after) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Cfg>(*this);
        co_return true; // sync — active before anyone asks
    }
    void
    on(Cfg &e) {
        qb::answer(*this, e, [](Cfg const &r) { return r.key * 10; });
        if (--_remaining <= 0)
            kill();
    }
};

} // namespace qb::test

#endif // QB_CORE_TESTS_SHARED_INIT_FIXTURES_H
