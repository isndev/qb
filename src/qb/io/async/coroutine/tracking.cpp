/**
 * @file qb/io/async/coroutine/tracking.cpp
 * @brief The out-of-line half of the opt-in suspension tracking (Huly QB-71): this thread's records.
 *
 * Only reached with tracking on (coroutine/tracking.h keeps the off path inline: one branch). The table hangs off
 * `suspension_tracking::table`, an anchored thread-local pointer, so a host and a plugin with their own copy of qb-io
 * share one table per thread as they share the flag; each image's reaper frees it at thread exit, the first one to run
 * clearing the shared pointer.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 * @ingroup Coroutine
 */

#include <qb/io/async/coroutine/tracking.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <exception>
#include <new>
#include <ostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <qb/io/async/coroutine/scheduler.h>
#include <qb/system/time.h>

namespace qb::io::async::detail {

namespace {

struct suspension_record {
    char const   *kind;
    void const   *waits_on;
    std::uint64_t stamp; ///< `qb::tsc_ticks()`, or `qb::mono_now()` in ns when the table has no counter
};

struct suspension_table {
    std::unordered_map<void const *, suspension_record> records;              ///< with tracking on
    std::unordered_map<void const *, std::string>       names;                ///< the frames named at spawn, tracking or not
    double                                              ticks_per_ns = 0.;    ///< 0: the stamps are monotonic ns
    bool                                                calibrated   = false; ///< ticks_per_ns measured (the first time tracking went on)
};

[[nodiscard]] suspension_table *
table() noexcept {
    return static_cast<suspension_table *>(suspension_tracking::table);
}

[[nodiscard]] std::uint64_t
stamp(suspension_table const &t) noexcept {
    if (t.ticks_per_ns > 0.)
        return qb::tsc_ticks();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(qb::mono_now().time_since_epoch()).count());
}

/// `frames` and this thread's count in `gate.frames` move together: the gate counts the threads whose `frames` is true.
void
set_frames(bool const seen) noexcept {
    if (suspension_tracking::frames == seen)
        return;
    suspension_tracking::frames = seen;
    if (seen)
        suspension_tracking::gate.frames.fetch_add(1, std::memory_order_relaxed);
    else
        suspension_tracking::gate.frames.fetch_sub(1, std::memory_order_relaxed);
}

/// This thread's recorder. A transfer awaiter's `track_then` leaves its suspension in `suspension_tracking::pending`
/// and transfers here, so that the awaiter makes no call (coroutine/tracking.h); the recorder records the suspension,
/// transfers to the coroutine the awaiter handed the thread to, and waits there for the next one. It never finishes:
/// it is created when this thread starts tracking and destroyed, suspended, when it stops or at thread exit.
struct recorder_coroutine {
    struct promise_type {
        recorder_coroutine
        get_return_object() noexcept {
            return {std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        /// Out of memory: no recorder, and tracking stays off (set_thread_tracking returns false).
        static recorder_coroutine
        get_return_object_on_allocation_failure() noexcept {
            return {};
        }
        static void *
        operator new(std::size_t size) noexcept {
            return ::operator new(size, std::nothrow);
        }
        static void
        operator delete(void *p) noexcept {
            ::operator delete(p);
        }
        std::suspend_always
        initial_suspend() const noexcept {
            return {};
        }
        std::suspend_always
        final_suspend() const noexcept {
            return {};
        }
        void
        return_void() const noexcept {}
        void
        unhandled_exception() const noexcept {
            std::terminate();
        }
    };
    std::coroutine_handle<promise_type> handle{};
};

/// The recorder's hop to the coroutine whose suspension it has just recorded.
struct hand_over {
    std::coroutine_handle<> next;
    [[nodiscard]] bool
    await_ready() const noexcept {
        return false;
    }
    // awaiter-tracking: the recorder's own hop -- it hands over the suspension it recorded just before
    [[nodiscard]] std::coroutine_handle<>
    await_suspend(std::coroutine_handle<>) const noexcept {
        return next;
    }
    void
    await_resume() const noexcept {}
};

recorder_coroutine
run_recorder() {
    for (;;) {
        const pending_suspension p = suspension_tracking::pending;
        note_suspension(p.frame, p.kind, p.waits_on);
        co_await hand_over{p.next};
    }
}

/// `on`, this thread's recorder and its count in `gate.tracking` move together. False when the recorder cannot be
/// allocated: tracking stays off.
bool
set_on(bool const on) noexcept {
    if (suspension_tracking::on == on)
        return true;
    if (on) {
        auto r = run_recorder();
        if (!r.handle)
            return false;
        suspension_tracking::recorder = r.handle.address();
        suspension_tracking::on       = true;
        suspension_tracking::gate.tracking.fetch_add(1, std::memory_order_relaxed);
    } else {
        suspension_tracking::on = false;
        suspension_tracking::gate.tracking.fetch_sub(1, std::memory_order_relaxed);
        std::coroutine_handle<>::from_address(std::exchange(suspension_tracking::recorder, nullptr)).destroy();
    }
    return true;
}

/// Frees this thread's table at thread exit when it is still allocated, and takes the thread out of both gates.
struct table_reaper {
    bool armed = false;
    ~table_reaper() {
        if (armed) {
            delete table();
            suspension_tracking::table = nullptr;
            (void) set_on(false);
            set_frames(false);
        }
    }
};
thread_local table_reaper t_reaper;

/// This thread's table, allocated on first use; null when it cannot be.
[[nodiscard]] suspension_table *
table_or_create() noexcept {
    if (auto *t = table())
        return t;
    auto *t = new (std::nothrow) suspension_table{};
    if (!t)
        return nullptr;
    suspension_tracking::table = t;
    t_reaper.armed             = true;
    return t;
}

/// With tracking off and no name left there is nothing to keep: the table goes, and a frame's destruction stops being
/// seen.
void
release_if_idle(suspension_table *t) noexcept {
    if (suspension_tracking::on || !t->names.empty())
        return;
    delete t;
    suspension_tracking::table = nullptr;
    set_frames(false);
}

} // namespace

void
note_suspension(void *frame, char const *kind, void const *waits_on) noexcept {
    auto *t = table();
    if (!t)
        return;
    try {
        t->records.insert_or_assign(frame, suspension_record{kind, waits_on, stamp(*t)});
    } catch (...) {
        // out of memory: this suspension goes unrecorded, the coroutine is not affected
    }
}

void
forget_frame(void const *frame) noexcept {
    auto *t = table();
    if (!t)
        return;
    if (!t->records.empty())
        t->records.erase(frame);
    if (!t->names.empty() && t->names.erase(frame) != 0)
        release_if_idle(t);
}

bool
name_frame(void const *frame, std::string_view const name) noexcept {
    auto *t = table_or_create();
    if (!t)
        return false;
    try {
        t->names.insert_or_assign(frame, std::string(name));
    } catch (...) {
        release_if_idle(t);
        return false; // out of memory: the coroutine runs, unnamed
    }
    set_frames(true);
    return true;
}

std::string_view
frame_name(void const *frame) noexcept {
    if (auto *t = table())
        if (const auto it = t->names.find(frame); it != t->names.end())
            return it->second;
    return {};
}

namespace {

/// The CPU counter's ticks per nanosecond, measured against the monotonic clock over ~200 us, as the listener measures
/// it for its own cadence; 0 when the counter does not move (no usable counter on this platform).
[[nodiscard]] double
calibrate_ticks_per_ns() noexcept {
    const auto          m0 = qb::mono_now();
    const std::uint64_t t0 = qb::tsc_ticks();
    while (qb::mono_now() - m0 < std::chrono::microseconds{200}) {
    }
    const auto          m1 = qb::mono_now();
    const std::uint64_t t1 = qb::tsc_ticks();
    const auto          ns = std::chrono::duration_cast<std::chrono::nanoseconds>(m1 - m0).count();
    return (ns > 0 && t1 > t0) ? static_cast<double>(t1 - t0) / static_cast<double>(ns) : 0.;
}

} // namespace

bool
set_thread_tracking(bool const enable) noexcept {
    if (!enable) {
        (void) set_on(false);
        if (auto *t = table()) {
            t->records.clear();
            release_if_idle(t);
        }
        return true;
    }
    auto *t = table_or_create();
    if (!t)
        return false;
    if (!t->calibrated) {
        t->ticks_per_ns = calibrate_ticks_per_ns();
        t->calibrated   = true;
    }
    if (!set_on(true)) {
        release_if_idle(t);
        return false; // out of memory: no recorder, tracking stays off
    }
    set_frames(true);
    return true;
}

void
visit_suspensions(void *ctx, void (*fn)(void *, suspension_view const &)) {
    auto *t = table();
    if (!t)
        return;
    const std::uint64_t now = stamp(*t);
    for (auto const &[frame, r] : t->records) {
        const auto elapsed = now >= r.stamp ? now - r.stamp : 0;
        const auto age_ns =
            t->ticks_per_ns > 0. ? static_cast<long long>(static_cast<double>(elapsed) / t->ticks_per_ns) : static_cast<long long>(elapsed);
        fn(ctx, suspension_view{frame, r.kind, r.waits_on, age_ns});
    }
}

} // namespace qb::io::async::detail

namespace qb::io::async {

std::vector<parked_coroutine>
CoroutineScheduler::dump() const {
    // Completed roots wait in frames_to_destroy_ for the drain that frees them: not parked. The test is on the list,
    // never on the frame -- a record may outlive a frame no promise of qb owned (see set_suspension_tracking).
    std::unordered_set<void const *> completed;
    for (auto const &h : frames_to_destroy_)
        if (h)
            completed.insert(h.address());
    auto name_of = [](void const *f) {
        return std::string(detail::frame_name(f));
    };
    std::vector<parked_coroutine>    out;
    std::unordered_set<void const *> listed;
    detail::for_each_suspension([&](detail::suspension_view const &v) {
        if (completed.count(v.frame) != 0)
            return;
        listed.insert(v.frame);
        out.push_back(parked_coroutine{
            v.frame, name_of(v.frame), v.kind, std::chrono::nanoseconds{v.age_ns}, v.waits_on,
            owned_frames_.count(const_cast<void *>(v.frame)) != 0
        });
    });
    // the coroutines parked on a loop watcher (a timer, a socket, an async operation, an offload), which the scheduler
    // always knows: with tracking off, or parked before it went on
    suspended_coroutines_.for_each([&](void *addr) {
        if (listed.insert(addr).second)
            out.push_back(parked_coroutine{addr, name_of(addr), "watcher", {}, nullptr, owned_frames_.count(addr) != 0});
    });
    // the roots with no record: tracking off, not run yet, or parked on an awaitable that is not qb's
    owned_frames_.for_each([&](void *addr) {
        if (completed.count(addr) == 0 && listed.insert(addr).second)
            out.push_back(parked_coroutine{addr, name_of(addr), nullptr, {}, nullptr, true});
    });
    // the longest waits first; what has no age keeps its place behind them
    std::stable_sort(out.begin(), out.end(), [](parked_coroutine const &a, parked_coroutine const &b) { return a.age > b.age; });
    return out;
}

void
CoroutineScheduler::dump(std::ostream &os) const {
    const auto                                    parked = dump();
    std::unordered_map<void const *, std::size_t> index;
    index.reserve(parked.size());
    for (std::size_t i = 0; i < parked.size(); ++i)
        index.emplace(parked[i].frame, i);
    auto one = [&os](parked_coroutine const &p) {
        if (!p.name.empty())
            os << '"' << p.name << "\" ";
        os << p.frame;
        if (p.kind) {
            os << ' ' << p.kind;
            if (p.age.count() > 0)
                os << ' ' << std::chrono::duration<double>(p.age).count() << " s";
        } else {
            os << " (not recorded)";
        }
    };
    std::vector<bool> shown(parked.size(), false);
    os << "coroutines parked: " << parked.size() << (detail::suspension_tracking::on ? " (tracking on)" : " (tracking off)") << '\n';
    for (std::size_t i = 0; i < parked.size(); ++i) {
        if (!parked[i].root)
            continue;
        os << "  ";
        one(parked[i]);
        shown[i] = true;
        // the chain: a root awaiting a task, that task awaiting the next, down to the leaf (bounded: a cycle is not
        // possible in a live chain, a stale record could fake one)
        std::size_t at = i;
        for (std::size_t depth = 0; parked[at].waits_on && depth < parked.size(); ++depth) {
            const auto next = index.find(parked[at].waits_on);
            if (next == index.end())
                break;
            at = next->second;
            os << " -> ";
            one(parked[at]);
            shown[at] = true;
        }
        os << '\n';
    }
    // the parked coroutines no root leads to
    for (std::size_t i = 0; i < parked.size(); ++i) {
        if (shown[i])
            continue;
        os << "  ";
        one(parked[i]);
        os << '\n';
    }
}

} // namespace qb::io::async
