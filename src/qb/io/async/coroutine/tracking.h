/**
 * @file qb/io/async/coroutine/tracking.h
 * @brief Opt-in suspension tracking: what each parked coroutine waits on, and for how long (Huly QB-71).
 *
 * Every awaiter of qb names what it waits on (`qb_suspension_kind`: "sleep", "io", "ask", "mutex", "task", ...) and
 * calls `detail::track_suspension` first thing in its `await_suspend` -- or, the two that transfer to another coroutine
 * (a task's, a generator's), `detail::track_then` last, just before the transfer. With nothing tracked anywhere -- the
 * default -- that is one load of a process-wide count and one branch: no thread-local access, no clock, nothing in the frame. With
 * `CoroutineScheduler::set_suspension_tracking(true)` on a thread, each suspension there records, under its FRAME,
 * what it waits on, when (the CPU's counter, `qb::tsc_ticks()`), and the frame it awaits when it awaits a task or a generator.
 * A frame's next suspension replaces its record, and its destruction erases it (the destructor of the `task` and
 * `async_generator` promises, the same branch); `CoroutineScheduler::dump()` reads the records.
 *
 * Measured, before this shape was chosen (Huly QB-71): a `promise_type::await_transform` wrapping every `co_await`
 * would have covered user awaitables too, but costs with tracking off -- the frame keeps the reference it returns,
 * 8 bytes an await site (a parent/child chain's frame 120 -> 136 B, a 64 B pool class higher), and the async mutex
 * paid up to +3.6 %. Per awaiter, the frames and the measurements are the control's. The modules' awaiters, and an
 * awaitable of your own, label themselves with the public `qb::io::async::track_suspension(h, kind)` below; one that
 * does not leaves its coroutine with the record of its previous suspension.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 * @ingroup Coroutine
 */

#ifndef QB_IO_ASYNC_COROUTINE_TRACKING_H
#define QB_IO_ASYNC_COROUTINE_TRACKING_H

#include <atomic>
#include <coroutine>
#include <string_view>
#include <type_traits>

#include <qb/utility/abi.h>
#include <qb/utility/branch_hints.h> // QB_MSVC_FORCEINLINE: the promise destructors that carry track_frame_destroyed

namespace qb::io::async::detail {

/// The process-wide gates in front of the thread-local tests, a cache line of their own -- read on every suspension and
/// every frame destruction, written only when a thread starts or stops tracking or naming.
struct alignas(QB_ABI_CACHELINE_BYTES) suspension_gate {
    std::atomic<int> tracking{0}; ///< threads that track (`on`): the awaiters' gate
    std::atomic<int> frames{0};   ///< threads that track or hold a name (`frames`): the promise destructors' gate
};

/// A transfer awaiter's suspension, left for this thread's recorder coroutine to record (see `track_then`).
struct pending_suspension {
    void                   *frame    = nullptr; ///< the suspending coroutine
    char const             *kind     = nullptr; ///< what it waits on
    void const             *waits_on = nullptr; ///< the coroutine it awaits, or null
    std::coroutine_handle<> next{};             ///< the coroutine the awaiter hands the thread to
};

/// This thread's tracking state. `inline` + QB_ABI_ANCHOR in the class, as `CoroutineScheduler::current_` is: one set
/// of flags and one table per thread across a host and the plugins that link their own copy of qb-io.
struct suspension_tracking {
    /// Read first by each test: zero -- the default -- is one load of a global and one branch, no thread-local access.
    /// That matters on MSVC, which reaches a thread_local through three dependent loads: the destructor's test alone
    /// cost its coroutine spawn cell 5 % (measured). Two counts, so that a name -- given at spawn, tracking off, the
    /// common case -- opens only the destructors' gate, never the awaiters'. A thread sees its own increment in program
    /// order, so a gate never hides a thread's own state; another thread's only sends the test on.
    QB_ABI_ANCHOR static inline suspension_gate gate{};
    /// Suspensions are recorded: the test an awaiter makes, behind `gate.tracking`. This thread counts in
    /// `gate.tracking` exactly while `on` is true.
    QB_ABI_ANCHOR static inline thread_local bool on = false;
    /// A frame's destruction must be seen -- tracking is on, or a coroutine of this thread has a name: the test the
    /// promise destructors make, behind the gate. A name is erased there, never on the scheduler's per-root paths, so a
    /// program that names nothing pays no test for names anywhere (measured: a per-root test in run_ready's drain cost
    /// the coroutine-churn cells 1.5 to 2 %). This thread counts in `gate.frames` exactly while `frames` is true.
    QB_ABI_ANCHOR static inline thread_local bool frames = false;
    /// The records and the names (an opaque `suspension_table`, coroutine/tracking.cpp), allocated on first use.
    QB_ABI_ANCHOR static inline thread_local void *table = nullptr;
    /// The frame of this thread's recorder coroutine (coroutine/tracking.cpp): it exists exactly while `on` is true.
    QB_ABI_ANCHOR static inline thread_local void *recorder = nullptr;
    /// The suspension a transfer awaiter leaves for the recorder, which reads it first thing when it is resumed.
    QB_ABI_ANCHOR static inline thread_local pending_suspension pending{};
};

/// Records a suspension of `frame` (cold, out of line), replacing the frame's previous record.
void note_suspension(void *frame, char const *kind, void const *waits_on) noexcept;
/// Erases the record and the name of `frame`, which is being destroyed.
void forget_frame(void const *frame) noexcept;

/// Names `frame` until it is destroyed (the scheduler's `spawn(name, ...)`); false when the name could not be stored.
bool name_frame(void const *frame, std::string_view name) noexcept;
/// The name given to `frame`, empty when none; valid until the frame goes or this thread names another frame.
[[nodiscard]] std::string_view frame_name(void const *frame) noexcept;

/// Turns this thread's tracking on (allocating its table and calibrating the CPU counter against `qb::mono_now()`,
/// once, ~200 us -- with no usable counter the stamps are `qb::mono_now()`) or off (dropping every record; the names
/// stay). Returns false only when the table could not be allocated (tracking stays off).
bool set_thread_tracking(bool enable) noexcept;

/// One record, as the dump reads it.
struct suspension_view {
    void const *frame;    ///< the suspended coroutine
    char const *kind;     ///< what it waits on
    void const *waits_on; ///< the frame of the coroutine it awaits, or null
    long long   age_ns;   ///< since the suspension, at the time of the call
};

/// This thread's records (empty with tracking off), each with its age. Allocates; call it from the dump only.
template <typename Out>
void for_each_suspension(Out &&out);
/// Out-of-line body of `for_each_suspension`: calls `fn(ctx, view)` for each record.
void visit_suspensions(void *ctx, void (*fn)(void *, suspension_view const &));

template <typename Out>
void
for_each_suspension(Out &&out) {
    visit_suspensions(&out, [](void *ctx, suspension_view const &v) { (*static_cast<std::remove_reference_t<Out> *>(ctx))(v); });
}

/// The awaiters' test: this thread records its suspensions. Behind `gate.tracking`: while no thread tracks -- names or
/// not -- one load of a global and one branch.
[[nodiscard]] inline bool
tracking_on() noexcept {
    return suspension_tracking::gate.tracking.load(std::memory_order_relaxed) != 0 && suspension_tracking::on;
}

/// What an awaiter of qb calls first thing in `await_suspend`: `frame` is the suspending coroutine's address, `kind`
/// the awaiter's `qb_suspension_kind`, `waits_on` the frame of the coroutine it awaits when it awaits one.
inline void
track_suspension(void *frame, char const *kind, void const *waits_on = nullptr) noexcept {
    if (tracking_on()) [[unlikely]]
        note_suspension(frame, kind, waits_on);
}

/// The record of an awaiter that TRANSFERS to another coroutine (it returns `next` from `await_suspend` and resumes
/// nothing inline), written LAST as its cold branch `if (tracking_on()) [[unlikely]] return track_then(..., next);`.
/// It makes no call: it leaves the suspension for this thread's recorder coroutine and transfers to it; the recorder
/// records it and transfers to `next`. A call would put its stack frame on the awaiter's fast path on MSVC, which does
/// not shrink-wrap and tail-calls no function returning a coroutine_handle (measured on task<T>::await_suspend: 6
/// instructions executed without tracking, 21 with a call first, 15 with it last, 10 with this shape).
[[nodiscard]] inline std::coroutine_handle<>
track_then(void *frame, char const *kind, void const *waits_on, std::coroutine_handle<> next) noexcept {
    suspension_tracking::pending = pending_suspension{frame, kind, waits_on, next};
    return std::coroutine_handle<>::from_address(suspension_tracking::recorder);
}

/// What a frame's destruction calls (the destructor of the `task` and `async_generator` promises). Behind
/// `gate.frames`: while no thread tracks or names, one load of a global and one branch.
inline void
track_frame_destroyed(void const *frame) noexcept {
    if (suspension_tracking::gate.frames.load(std::memory_order_relaxed) != 0 && suspension_tracking::frames) [[unlikely]]
        forget_frame(frame);
}

} // namespace qb::io::async::detail

namespace qb::io::async {

/**
 * @brief Says what a coroutine waits on, for `CoroutineScheduler::dump()`: call it first thing in the `await_suspend`
 *        of an awaitable of your own, as every awaiter of qb and of its modules does (since 3.3, Huly QB-71).
 * @param h    the suspending coroutine (`await_suspend`'s parameter)
 * @param kind what it waits on, shown as given: a string that outlives the wait (a literal)
 * @details With nothing tracked anywhere -- the default -- one load of a global and one branch. With tracking on, the
 *          coroutine's record says `kind` from this suspension, and its age starts. An awaitable that does not call it
 *          leaves the coroutine with the record of its previous suspension.
 */
inline void
track_suspension(std::coroutine_handle<> h, char const *kind) noexcept {
    detail::track_suspension(h.address(), kind);
}

} // namespace qb::io::async

#endif // QB_IO_ASYNC_COROUTINE_TRACKING_H
