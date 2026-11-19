/**
 * @file qb/io/async/listener.h
 * @brief Core event loop manager for the asynchronous IO framework
 *
 * This file defines the listener class which serves as the central event loop manager
 * for the asynchronous IO framework. It provides thread-local access to the event loop,
 * registration and management of event handlers, and methods to run the event loop.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
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
 * limitations under the License.
 * @ingroup Async
 */

#ifndef QB_IO_ASYNC_LISTENER_H_
#define QB_IO_ASYNC_LISTENER_H_

#include <algorithm>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <qb/io.h> /* QB_LOG_INFO and qb logging conventions */
#include <qb/system/time.h>
#include <qb/utility/branch_hints.h>
#include <qb/utility/type_traits.h>
#include <thread>
#include <utility>
#include <vector>
#include "event/base.h"
#include "coroutine/scheduler.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

// The sanitizer's poison call, in an AddressSanitizer build only -- detected the way thread_arena.h does it: gcc and
// MSVC define __SANITIZE_ADDRESS__, clang answers __has_feature.
#if defined(__SANITIZE_ADDRESS__)
#define QB_LISTENER_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define QB_LISTENER_ASAN 1
#endif
#endif
#ifdef QB_LISTENER_ASAN
#include <sanitizer/asan_interface.h>
#endif

#if defined(QB_DEBUG_CORO_LIFECYCLE) && QB_DEBUG_CORO_LIFECYCLE
// Standard C++20 __VA_OPT__ elides the comma when no trailing args are passed
// (MSVC needs the conformant preprocessor /Zc:preprocessor, enabled by qb's build).
#define QB_LISTENER_TRACE(fmt, ...) std::fprintf(stderr, "[listener] " fmt "\n" __VA_OPT__(, ) __VA_ARGS__)
#else
#define QB_LISTENER_TRACE(fmt, ...) ((void) 0)
#endif

namespace qb::io::async {

class listener;
namespace detail {
/// INTERNAL (Huly QB-251): the counter rate `loop` calibrated at construction; defined after the class.
[[nodiscard]] inline double tsc_per_ns(listener const &loop) noexcept;
} // namespace detail

/**
 * @class listener
 * @ingroup Async
 * @brief Central event loop manager for asynchronous IO operations.
 *
 * The listener class is the core of the asynchronous event system. It manages
 * an event loop (based on libev) that handles all asynchronous events (IO, timer, signal, etc.)
 * and dispatches them to registered handlers.
 *
 * Each thread has its own listener instance accessible via the thread_local
 * static member 'current'.
 *
 * @note **Thread Safety and Execution Model:**
 *       - Each `listener` instance is **thread-local** and operates in a **single-threaded context**.
 *       - When used with `qb-core`, each `VirtualCore` (worker thread) has its own `listener::current`.
 *       - All I/O objects (clients, servers, sessions) registered with a listener **must not be shared**
 *         between threads. They are designed to be used exclusively within a single VirtualCore.
 *       - This single-threaded execution model eliminates the need for mutexes or atomic operations
 *         for I/O objects within the same VirtualCore, providing inherent thread safety through
 *         isolation rather than synchronization.
 *       - If you need to use qb-io objects across different threads, each thread must have its own
 *         listener instance (via `async::init()`) and objects must not be shared.
 *       - On Windows the libev epoll backend is wepoll (IOCP). The loop must not call
 *         `epoll_wait` / `ev_run` on one thread while another thread closes the same loop or its
 *         epoll handle. Staying on one thread per `listener` satisfies that contract.
 */
class listener {
public:
    /**
     * @brief Thread-local instance of the listener.
     *
     * Each thread has its own listener accessible through this static member.
     * This provides a way to access the current thread's event loop without
     * passing a reference explicitly.
     * @note Nothing initialises it by hand: a `VirtualCore` thread and a standalone `qb-io` thread alike reach it
     *       through this member. Its event loop is built on the FIRST USE THAT NEEDS ONE -- an event registered, a
     *       coroutine that sleeps or waits on a socket, `loop()`, a park -- never before: a thread that uses
     *       nothing of qb builds no loop, on any platform (Huly QB-1246; MSVC constructs every thread_local of a
     *       module at each thread's start, and until 3.4 that was an event loop per thread).
     * @note AFTER ITS DESTRUCTOR -- reached from the destructor of an object with static storage, or of a
     *       thread_local built before it -- every member follows one rule, decided on the thread's record
     *       (`detail::thread_loop()`), never on the destroyed object (Huly QB-1246):
     *       - a REQUEST that needs the loop stops the process on one line that names it: `registerEvent()` (an
     *         io object, a timer, an `async::callback`), `loop()`, `coro_scheduler()`, `arm_wake()`,
     *         `run_once_for()`, and the free `async::run()`, `run_once()`, `run_until()`, `run_for()`, `run_sync()`;
     *       - a CLEANUP, a hand-over or a setting is dropped: `release()` and `unregisterEvent()` of a wrapper
     *         `clear()` detached (freed without reading the listener), `defer()` (the callback is destroyed, never
     *         run), `clear()`, `break_one()`, `disarm_wake()`, `recycle_coro_scheduler()`,
     *         `reset_coro_scheduler()`, `set_io_poll_interval()`, `set_dispatch_timing()`;
     *       - a QUERY answers "nothing": `has_coro_scheduler()`, `has_deferred()`, `is_wake_armed()` false;
     *         `size()`, `total_events_processed()`, `io_poll_interval_ticks()`, `backend()` zero;
     *         `dispatch_timing()` all zero;
     *       - `run()`, `has_work()` and `nb_invoked_event()` carry NO test: a core calls them on every pass, on its
     *         own thread, and is destroyed before its thread's listener. Nor does `wake()`, the one call made from
     *         another thread: its caller knows the wake is armed, which a destroyed listener's never is. In an
     *         AddressSanitizer build the destroyed listener is poisoned and such a touch reported (g++: at `-O0`).
     * @warning **One per thread, not one per thread per image.** The definition is `inline` and
     *          `QB_ABI_ANCHOR`-annotated *below this class*, and both parts are load-bearing —
     *          see the definition site for what regressed when it lived in `listener.cpp`.
     */
    thread_local static listener current;

    /**
     * @class RegisteredKernelEvent
     * @ingroup AsyncEvent
     * @brief Template wrapper for concrete event handlers and their associated libev watchers.
     *
     * This internal class wraps a specific libev event watcher (like `ev::io` or `ev::timer`)
     * and the user-provided actor/handler object. It implements the `IRegisteredKernelEvent`
     * interface.
     *
     * @tparam _Event The event type
     * @tparam _Actor The actor (handler) type
     */
    template <typename _Event, typename _Actor>
    class RegisteredKernelEvent final : public IRegisteredKernelEvent {
        friend class listener;

        _Actor &_actor; /**< Reference to the actor that handles the event */
        _Event  _event; /**< The event object */

        /**
         * @brief Destructor
         */
        ~RegisteredKernelEvent() final {
            _event.stop();
        }

        /**
         * @brief Constructor
         * @param loop Reference to the event loop
         * @param actor Reference to the actor that will handle the event
         */
        explicit RegisteredKernelEvent(ev::loop_ref loop, _Actor &actor) noexcept
            : _actor(actor)
            , _event(loop) {}

        /**
         * @brief Invoke the actor's handler for this event
         *
         * This method is called when the event is triggered. It checks if the
         * actor is still alive (if it provides an is_alive method) and calls
         * the actor's on() method with the event.
         */
        void
        invoke() final {
            // C++20: use concept directly
            if constexpr (qb::has_is_alive<_Actor>) {
                if (likely(_actor.is_alive()))
                    _actor.on(_event);
            } else
                _actor.on(_event);
        }

        void
        stop() noexcept final {
            _event.stop();
        }

        // ---- Thread-local freelist (QB_IO_PLAN 2.13) -----------------------
        // Each RegisteredKernelEvent<E, A> instantiation gets its own
        // thread-local LIFO pool. Because the listener itself is thread-local
        // and every registration is routed through it, allocator contention
        // is zero and the pool size naturally converges to the steady-state
        // number of concurrently registered watchers of this exact type.
        //
        // Freed blocks are re-linked in place using their own first
        // `sizeof(void*)` bytes (which is always ≥ `alignof(void*)` — the
        // class carries two intrusive-list pointers, so the storage is
        // available). No per-block header, no extra allocation.
        //
        // Thread-exit draining (mirrors detail::CoroutineFrameAllocator): the
        // pool is a thread_local RAII holder whose destructor returns every
        // parked block to the global allocator at thread termination. The main
        // thread keeps its pool reachable until process exit, but a *joined
        // worker* thread abandons its TLS — without draining, any block parked
        // here at exit (e.g. a watcher freed during `listener::clear()`) is lost
        // and reported as a leak. A separate `_pool_alive()` guard (a trivially
        // destructible bool that outlives the holder in TLS-teardown order) lets
        // an `operator delete` that runs *after* the holder was destroyed —
        // exactly the case when `~listener` frees a still-registered watcher,
        // its own freelist TLS having been torn down first — bypass the dead
        // free-list and go straight to the global allocator instead of pushing
        // onto destroyed storage. This is what makes the teardown delete free to
        // the OS rather than re-leak the block.
        QB_ABI_ANCHOR static bool &
        _pool_alive() noexcept {
            thread_local bool alive = false;
            return alive;
        }

        struct FreeList {
            void *head = nullptr;
            FreeList() noexcept {
                _pool_alive() = true;
            }
            ~FreeList() noexcept {
                _pool_alive() = false;
                void *p       = head;
                while (p) {
                    void *next = *static_cast<void **>(p);
                    ::operator delete(p, std::align_val_t{alignof(RegisteredKernelEvent)}); // UNSIZED on purpose: see qb/io/async/io.h
                    p = next;
                }
                head = nullptr;
            }
        };

        QB_ABI_ANCHOR static FreeList &
        _freelist() noexcept {
            thread_local FreeList fl;
            return fl;
        }

    public:
        static void *
        operator new(std::size_t sz) {
            // `operator new` is only ever reached from a running listener (a live
            // `registerEvent`), never during teardown, so the pool is always in
            // its alive window here — touching `_freelist()` also (re)constructs
            // the holder and arms `_pool_alive()`.
            auto &fl = _freelist();
            if (fl.head) {
                void *p = fl.head;
                fl.head = *static_cast<void **>(p);
                return p;
            }
            // Honour the type's alignment: a watcher may capture a by-value event
            // (cache-line aligned) in its callback closure; plain ::operator new
            // only guarantees 16 B, which under-aligns it and SIGSEGVs on the
            // aligned AVX moves the optimiser emits under -march=native.
            return ::operator new(sz, std::align_val_t{alignof(RegisteredKernelEvent)});
        }

        static void
        operator delete(void *p) noexcept {
            if (!p)
                return;
            // A delete that runs after this thread's pool was torn down (e.g.
            // `~listener` freeing a still-registered watcher, the freelist TLS
            // having destructed first) must not chain onto the dead free-list.
            if (!_pool_alive()) {
                ::operator delete(p, std::align_val_t{alignof(RegisteredKernelEvent)});
                return;
            }
            auto &fl                 = _freelist();
            *static_cast<void **>(p) = fl.head;
            fl.head                  = p;
        }

        // C++14 sized-delete: forward to the unsized version so the pool
        // logic always applies (sizes are identical for a given instantiation).
        static void
        operator delete(void *p, std::size_t) noexcept {
            RegisteredKernelEvent::operator delete(p);
        }
    };

private:
    /// The loop this listener owns: created by `build()`, destroyed with the listener. Null until it is built --
    /// the constructor of a listener of one's own builds it, the thread's `listener::current` on its first use.
    struct owned_loop : ev::loop_ref {
        owned_loop() noexcept
            : ev::loop_ref(static_cast<struct ev_loop *>(nullptr)) {}
        owned_loop(owned_loop const &)            = delete;
        owned_loop &operator=(owned_loop const &) = delete;
        ~owned_loop() {
            if (raw_loop)
                ev_loop_destroy(raw_loop);
        }
        void
        build(unsigned int const flags) {
            raw_loop = ev_loop_new(flags);
            if (!raw_loop)
                throw ev::bad_loop();
        }
    };

    owned_loop _loop; /**< The libev event loop */
    // The loop's own "is there anything to do" counters, read INLINE on every pass a core
    // takes through `has_work()` / `run()`: the referenced-active count `ev_run`'s loop
    // condition consults, and the per-priority pending counts (`EV_NUMPRI` entries). Two
    // addresses into the loop, taken once and valid for its lifetime (qev
    // `ev_active_count_addr` / `ev_pending_count_addr`); the calls they replace were two
    // out-of-line trips into libqev plus a loop over the priorities, ~10 % of a one-event
    // core pass once nothing else in the pass read a clock (Huly QB-182). `_active_count`
    // is the RAW counter — it reads -1 between an `unref()` and the start it pairs with,
    // which is why `_loop_has_work()` tests it with `> 0`, exactly as `ev_active_count()`
    // clamps it.
    const int *_active_count;  ///< `&activecnt` of `_loop`
    const int *_pending_count; ///< `pendingcnt[EV_NUMPRI]` of `_loop`

    // ---- The io cadence (Huly QB-191) ------------------------------------------------
    // A quiet fd polled on every pass costs the backend's syscall for nothing -- `epoll_wait(0)`
    // ~80 ns on WSL2, wepoll's `GetQueuedCompletionStatusEx` ~250 ns -- on every pass of every
    // core that owns a socket: measured, an io pass was 101 ns against 22 for a timers-only one
    // once qev's pass reached its floor (QB-188), and every one of those passes found nothing.
    // The poll runs on a cadence when an embedder asks for one. It runs on every pass while
    // the loop is HOT -- the previous non-blocking pass delivered something, so a burst stays
    // at poll latency -- and otherwise once per `io_poll_interval` (a `VirtualCore` sets 1 µs
    // by default, `CoreInitializer::setIoPollInterval`; the listener's own default is zero =
    // every pass, the 3.1 contract for a program driving `run(EVRUN_NOWAIT)` itself), measured
    // on the CPU's own counter
    // (`qb::tsc_ticks`, ~5-7 ns, calibrated once against `mono_now()` at construction). A pass
    // in between runs `ev_run` with `EVRUN_NOPOLL`, which reifies timers and periodics and
    // invokes pending events exactly as before and only leaves the backend call out. What a
    // quiet socket's owner pays: at most one interval plus a pass before its byte is seen;
    // what every other pass saves: the syscall. A blocking pass (`run_once_for`, `run(0)`)
    // always polls -- there the poll is the wake.
    const int          *_io_count;              ///< `&iocnt` of `_loop`: the fds a poll is for, the loop's own excluded (QB-1277)
    const unsigned int *_io_fed;                ///< `&iofed` of `_loop`: ready fds the backend reported so far
    unsigned int        _io_fed_seen    = 0;    ///< `*_io_fed` after the last non-blocking pass; a difference = hot
    std::uint64_t       _last_poll_tsc  = 0;    ///< `qb::tsc_ticks()` at the last backend poll of a non-blocking pass
    std::uint64_t       _poll_every_tsc = 0;    ///< the interval in ticks; 0 = poll on every pass
    double              _tsc_per_ns     = 0.;   ///< the calibration; 0 = no usable counter, poll every pass
    bool                _io_hot         = true; ///< the previous non-blocking pass's poll reported a ready fd: poll again at once

    // ---- The pass without the loop (Huly QB-190) -----------------------------------------
    // What a non-blocking pass still paid once the poll was on a cadence was `ev_run` itself:
    // its clock read, the timer heap, the pending walk, ~22 ns on g++ (`bench-pass` `timer`),
    // ~31 on MSVC -- on every pass of every core that holds one far timer (a `sleep`, a retry,
    // a keep-alive) or one quiet socket. The loop has nothing to do on such a pass, and it can
    // now be asked, inline: `*_pending_count` (an event fed since the last pass), the wake flag
    // (`ev_wake_pending_addr`: an `ev_async_send` from another thread that found no park to
    // write to), `*_timer_count` / `*_periodic_count` and `ev_timer_next()` (the earliest deadline
    // of either, on the loop's clock). A pass that has no event pending, no wake, no poll to make
    // and no timer within reach does not call the loop at all. "Within reach" is judged on the CPU's counter, as
    // the io cadence is: the listener anchors a reading of the loop's clock (`ev_clock_now`)
    // against `tsc_ticks()` whenever it makes one, and estimates now as the anchor plus the
    // counter's advance -- with a rate margin of 0.1 % for the calibration and the kernel's
    // slewing of the monotonic clock, a slack of 2 ms, and a fresh anchor every second at most
    // -- so a deadline beyond the estimate is not due and costs no clock read; one within it
    // costs the precise read, which is then HANDED to the loop (`ev_now_set`) so the pass that
    // fires the timer reads no clock of its own: one read per pass, whoever needs it. A timer
    // is never judged against the estimate, only against a real reading -- the estimate only
    // decides whether to make one -- so precision is the clock's, as before.
    // (the fields of this gate are declared with the park's, at the end of the data members: see there)

    static constexpr double kTimerBoundSlackS  = 2e-3; ///< the estimate's slack before a deadline counts as within reach
    static constexpr double kTimerBoundMarginR = 1e-3; ///< the rate margin on the counter's advance
    static constexpr double kTimerReanchorS    = 1.;   ///< an anchor older than this is not trusted: read

    /// Whether the earliest timer is due, on a real reading of the loop's clock -- made only when
    /// the counter-based estimate says the deadline is within reach (see the block above). A
    /// reading made here is kept for the loop (`_supply`). Called with a timer or a periodic active:
    /// `ev_timer_next` reads both heaps (Huly QB-275) and, with a periodic, the loop's next wall-clock read (QB-1277).
    [[nodiscard]] bool
    _timer_due(std::uint64_t const tsc) noexcept {
        const double next = _loop.timer_next();
        if (_have_anchor && _tsc_per_ns > 0.) {
            const double elapsed = static_cast<double>(tsc - _anchor_tsc) / _tsc_per_ns * 1e-9;
            if (elapsed < kTimerReanchorS && _anchor_loop_s + elapsed * (1. + kTimerBoundMarginR) + kTimerBoundSlackS < next)
                return false;
        }
        const double now = ev_clock_now();
        _anchor_tsc      = tsc;
        _anchor_loop_s   = now;
        _have_anchor     = true;
        _supply_loop_s   = now;
        _supply          = true;
        return qb::detail::ev_deadline_passed(next, now); // strictly past: the loop's own rule (Huly QB-1016)
    }

    /// The gate of a non-blocking pass: whether it runs the loop at all, and with which flags (the
    /// io cadence adds `EVRUN_NOPOLL`; a timer within reach leaves a reading in `_supply`). OUT OF
    /// LINE on purpose: `run()` is inlined into every core's pass, and this gate's code -- a
    /// counter read, a call into the loop, double arithmetic -- inlined with it cost MSVC's `push`
    /// round trip +2 ns (31 → 33, five alternations, fully separated) on a core whose pass never
    /// enters the gate, through what the compiler did to the pass around it; as a call it is a
    /// few loads on the passes that take it and nothing on the others (measured level).
    [[nodiscard]] QB_NOINLINE bool
    _nowait_gate(int &f, bool const pending) noexcept {
        // The io cadence (see `_io_count`): a non-blocking pass over a loop with an fd polls only
        // when hot or due; `EVRUN_NOPOLL` keeps everything else of the pass. A hot pass stamps its
        // poll too, so the interval counts from the LAST poll, whichever rule ran it. One counter
        // read serves the cadence and the timer gate.
        std::uint64_t tsc  = 0;
        bool          poll = *_io_count > 0;
        if (poll && _poll_every_tsc != 0) {
            tsc = qb::tsc_ticks();
            if (_io_hot)
                _last_poll_tsc = tsc;
            else if (tsc - _last_poll_tsc < _poll_every_tsc) {
                poll = false;
                f |= EVRUN_NOPOLL;
            } else
                _last_poll_tsc = tsc;
        }
        // The pass without the loop (see `_timer_count`): nothing pending, no wake, no poll to
        // make and no timer within reach -- the loop has nothing to do, and the pass does not call
        // it. A timer within reach is judged on a real reading, which the loop is then handed
        // instead of making its own.
        if (poll || pending || _wake_is_pending() != 0)
            return true;
        // A deadline is a timer's or a periodic's, in two heaps `ev_timer_next` reads both of: the
        // gate counted timers only, so a periodic alone on the loop never fired under non-blocking
        // passes, and one beside a quiet socket fired at the cadence's next poll (Huly QB-275).
        if ((*_timer_count | *_periodic_count) == 0)
            return false;
        if (_tsc_per_ns <= 0.) // no usable counter: the loop's own read is the cheaper judge
            return true;
        if (tsc == 0)
            tsc = qb::tsc_ticks();
        return _timer_due(tsc);
    }

    // ---- The counter's rate (Huly QB-1013) ---------------------------------------------------
    // Every io interval above is converted into ticks with it, and `_timer_due` turns the counter's
    // advance back into time with it, so its error is theirs. It was read as two pairs -- the
    // clock, then the counter, at each end of the window -- and a pair takes whatever ran between
    // its two reads (an interrupt, a preemption, a VM exit) for elapsed ticks or elapsed time,
    // multiplied by the window over the delay. Measured with a 1 ms sleep injected between the
    // closing pair's reads (WSL2, g++-14): the counter read 6.3-6.5 times too fast, every io
    // interval as many times longer and the estimate as many times short, so a 100 ms timer on a
    // core that never parks fired after 616-634 ms (885-1030 ms on MSVC, whose 1 ms sleep lasts
    // longer). Between the opening pair's reads the error goes the other way: a rate near zero,
    // an interval of nothing, a clock read on every pass. `qb::detail::calibrate_tsc_per_ns`
    // reads each end's clock between two counter reads and keeps the tightest of five such
    // brackets, so the delay lands in a bracket that is not kept, for 0.2-0.3 µs on top of the
    // 200 µs spin. The suspension tracker calibrates with the same function (`coroutine/tracking.cpp`).

    /// Calibrate `tsc_ticks()` against `mono_now()` over ~200 µs, once per listener (a fixed
    /// tick rate is assumed: an invariant TSC on x86, the architected counter on arm64; the
    /// portable fallback of `tsc_ticks()` IS a nanosecond clock and calibrates to 1), each end
    /// of the window bracketed (see the block above). A zero rate -- a counter that does not
    /// move -- leaves the cadence polling on every pass and the timer gate handing every pass
    /// that holds a timer to the loop.
    void
    _calibrate_tsc() noexcept {
        _tsc_per_ns = qb::detail::calibrate_tsc_per_ns();
    }

    /// The loop's own liveness rule, read inline: a referenced active watcher, or a pending event
    /// at any priority. What `ev_run` would find something to do for — and nothing else.
    [[nodiscard]] bool
    _loop_has_work() const noexcept {
        return *_active_count > 0 || _loop_has_pending();
    }

    /// Any pending event at any priority — non-zero exactly when `ev_pending_count()` would be.
    /// Branchless on purpose (Huly QB-257): the counts are OR-ed and tested once. The early-exit loop
    /// it replaced was five compares and five branches on exactly the passes that found nothing --
    /// every idle pass of every core, and twice on a pass that ran the loop (`has_work()`, then
    /// `run()`), both inlined into the core's pass.
    [[nodiscard]] bool
    _loop_has_pending() const noexcept {
        int pending = 0;
        for (int pri = 0; pri < EV_NUMPRI; ++pri)
            pending |= _pending_count[pri];
        return pending != 0;
    }

    // ---- Registered events (QB_IO_PLAN 2.20) --------------------------------
    // Intrusive doubly-linked list of every `IRegisteredKernelEvent *` this
    // listener currently owns. Was a `qb::unordered_set<void *>` previously,
    // which cost one extra heap allocation + one hash computation per
    // `registerEvent`/`unregisterEvent`. Since the listener is strictly
    // single-threaded (thread-local) and every registration is already backed
    // by a heap-allocated handler, piggy-backing the list links on the
    // interface is optimal — insertion and removal become a handful of
    // pointer swaps, and membership tests disappear entirely.
    IRegisteredKernelEvent *_registered_head  = nullptr;
    std::size_t             _registered_count = 0;

    // ---- Re-entrant-dispatch guard (double-free safety for clear()) ----------
    // A loop-owned, self-deleting handler (e.g. an `async::callback` Timeout) frees itself with
    // `delete this` at the end of its own `invoke()`. If that handler's body calls `clear()`,
    // clear() would otherwise also destroy it via `_destroy_owner` — freeing it once, then the
    // in-flight `delete this` frees it again (double-free / use-after-free in `invoke()`).
    // `on()` threads the handlers whose `invoke()` is currently on the call stack through these
    // stack-local nodes (innermost first); `clear()` consults the chain and never destroys a
    // mid-dispatch handler — that handler reclaims itself when `invoke()` returns. The node is a
    // plain stack object, so the cost per dispatch is two pointer writes (no heap, no growth),
    // and nested dispatch is handled naturally.
    struct DispatchNode {
        IRegisteredKernelEvent *iface;
        DispatchNode           *prev;
    };
    DispatchNode *_dispatch_top = nullptr;

    [[nodiscard]] bool
    _is_dispatching(IRegisteredKernelEvent const *e) const noexcept {
        for (auto const *n = _dispatch_top; n != nullptr; n = n->prev)
            if (n->iface == e)
                return true;
        return false;
    }

    std::size_t _nb_invoked_events      = 0; /**< Counter for the number of invoked events */
    std::size_t _total_events_processed = 0; /**< Total number of events processed since listener creation */

    // Coroutine support
    std::unique_ptr<class CoroutineScheduler> _coro_scheduler; /**< Coroutine scheduler */

    // ---- Deferred callbacks (next-tick post queue) --------------------------
    // Callbacks queued via `async::defer()`: each runs once, at the tail of the
    // same `run()` iteration — right after every libev watcher for this turn has
    // returned. This is the correct primitive for "continue after this event
    // handler unwinds", most importantly a handler that must destroy or replace
    // the very object it is running on (a reconnect = destroy+recreate a
    // connection). Unlike `callback(fn)` it never runs `fn` inline; unlike
    // `callback(fn, delay)` it needs no libev timer, no heap `Timeout`, and no
    // magic delay. It is the non-coroutine twin of `co_await sleep(0ms)`'s
    // cooperative yield (mirrors the coroutine scheduler's `ready_queue_`).
    //
    // A TURN is one drain: it runs the callbacks queued before it began, in order, and one a
    // callback defers is the next turn's. Drains NEST (Huly QB-1246): a callback being drained that
    // runs the loop -- `run_sync()`, `run_until()`, a module's `await()` -- opens a turn inside its
    // own, whose drain takes everything queued at that moment, what is left of the enclosing drain's
    // snapshot included, the callbacks executing excepted (they are off the queue). The enclosing
    // drain then runs only what is LEFT of its snapshot: a drain is bounded by the count of entries
    // TAKEN off the queue (`_deferred_taken`), whoever took them, never by a count of its own pops --
    // so it cannot run a callback queued after it began, and `clear()`, which counts what it drops,
    // ends every drain on the stack. Until 3.4 a nested `run()` drained nothing, and a callback that
    // waited by pumping the loop for something delivered through `defer()` -- a connect refused at
    // once is (`tcp::connector`) -- span for ever: the queue was never empty, so the pump never
    // parked, and never drained, so it was never empty.
    std::deque<std::function<void()>> _deferred;
    bool                              _defer_held    = false; ///< `clear()` is running: no drain runs, `defer()` feeds no hook
    bool                              _hook_drains   = false; ///< the in-loop hook is draining, nothing ended under it: `defer()` feeds no hook
    bool                              _break_run     = false; ///< a run that only a break ends is on the stack (`_run_until_broken`)
    bool                              _break_asked   = false; ///< a break was asked of that run, which has not ended yet
    std::uint32_t                     _blocking_runs = 0;     ///< blocking `ev_run`s on the stack (`_run_blocking`): the hook's runs

    // ---- In-loop drain hook -------------------------------------------------
    // Draining only *after* `_loop.run()` returns is not enough: a blocking
    // `run()` (flag 0 — `qb::io::async::run()`, the documented standalone main
    // loop) never returns while any watcher is active, so a `defer()` posted from
    // a handler would be starved forever. That is reachable from framework code on
    // a public API path (`tcp::connector::deliver_failure_deferred()`, qbm-http's
    // reconnect), so the queue must also be serviced from INSIDE the loop.
    //
    // `_defer_wake` is that hook. It is normally never started, so it holds no
    // reference on the loop and never perturbs `activecnt` or the poll timeout;
    // `defer()` merely FEEDS it an event. libev drains `pendings` from the highest
    // priority downwards and this watcher sits at `EV_MINPRI` (every qb watcher
    // stays at the default priority), so its callback runs after every other
    // watcher pending in the same iteration — precisely `defer()`'s "tail of the
    // current turn, never re-entrant from inside a handler" contract.
    //
    // It serves a BLOCKING run only (`run(0)`, `EVRUN_ONCE`: `_blocking_runs`), the one
    // that may not return to its tail drain. A non-blocking run is one turn and drains
    // once, at its tail, after every watcher of the call: libev invokes what is pending
    // on ENTRY to `ev_run` too, so a hook fed between two calls (a `disconnect()` feed,
    // a defer from a handler run at entry) drained there, and its re-arm below fired in
    // the same call's iteration -- two generations in one `run(EVRUN_NOWAIT)` (Huly
    // QB-448). `defer()` feeds the hook only under a blocking run, and the hook stands
    // down when a stale re-arm fires in a non-blocking one.
    //
    // Leftovers (a callback that itself defers — excluded from this pass by the
    // snapshot bound) must NOT be re-fed: `ev_feed_event` lands in the pass that is
    // still draining and would run them in the same turn. They are re-armed with a
    // 0-delay one-shot instead, which expires on the next loop iteration, and until it
    // fires no drain takes them (`_drain_deferred`).
    //
    // `defer()` FEEDS the hook under a blocking run, so that nothing queued waits on the poll of a
    // blocking loop -- a run that only a break ends has no tail drain to count on. One caller is
    // excepted, the one the re-arm above is for: a callback this hook is draining (`_hook_drains`),
    // whose feed would only be a second invocation in the same pass, taking nothing (the re-arm is
    // armed by then). The mark saves that invocation and nothing else, so it errs one way only: the
    // hook raises it around its drain, and what ends under that drain leaves it lowered -- a blocking
    // run OPENED from such a callback, which is another loop whose handlers must feed
    // (`_run_blocking`), a hook that fired inside a pump of such a callback. Lowered early it costs
    // the invocation it was saving; raised when it should not be, a handler's callback would wait on
    // the poll. Until 3.4 the feed was skipped whenever a drain was on the stack, any drain: a
    // blocking run opened from a callback being drained never served what its own handlers deferred
    // (Huly QB-1246).
    ev::timer _defer_wake;

    void
    _on_defer_wake(ev::timer &, int) {
        if (_blocking_runs == 0)
            return; // a non-blocking turn: its tail drain takes the queue
        // A callback of this drain that defers has the re-arm below, and feeds nothing (`defer()`). The mark is
        // lowered after the drain, never put back: a feed too many is an invocation that takes nothing.
        _hook_drains        = true;
        const std::size_t n = _drain_deferred();
        _hook_drains        = false;
        _nb_invoked_events += n;
        _total_events_processed += n;
        if (!_deferred.empty() && !_defer_wake.is_active())
            _defer_wake.start(0.); // next turn, never this pass
        if (_break_asked) [[unlikely]]
            _say_break_again(); // a callback of this drain pumped: the loop forgot it (`_run_until_broken`)
    }

    /// A blocking `ev_run` (`run(0)`, `EVRUN_ONCE`): the hook's runs. What was deferred before the
    /// call is handed to the hook on entry -- the loop may block in its first poll, and only the hook
    /// drains inside it; a blocking turn opened from a callback being drained hands it over likewise
    /// (it used to keep it for the drain it was called from, and wait). Out of line: a core's pass
    /// inlines `run(EVRUN_NOWAIT)`, where this is dead.
    /// The count is 32 bits wide: a byte read zero with 256 blocking turns on the stack, and the hook
    /// stood down in the 256th; four thousand million of them do not fit on a stack.
    QB_NOINLINE void
    _run_blocking(int const flags) {
        struct Depth {
            std::uint32_t &runs;
            ~Depth() {
                --runs;
            }
        } const depth{++_blocking_runs};
        _hook_drains = false; // another loop, were it opened from a callback the hook is draining: its handlers feed
        if (!_deferred.empty() && !_defer_held && !_defer_wake.is_pending() && !_defer_wake.is_active())
            _defer_wake.feed_event(EV_CUSTOM);
        if (flags & EVRUN_ONCE) {
            _loop.run(flags); // one iteration: it ends by itself, a break has nothing to end there
            if (_break_asked) [[unlikely]]
                _say_break_again(); // a pump's park: the loop forgot it as the park began
        } else {
            _run_until_broken(flags);
        }
    }

    // ---- A break, kept for the run it is for (Huly QB-1246) -----------------------------------
    // `break_one()` is for the innermost run that only a break ends: `run()` with no flag. The loop
    // itself keeps a break for the `ev_run` in progress only: every `ev_run` clears it as it begins,
    // and a pump begins one at every pass -- a non-blocking pass, an `EVRUN_ONCE` park: `run_until()`,
    // `run_sync()`. A break asked while a handler or a callback of the run was pumping, or before one
    // pumped in the same iteration, was lost at the pump's next pass, and the run never ended. A
    // handler could always ask it there. Since a nested turn drains the deferred queue a deferred
    // callback can too: one queued behind the callback that pumps ran after it until then, in the
    // run's own drain, where its break was heard.
    //
    // So the run KEEPS the request (`_break_asked`, while `_break_run` says such a run is on the
    // stack), and it is said to the loop again wherever a pump may have made it forget -- nowhere in
    // what a core's pass inlines; a park pays one test (`run_once_for()`, `_run_blocking`):
    //  - when the in-loop drain has run its callbacks (`_on_defer_wake`): a callback that pumped;
    //  - when an `EVRUN_ONCE` turn returns (`_run_blocking`): a pump's park;
    //  - when a run of this kind opened under this one returns (below): its own request is its own,
    //    kept apart, and the outer one's is handed back and said again;
    //  - when a pump of the library returns (`relay_break()`: `async::run(flag)`, `run_until()`,
    //    `run_for()`, `run_sync()`, the non-blocking pass of `run_once_for()`), which covers the passes
    //    of a pump that a watcher's handler runs, and after the passes `clear()` runs. A pump written
    //    by hand over `run(EVRUN_NOWAIT)` calls it too.
    // Two conditions hold at every one of those points (`_say_break_again`). It is said AGAIN only at
    // the loop depth of the run that keeps it (`_break_depth`): under a handler or a callback that run
    // itself invoked, where it ends the run when that one returns -- between two passes of a pump of
    // theirs included, where it holds until the next pass begins and the pump says it again as it
    // returns. Anywhere deeper is a pass of a pump, and a break there ends that pass and nothing else
    // -- BEFORE IT POLLS, when what says it is invoked as the pass begins: the in-loop drain fed
    // between two passes is, and so is whatever a callback it runs there ends with -- a pump with
    // nothing left to wait for, an `EVRUN_ONCE` turn or a run of its own that ended at once. Said
    // there at every pass, the break kept the pump from ever polling again. The pump that runs the
    // pass relays in its turn, once it has returned to the run. And it is said only when the loop
    // holds no break at that moment (`ev_break_state()`): what it still holds is what was asked -- or
    // more than that, a `break_loop(ev::ALL)` asked since -- and is left as it is.
    // `break_one()` itself is not held to the depth: asked under a pass, it ends that pass, as it
    // always did -- a break ends the `ev_run` it is asked in, the loop's own rule -- and the run keeps
    // it besides.
    QB_NOINLINE void
    _run_until_broken(int const flags) {
        struct Run {
            listener          &self;
            unsigned int const outer_depth;
            bool const         outer_run;
            bool const         outer_asked;
            ~Run() {
                self._break_depth = outer_depth;
                self._break_run   = outer_run;
                self._break_asked = outer_asked;
                if (outer_asked)
                    self._say_break_again(); // the outer run's, which this one made the loop forget as it began
            }
        } const run{*this, std::exchange(_break_depth, _loop.depth() + 1), std::exchange(_break_run, true), std::exchange(_break_asked, false)};
        _loop.run(flags);
    }

    /// The kept break, said to the loop again: only from the `ev_run` of the run that keeps it, and only when the
    /// loop holds no break -- a pump made it forget.
    QB_NOINLINE void
    _say_break_again() noexcept {
        if (_loop.depth() == _break_depth && _loop.break_state() == EVBREAK_CANCEL)
            _loop.break_loop();
    }

    // ---- Park inside the loop (qb-core's `Mailbox::wait(listener &)`) -------------
    // A `VirtualCore` that owns io watchers used to park in a condition variable, and
    // while it slept nothing polled its loop: a socket that became readable waited for
    // the core's `latency` timeout (measured: p50 0.9–1.2 ms at 100 µs latency, 15.6 ms at
    // 10 ms, against 19 µs while polling — qb-vs-others audit, axis N). `run_once_for()`
    // parks in `ev_run(EVRUN_ONCE)` instead, so io wakes the core at poll latency, and
    // `_wake` is how a PRODUCER on another thread ends that park after enqueuing an
    // event: `ev_async_send` is libev's one thread-safe entry point, and the loop's own
    // `pipe_write_wanted` / `pipe_write_skipped` fences make it airtight against the poll
    // (a send landing before the poll is fed by the pass itself; one landing after the
    // last pass is drained by the next `ev_run` before it can block). The watcher is
    // `unref()`-ed once started so it never counts as work in `has_work()` / `run()`,
    // and armed lazily — a spinning or actor-only core never creates the wake pipe.
    ev::async _wake;
    ev::timer _park_cap;
    bool      _wake_armed      = false;
    bool      _scheduler_dying = false; ///< a reset or a `recycle()` is busy with its frames: a nested one is a no-op

    // ---- The pass without the loop, its fields (see the block above `_timer_due`). Declared LAST
    //      on purpose: `has_work()` reads `_deferred` and `_coro_scheduler` on every pass of every
    //      core, and 48 bytes inserted before them read +2 ns on MSVC's `push` (measured, five
    //      alternations, fully separated); after them, level.
    const int         *_timer_count;    ///< `&timercnt` of `_loop`
    const int         *_periodic_count; ///< `&periodiccnt` of `_loop` (`kNoPeriodics` when they are compiled out)
    const EV_ATOMIC_T *_wake_pending;   ///< `&pipe_write_skipped` of `_loop`: a cross-thread wake no pass delivered

    static constexpr int kNoPeriodics = 0; ///< the periodic count of a loop built without them

    // What the counters above read while this listener has no loop yet (the thread's, before its first use that
    // needs one): nothing active, nothing pending, no fd, no timer, no wake. `has_work()` and `run()` are then
    // right as they are -- no test of their own on the pass every core takes.
    static constexpr int          kNoCounts[EV_NUMPRI] = {};
    static constexpr unsigned int kNoIoFed             = 0;
    static constexpr sig_atomic_t kNoWake              = 0;

    // `_wake_pending` (qev's `pipe_write_skipped`) is written by a PRODUCER thread's `ev_async_send`
    // (Huly QB-192 made that store a `__atomic` release); this inline read is on the owner thread, so
    // it must be an atomic acquire too or ThreadSanitizer flags the pair once ev.c is instrumented.
    // Other inline reads in the gate (`_io_fed`, `_active_count`, `_timer_count`, `_periodic_count`) are owner-thread
    // only -- written by `ev_run` on this same thread -- so they stay plain. gcc/clang only; the MSVC
    // read is plain (no TSan there), matching qev's own `#else`.
    [[nodiscard]] int
    _wake_is_pending() const noexcept {
#if (defined(__GNUC__) || defined(__clang__)) && !defined(EV_NO_ATOMIC_BUILTINS)
        return __atomic_load_n(_wake_pending, __ATOMIC_ACQUIRE);
#else
        return *_wake_pending;
#endif
    }
    std::uint64_t _anchor_tsc    = 0;  ///< `tsc_ticks()` at the last reading of the loop's clock
    double        _anchor_loop_s = 0.; ///< that reading, on the loop's clock (seconds)
    double        _supply_loop_s = 0.; ///< a reading made for THIS pass, handed to the loop before it runs
    bool          _have_anchor   = false;
    bool          _supply        = false;

    // ---- The drains on the stack (Huly QB-1246). After everything a pass reads, like the block above:
    //      only a drain that finds a callback queued, `defer()` under a blocking run and `clear()` come here.
    /// One drain on the stack. `~listener` marks every one of them: a listener destroyed by a callback it is
    /// draining is found out when that callback returns, by a frame that is still there to say so.
    struct DrainFrame {
        DrainFrame *outer;
        bool        listener_gone = false;
    };
    std::uint64_t _deferred_taken = 0;       ///< entries ever taken off `_deferred`, run or dropped: the head's sequence number
    DrainFrame   *_drain_top      = nullptr; ///< the innermost drain on the stack
    unsigned int  _break_depth    = 0;       ///< the loop's depth inside the innermost run that only a break ends

    void
    _on_wake(ev::async &, int) noexcept {}
    void
    _on_park_cap(ev::timer &, int) noexcept {}

    // Run every callback queued BEFORE this pass; one that itself defers is left
    // for the next loop turn (the snapshot bounds the drain and stops a
    // self-re-deferring callback from starving the loop). The one test the pass pays is
    // the queue's: a drain already on the stack is no reason to refuse this one (see `_deferred`).
    std::size_t
    _drain_deferred() {
        if (_deferred.empty())
            return 0; // branch-only fast path when nothing is queued
        return _drain_queued();
    }

    // The drain itself, OUT OF LINE: `run()` is inlined into every core's pass, and this body
    // inlined with it -- a deque walk, a call through `std::function`, an exception scope -- was
    // code the pass carried for the passes that find a callback queued; its register pressure
    // spilled the pass around it, and any change to it moved the actor-only pass (+2 instructions a
    // push round trip under callgrind, g++-14). As a call it costs those passes one call.
    [[nodiscard]] QB_NOINLINE auto
    _drain_queued() -> std::size_t {
        // The queue's head is the next turn's while the in-loop drain's 0-delay re-arm is pending
        // (see `_defer_wake`): `run()`'s own drain, after a blocking loop returned in the same call,
        // is not where it runs (Huly QB-448). And nothing is drained while `clear()` runs: what its
        // passes find queued it drops.
        if (_defer_wake.is_active() || _defer_held)
            return 0;
        // This drain's frame, taken off the stack below -- nothing between here and there throws: a callback's
        // exception is contained, and what the loop does with the queue cannot (`std::function`'s move, the
        // deque's `pop_front`). The one way out that leaves it is a listener its own callback destroyed, which
        // returns nowhere.
        DrainFrame frame{_drain_top};
        _drain_top = &frame;
        // The snapshot, as a bound on the count of entries TAKEN off the queue: by this drain, by a drain one of
        // its callbacks opens (it takes what is left of this snapshot, and more), or by a `clear()` one of them
        // calls (it drops the rest of it, and what is deferred after the clear is the next turn's, Huly QB-276).
        // Past the bound nothing of the snapshot is left, whoever took it.
        std::uint64_t       taken   = _deferred_taken;
        const std::uint64_t end     = taken + _deferred.size();
        std::size_t         drained = 0;
        while (taken < end) {
            {
                std::function<void()> fn = std::move(_deferred.front());
                _deferred.pop_front();
                _deferred_taken = taken + 1; // before the call: the head's number whenever a callback runs
                if (fn) {
                    // Runs inside the loop: an escaping exception would unwind through
                    // libev's C frames (UB). Contain it, exactly as `Timeout` does.
                    try {
                        fn();
                    } catch (...) {
                    }
                    ++drained;
                }
            } // what the callable captured goes HERE, under this frame: its destructors are user code as its body was
            // A listener destroyed by a callback it was draining -- or by what that callback captured -- has
            // nothing left for this drain to read, and the `run()` it returns to reads its members too.
            if (frame.listener_gone) [[unlikely]]
                detail::refuse_listener_destroyed_under_its_drain();
            taken = _deferred_taken; // read again: whoever took entries meanwhile, the bound is on the count
        }
        _drain_top = frame.outer;
        return drained;
    }

    // Drop every deferred callback without running it (`clear()`). By SWAPPING the queue out,
    // never `_deferred.clear()` in place: releasing a closure's captured state runs arbitrary
    // destructors -- a captured shared_ptr can be the last reference to an object whose teardown
    // calls `defer()` again -- and a push_back into a deque halfway through its own clear() is
    // undefined. After the swap the member is empty, so such a re-entrant defer lands in a fresh
    // queue, which the loop drops in turn -- sixteen turns at most: a closure whose destructor defers again at
    // every turn kept this loop running for ever (Huly QB-1246). What the sixteenth turn leaves is given up:
    // never run, and never destroyed either (leaked), so that nothing defers again.
    // What leaves the queue here is COUNTED as taken, like what a drain runs: that is what ends every drain on
    // the stack at its next test (`_drain_queued`).
    void
    _drop_deferred() {
        for (unsigned turn = 0; !_deferred.empty(); ++turn) {
            if (turn == kMaxTeardownPasses) [[unlikely]]
                return _give_up_deferred();
            std::deque<std::function<void()>> dropped;
            dropped.swap(_deferred);
            _deferred_taken += dropped.size();
            dropped.clear();
        }
    }

    // What is still deferred is given up: taken out of the listener, never run and never destroyed -- leaked on
    // purpose, and LeakSanitizer says so: a destructor that defers again at every turn is the defect it reports.
    QB_NOINLINE void
    _give_up_deferred() {
        auto *const given_up = new std::deque<std::function<void()>>;
        given_up->swap(_deferred);
        _deferred_taken += given_up->size();
    }

    /** @brief Prepend `e` to the registered-events list. O(1), no alloc. */
    inline void
    _link(IRegisteredKernelEvent *e) noexcept {
        e->_list_prev = nullptr;
        e->_list_next = _registered_head;
        if (_registered_head)
            _registered_head->_list_prev = e;
        _registered_head = e;
        ++_registered_count;
    }

    /**
     * @brief Detach `e` from the list if it is currently linked.
     * @return `true` when `e` was linked (and is now detached); `false` when it
     *         was either never registered with this listener or already
     *         unlinked (idempotency safety net for double-unregister bugs).
     */
    [[nodiscard]] inline bool
    _unlink(IRegisteredKernelEvent *e) noexcept {
        // A node is linked iff it is the current head OR its `_list_prev`
        // points somewhere. This check is robust against already-detached
        // pointers (double `unregisterEvent`).
        if (e->_list_prev == nullptr && _registered_head != e)
            return false;

        auto *prev = e->_list_prev;
        auto *next = e->_list_next;
        if (prev)
            prev->_list_next = next;
        else
            _registered_head = next;
        if (next)
            next->_list_prev = prev;
        e->_list_prev = nullptr;
        e->_list_next = nullptr;
        --_registered_count;
        return true;
    }

    /**
     * @brief Resolve the libev backend flags from the `QB_EV_BACKEND` environment
     *        variable, falling back to `EVFLAG_AUTO`.
     *
     * Accepts: select, poll, epoll, kqueue, port, linuxaio, iouring (alias io_uring),
     * auto. Forcing a specific backend is primarily a testing/benchmarking aid so the
     * whole suite can be exercised on each backend (otherwise io_uring/linuxaio are
     * never auto-selected — `EV_RECOMMEND_*` defaults are 0). The selection is
     * **safe**: an unknown name, a not-compiled-in backend, or a backend that fails
     * to initialise at runtime (e.g. io_uring blocked by a container seccomp policy)
     * all degrade to `EVFLAG_AUTO` with a one-line stderr notice — never a throw.
     */
    [[nodiscard]] static unsigned int
    _resolve_backend_flags() noexcept {
#if defined(_MSC_VER)
#pragma warning(push)
// The CRT deprecates getenv; this read needs neither ownership nor an allocation.
#pragma warning(disable : 4996)
#endif
        const char *env = std::getenv("QB_EV_BACKEND");
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
        if (!env || !*env)
            return EVFLAG_AUTO;

        static const struct {
            const char  *name;
            unsigned int flag;
        } table[] = {
            {"select", EVBACKEND_SELECT},   {"poll", EVBACKEND_POLL},        {"epoll", EVBACKEND_EPOLL},
            {"kqueue", EVBACKEND_KQUEUE},   {"port", EVBACKEND_PORT},        {"linuxaio", EVBACKEND_LINUXAIO},
            {"iouring", EVBACKEND_IOURING}, {"io_uring", EVBACKEND_IOURING}, {"auto", EVFLAG_AUTO},
        };

        unsigned int req   = 0;
        bool         known = false;
        for (auto const &e : table)
            if (std::strcmp(env, e.name) == 0) {
                req   = e.flag;
                known = true;
                break;
            }

        if (!known) {
            QB_LOG_INFO("[qb-io] QB_EV_BACKEND='" << env << "' unknown; using auto");
            return EVFLAG_AUTO;
        }
        if (req == EVFLAG_AUTO)
            return EVFLAG_AUTO;
        if (!(ev_supported_backends() & req)) {
            QB_LOG_INFO("[qb-io] QB_EV_BACKEND='" << env << "' not built into libev; using auto");
            return EVFLAG_AUTO;
        }
        /* Probe: a backend can be compiled in yet fail at runtime (io_uring under a
         * restrictive seccomp profile, kqueue on some sandboxes, ...). Create and
         * immediately destroy a throwaway loop so the real loop below never throws. */
        if (struct ev_loop *probe = ev_loop_new(req)) {
            ev_loop_destroy(probe);
            return req;
        }
        QB_LOG_INFO("[qb-io] QB_EV_BACKEND='" << env << "' unavailable at runtime; using auto");
        return EVFLAG_AUTO;
    }

private:
    // Everything of a listener but its loop: the state both constructors start from.
    struct no_loop_tag {};
    explicit listener(no_loop_tag) noexcept
        : _active_count(kNoCounts)
        , _pending_count(kNoCounts)
        , _io_count(kNoCounts)
        , _io_fed(&kNoIoFed)
        , _defer_wake(_loop)
        , _wake(_loop)
        , _park_cap(_loop)
        , _timer_count(kNoCounts)
        , _periodic_count(&kNoPeriodics)
        , _wake_pending(&kNoWake) {
        _defer_wake.set<listener, &listener::_on_defer_wake>(this);
        // Lowest priority: libev invokes pendings highest-priority-first, so the
        // drain lands after every other watcher pending in the same iteration.
        // Safe here — the watcher is neither active nor pending at construction.
        ev_set_priority(static_cast<ev_timer *>(&_defer_wake), EV_MINPRI);
        _wake.set<listener, &listener::_on_wake>(this);
        _park_cap.set<listener, &listener::_on_park_cap>(this);
    }

    // The constructor of `listener::current`, and of nothing else. It enters the thread's record -- a listener of
    // one's own never does: the record is about the loop the thread's coroutines wait on -- and it builds NO loop:
    // MSVC runs it at the start of every thread of the process, whatever that thread does (Huly QB-1246). From
    // here "this is the thread's listener" is asked of the record (`_is_thread_current()`), on the calling thread:
    // a member called from another thread, against the class's rule, writes nothing into that thread's record.
    struct thread_current_tag {};
    explicit listener(thread_current_tag) noexcept
        : listener(no_loop_tag{}) {
        detail::thread_loop_t &state = detail::thread_loop();
        state.owner                  = this;
        state.phase                  = detail::thread_loop_phase::alive;
    }

    // The loop, and everything that reads it: the backend (`QB_EV_BACKEND`), the counters' addresses, the three
    // watchers of the listener's own, the counter's calibration (~200 us). The public constructor runs it; the
    // thread's listener runs it on the first use that needs a loop, through `_require_loop()`. Never in a listener
    // whose destructor has run: that would be a loop nobody destroys, in an object nobody may read. It throws
    // `ev::bad_loop` when no backend can be created (it was the thread_local's initialiser that threw, at the
    // first touch); from a `noexcept` caller -- `backend()`, `set_io_poll_interval()`, `set_dispatch_timing()`,
    // qb-core's `ask` -- that is `std::terminate()`, as it always was on MSVC, where the initialiser ran in the
    // thread's startup.
    QB_NOINLINE void
    _build_loop() {
        if (detail::is_gone_thread_listener(this)) [[unlikely]]
            detail::refuse_after_thread_loop("its thread's event loop");
        _loop.build(_resolve_backend_flags());
        const ev::loop_ref loop = _loop;
        _active_count           = loop.active_count_addr();
        _pending_count          = loop.pending_count_addr();
        _io_count               = loop.io_count_addr();
        _io_fed                 = loop.io_fed_addr();
        _timer_count            = loop.timer_count_addr();
#if EV_PERIODIC_ENABLE
        _periodic_count = loop.periodic_count_addr();
#endif
        _wake_pending = loop.wake_pending_addr();
        _defer_wake.set(loop);
        _wake.set(loop);
        _park_cap.set(loop);
        _calibrate_tsc(); // the interval itself stays 0 (poll every pass) until an embedder sets one
        if (_is_thread_current())
            detail::thread_loop().raw = static_cast<struct ev_loop *>(loop);
    }

    // This listener is the calling thread's `listener::current`: the thread's record names it.
    [[nodiscard]] bool
    _is_thread_current() const noexcept {
        return detail::thread_loop().owner == this;
    }

    // The door of every request that needs this listener's loop, then the loop itself. The thread's record is read
    // BEFORE any member: after its destructor the thread's listener may not be read at all, and the request stops
    // the process on the line that names it. Then the loop is built if this is the thread's listener and nothing
    // needed one yet -- `const`, because `loop()` and `backend()` are: no listener object is const, and one a user
    // declared const built its loop in its constructor.
    void
    _require_loop(char const *const request) const {
        if (detail::is_gone_thread_listener(this)) [[unlikely]]
            detail::refuse_after_thread_loop(request);
        if (!_loop.raw_loop) [[unlikely]]
            const_cast<listener *>(this)->_build_loop();
    }

    // Sixteen teardown passes, as a scheduler's: see the destructor.
    static constexpr unsigned kMaxTeardownPasses = 16;

    // What is still registered is stopped and taken off the list, its owner left alive: no destructor runs, so
    // nothing registers again. A loop-owned handler is leaked. The destructor's last resort.
    void
    _detach_registered() noexcept {
        while (_registered_head) {
            IRegisteredKernelEvent *const cur = _registered_head;
            (void) _unlink(cur);
            cur->stop();
            cur->_detached_by_clear = true;
        }
    }

public:
    /**
     * @brief Constructor
     *
     * Creates a new listener with a dynamic event loop. The backend is normally
     * auto-detected (libev's `EVFLAG_AUTO`: e.g. epoll/io_uring on Linux, kqueue/
     * select on macOS). It can be pinned for testing/benchmarking via the
     * `QB_EV_BACKEND` environment variable (see `_resolve_backend_flags()`).
     * The thread's own listener, `listener::current`, builds its loop the same way, on its
     * first use that needs one.
     */
    listener()
        : listener(no_loop_tag{}) {
        _build_loop();
    }

    /**
     * @brief libev backend actually in use by this listener's loop.
     * @return One of the `EVBACKEND_*` values (e.g. `EVBACKEND_EPOLL`).
     */
    [[nodiscard]] inline unsigned int
    backend() const noexcept {
        if (detail::is_gone_thread_listener(this)) [[unlikely]]
            return 0; // no backend any more
        if (!_loop.raw_loop) [[unlikely]]
            const_cast<listener *>(this)->_build_loop();
        return _loop.backend();
    }

    /**
     * @brief Human-readable name of the backend actually in use.
     */
    [[nodiscard]] static const char *
    backend_name(unsigned int b) noexcept {
        switch (b) {
            case EVBACKEND_SELECT:
                return "select";
            case EVBACKEND_POLL:
                return "poll";
            case EVBACKEND_EPOLL:
                return "epoll";
            case EVBACKEND_KQUEUE:
                return "kqueue";
            case EVBACKEND_PORT:
                return "port";
            case EVBACKEND_LINUXAIO:
                return "linuxaio";
            case EVBACKEND_IOURING:
                return "iouring";
            default:
                return "unknown";
        }
    }

    /**
     * @brief Clear all registered events from this listener.
     *
     * Removes and deletes all registered event handlers (IRegisteredKernelEvent instances).
     * It then runs the loop with `EVRUN_NOWAIT` (a few passes) to flush pending libev work
     * without blocking. Do not use `EVRUN_ONCE` here: with monotonic + timerfd enabled in
     * libev, `ev_run` can choose a ~1.5e6 s waittime when `timercnt == 0`, which would wedge
     * thread teardown (`~listener` / explicit `clear()`).
     *
     * Deferred callbacks are DROPPED, never run -- those queued before the call and those
     * deferred while it tears down (Huly QB-276) -- and their captured state is released.
     * Called from a deferred callback, it ends that drain -- and every drain on the stack, when
     * that callback runs under a turn another one opened (`defer()`): what is deferred after it
     * runs on the next turn.
     * @note This is automatically called by the listener's destructor. On `listener::current` after
     *       its destructor it does nothing: that destructor cleared it.
     */
    void
    clear() {
        if (detail::is_gone_thread_listener(this)) [[unlikely]]
            return;
        QB_LISTENER_TRACE("clear() begin registeredEvents=%zu has_coro_scheduler=%d", _registered_count, _coro_scheduler != nullptr);
        // Drop pending deferred callbacks: they must not run against a listener being
        // torn down (they may touch registrations we are about to destroy). Each
        // std::function's destructor releases its captured state (e.g. a shared_ptr
        // held only to keep a target alive across the defer) cleanly — no leak, no
        // execution. Watchers are detached below before the flush runs(), so no WATCHER
        // can post anything here; a released closure's destructor still can, and so can
        // what the teardown below destroys -- a loop-owned timer and the state its callable
        // captured -- and what the flush passes resume. All of it is dropped too (Huly
        // QB-276): the queue is HELD for the whole call, so no drain runs and
        // `defer()` does not feed the in-loop hook, and the queue is dropped again on the
        // way out. A drain this call runs inside -- every drain on the stack -- ends at its next
        // test: what is dropped counts as taken off the queue (`_drop_deferred`).
        struct Hold {
            bool      &held;
            bool const was;
            ~Hold() {
                held = was;
            }
        } const hold{_defer_held, std::exchange(_defer_held, true)};
        _drop_deferred();
        _defer_wake.stop(); // also clears a pending feed; nothing is left to drain
        _park_cap.stop();
        disarm_wake();
        if (_registered_head) {
            // Detach every handler but do not delete it here: async::base stores
            // a reference to the embedded event, so deleting the wrapper while
            // the owning async object is still alive leaves a dangling
            // `_async_event`. The owner's destructor will unregister and delete
            // the detached wrapper later.
            //
            // Drain strictly from the head, popping each node with `_unlink()` BEFORE
            // touching it, and re-reading `_registered_head` on every iteration. Never
            // cache a `_list_next` across `destroy(owner)`: that call runs arbitrary
            // user code (the `async::callback` closure's captured state — e.g. the last
            // `shared_ptr` to a still-registered session), and that code's own `~base`
            // re-enters `listener::release()` on ANOTHER still-linked node, freeing it. A
            // cached `next` pointing at that node then becomes dangling and the next
            // `cur->stop()` is a use-after-free. Popping through `_unlink()` also keeps
            // `_registered_count` exact — the previous "null the head + zero the count"
            // prelude let such a re-entrant `_unlink` underflow the counter to SIZE_MAX.
            //
            // The owners it destroys are BUDGETED: sixteen times what was registered when the drain
            // began. A destroyed owner may register again -- an `async::callback` whose closure's
            // destructor posts another -- and the new handler is popped in its turn, so a closure that
            // re-posts at every destruction kept this loop running for ever, and with it the thread's
            // exit (Huly QB-1246). Past the budget a loop-owned handler is stopped and detached like any
            // other and its owner left alive: no destructor runs, nothing registers again, the drain ends.
            // The budget is a cut, not a judgement: a chain of destructions that would have ended by itself
            // later than that leaves its tail undestroyed too (its closures, what they keep alive).
            std::size_t owners_to_destroy = _registered_count * kMaxTeardownPasses;
            while (_registered_head) {
                IRegisteredKernelEvent *cur = _registered_head;
                (void) _unlink(cur);
                cur->stop();
                cur->_detached_by_clear = true;
                if (cur->_destroy_owner && !_is_dispatching(cur) && owners_to_destroy != 0) {
                    --owners_to_destroy;
                    // Loop-owned, self-deleting handler (e.g. an `async::callback`
                    // Timeout) whose one-shot timer never fired: nothing else will
                    // reclaim it. Destroy the owner now — its `~base` re-enters
                    // `listener::release(cur)` and frees this wrapper via the
                    // `_detached_by_clear` branch (cur is already unlinked). Clear the
                    // hook first so the re-entry can never recurse back into here.
                    //
                    // The `!_is_dispatching` guard is essential: when clear() runs from
                    // INSIDE this handler's own invoke() (a callback whose body tears down
                    // the loop), the handler will free itself via `delete this` the moment
                    // invoke() returns — destroying it here too would double-free it. It is
                    // already detached (above), so that in-flight delete reclaims it cleanly.
                    void *const owner   = cur->_owner;
                    auto *const destroy = cur->_destroy_owner;
                    cur->_owner         = nullptr;
                    cur->_destroy_owner = nullptr;
                    destroy(owner); // frees `cur`; do not touch it afterwards
                }
            }
            // The passes that let the loop take the detached watchers out of its backend. While qb-core is
            // resuming a coroutine of this thread inline -- `exit()` reached from an `ask` reply or from an
            // `onInit()` ends the listener from inside it -- they pump the loop ALONE: a whole pass would resume the
            // coroutines queued behind the executing one, and each that finished would destroy its locals in the
            // middle of that teardown (Huly QB-1246). Under a drain `run_ready()` refuses by itself.
            const bool loop_alone = detail::thread_loop().inline_resumes != 0;
            for (int i = 0; i < 4; ++i) {
                if (loop_alone) [[unlikely]]
                    _loop.run(EVRUN_NOWAIT);
                else
                    run(EVRUN_NOWAIT);
            }
            relay_break(); // those passes made the loop forget a break kept for the run this is called under
        }
        _drop_deferred();
        QB_LISTENER_TRACE("clear() end (scheduler NOT reset)");
    }

    /**
     * @brief Destructor
     *
     * Ends with nothing registered and no scheduler, while the loop lives: `clear()` stops and detaches what is
     * registered and destroys the loop-owned handlers, then the scheduler destroys the coroutines it owns -- and
     * again while those destructions left something registered or deferred. A coroutine local's destructor may
     * post an `async::callback`, hand a closing socket over to the loop, or defer; a callback's closure may spawn.
     * Until 3.4 one pass was all there was, and what the scheduler's teardown registered after `clear()` stayed
     * on a loop about to be destroyed: a handler nobody destroyed, a descriptor held until the process ended
     * (Huly QB-1246). Sixteen passes at most, as a scheduler's own teardown (which each pass runs whole): past
     * them, what is still registered is stopped and detached, its owner left alive, and what is still deferred
     * is given up -- no destructor runs any more, so nothing registers again.
     *
     * For `listener::current` the thread's record then reads `gone`: the loop itself goes after this body, with
     * the members.
     *
     * @warning Not from one of this listener's own deferred callbacks, unless that callback ends the process
     *          (`exit()`): the drain that runs it, and the `run()` under that drain, are member functions of this
     *          object, and a blocking run has the loop's own frames on the stack. A callback that destroys its
     *          listener and RETURNS stops the process on a line that says so; until 3.4 the drain went on reading
     *          the freed listener (Huly QB-1246).
     */
    ~listener() noexcept {
        QB_LISTENER_TRACE("~listener() begin");
        for (unsigned pass = 0;;) {
            clear();
            QB_LISTENER_TRACE("~listener() about to reset _coro_scheduler");
            // Leaves no scheduler: one a destructor asks for meanwhile is the dying one. Not `reset_coro_scheduler()`:
            // the listener goes whatever is executing, and a scheduler one of whose coroutines is abandons by itself.
            if (_coro_scheduler && !_scheduler_dying)
                _destroy_coro_scheduler();
            if (!_registered_head && _deferred.empty()) [[likely]]
                break;
            if (++pass == kMaxTeardownPasses) [[unlikely]] { // sixteen passes did not end it
                _detach_registered();
                if (!_deferred.empty())
                    _give_up_deferred();
                break;
            }
        }
        // A drain of this listener still on the stack: one of its callbacks is destroying it. When the process is
        // ending under that callback (`exit()`) nothing returns there; when the callback returns, its drain reads
        // this mark, on its own frame, and stops on a line that says so (`_drain_queued`).
        for (DrainFrame *frame = _drain_top; frame != nullptr; frame = frame->outer)
            frame->listener_gone = true;
        if (detail::thread_loop_t &state = detail::thread_loop(); state.owner == this) {
            // From here the thread's loop is gone: what this listener owned was destroyed above, while it lived.
            // `owner` stays in the record, to be compared; nothing else of it names this listener any more.
            state.raw       = nullptr;
            state.scheduler = nullptr;
            state.phase     = detail::thread_loop_phase::gone;
        }
    }

    /**
     * @brief Generic event callback handler invoked by libev for any active watcher.
     *
     * This method is the entry point for libev to notify of an event. It updates
     * the custom event wrapper's `_revents` field and then invokes the stored
     * `IRegisteredKernelEvent::invoke()` method, which in turn calls the user-defined
     * `on(SpecificEvent&)` handler in the registered actor/object.
     *
     * @tparam EV_EVENT The specific libev watcher type (e.g., `ev::io`, `ev::timer`).
     * @param event The libev watcher that was triggered.
     * @param revents The bitmask of triggered event flags (e.g., `EV_READ`, `EV_WRITE`).
     */
    template <typename EV_EVENT>
    void
    on(EV_EVENT &event, int revents) {
        // Safe reinterpret_cast: event::base<EV_EVENT> is standard-layout compatible
        // with EV_EVENT (libev watcher). Required for the C++ wrapper pattern around libev.
        auto &w    = *reinterpret_cast<event::base<EV_EVENT> *>(&event);
        w._revents = revents;
        // Record this handler as mid-dispatch so a re-entrant clear() (from inside invoke())
        // does not destroy it out from under the in-flight call. invoke() may `delete this`
        // (the wrapper), so nothing below dereferences w._interface afterwards.
        DispatchNode node{w._interface, _dispatch_top};
        _dispatch_top = &node;
        // Contain user exceptions HERE — the single locus every watcher dispatch flows
        // through. `invoke()` runs arbitrary user code (`on(event::disconnected&&)`,
        // `on(event::pending_read&&)`, `on(event::eos&&)`, a protocol handler, an
        // `ev::stat` observer, …), and libev is built as C (qb/src/qb/ev, LANGUAGES C):
        // letting an exception unwind through `ev_invoke_pending`/`ev_run` is UB — it
        // skips libev's own epilogue (`--loop_depth`, the `loop_done = EVBREAK_CANCEL`
        // reset that re-arms a broken loop) and, on toolchains that do not emit unwind
        // info for C (MSVC), is a hard failure. It also strands `_dispatch_top` on a
        // destroyed stack frame, permanently corrupting the re-entrancy guard `clear()`
        // reads. Zero cost on the non-throwing path (table-driven EH), and it matches the
        // policy already applied at every other loop-facing boundary (`Timeout::on`,
        // `ScopedTimeout::on`, `_drain_deferred`).
        try {
            w._interface->invoke();
        } catch (...) {
            QB_LOG_WARN("[qb-io] exception escaped an event handler; contained by the event loop");
        }
        _dispatch_top = node.prev;
        ++_nb_invoked_events;
        ++_total_events_processed;
    }

    /**
     * @brief Register an event handler (actor/object) for a specific asynchronous event type.
     *
     * Creates a `RegisteredKernelEvent` wrapper for the given actor and event type,
     * initializes the underlying libev watcher with the provided arguments, and registers
     * it with this listener's event loop.
     *
     * @tparam _Event The qb-io event type (e.g., `qb::io::async::event::io`, `qb::io::async::event::timer`).
     *                This type wraps a specific libev watcher.
     * @tparam _Actor The type of the class that will handle the event (must have an `on(_Event&)` method).
     * @tparam _Args Types of additional arguments for initializing the libev watcher (e.g., fd and event flags for `ev::io`).
     * @param actor Reference to the actor/object instance that will handle the event.
     * @param args Additional arguments forwarded to the libev watcher's `set()` or equivalent initialization method.
     * @return Reference to the created `_Event` object (which is also the libev watcher).
     *         This reference can be used to later `start()` or `stop()` the watcher.
     */
    template <typename _Event, typename _Actor, typename... _Args>
    _Event &
    registerEvent(_Actor &actor, _Args &&...args) {
        _require_loop("listener::current.registerEvent() (an io object, a timer, an async::callback)");
        auto revent = new RegisteredKernelEvent<_Event, _Actor>(_loop, actor);
        revent->_event.template set<listener, &listener::on<typename _Event::ev_t>>(this);
        revent->_event._interface = revent;

        if constexpr (sizeof...(_Args) > 0)
            revent->_event.set(std::forward<_Args>(args)...);

        _link(revent);
        return revent->_event;
    }

    /**
     * @brief Unregister an event handler and its associated libev watcher.
     *
     * Removes the specified `IRegisteredKernelEvent` from the listener's tracking
     * and deletes the event handler object, which also stops and cleans up the
     * underlying libev watcher via its destructor.
     *
     * @param kevent Pointer to the `IRegisteredKernelEvent` to unregister. This pointer
     *               is typically obtained when the event was initially registered or stored
     *               within the libev watcher wrapper itself (e.g., `_Event::_interface`).
     * @note A wrapper `clear()` detached is on no list: it is freed without reading this listener (see
     *       `release()`).
     */
    void
    unregisterEvent(IRegisteredKernelEvent *kevent) {
        if (!kevent)
            return;
        if (kevent->_detached_by_clear || _unlink(kevent)) {
            kevent->_detached_by_clear = false;
            delete kevent;
        }
    }

    /**
     * @brief Release the registration an io object holds on the thread's listener: the one entry point of every
     *        owner's teardown (`io::async::base`, the QUIC endpoint, the tcp connector).
     * @details A wrapper `clear()` detached is freed WITHOUT reaching a listener: its owner may be destroyed after
     *          the listener that held it -- an io object with static storage, after the main thread's
     *          `listener::current` -- and `clear()`, which that listener's destructor runs, already took it off
     *          the list. Any other is unregistered from the thread's listener, as
     *          `listener::current.unregisterEvent()` does. Until 3.4 every owner called that member, which read
     *          the destroyed listener first (Huly QB-1246).
     * @note DEFINED after `listener::current`'s definition, below the class, and never in it: its body reads
     *       `current`, and a body written in the class names the variable through its in-class declaration, which
     *       is not `inline`. On Darwin, clang then emits the variable's thread wrapper STRONG (`external`) in every
     *       translation unit whose first reference to it is this one, and two such units in one link are a
     *       duplicate `thread-local wrapper routine for qb::io::async::listener::current` (ld's warning on the macOS
     *       runner, a link error on a developer's Mac, five qbm-http test binaries when it was written here).
     *       Written after the definition it names the inline one, and the wrapper is `weak_odr hidden`, as it
     *       always is on Linux (the MSVC ABI has none). The rule is at that definition, and
     *       `scripts/check-header-linkage.py` (THREAD-WRAP) refuses the shape.
     */
    static void release(IRegisteredKernelEvent *kevent);

    /**
     * @brief Get a reference to the underlying libev event loop.
     * @return `ev::loop_ref` (a reference wrapper to `ev_loop*`) for this listener.
     * @details Useful for advanced direct interaction with libev if needed, though most
     *          operations are handled through the listener's API. On `listener::current` this
     *          builds the thread's loop if nothing needed one yet; after its destructor the
     *          request stops the process by name. The coroutine awaiters do not come through
     *          here on their fast path: they read the thread's record (`detail::thread_listener_loop()`).
     */
    [[nodiscard]] inline ev::loop_ref
    loop() const {
        _require_loop("listener::current.loop()");
        return _loop;
    }

    /**
     * @brief Run the event loop to process pending events.
     *
     * Executes the event loop with the specified libev run flag.
     * This call blocks or returns based on the flag and event activity.
     * `nb_invoked_event()` reports this turn's count afterwards: the watchers the loop's pass invoked, the
     * deferred callbacks and the coroutine resumes, written once at the end of the turn.
     *
     * After processing libev events, any ready coroutines are also executed -- unless this pass
     * is nested inside a coroutine drain (library code draining the loop from a coroutine body,
     * `Redis::await()`, `Transaction::await()`): that drain resumes them (Huly QB-253).
     *
     * @param flag The libev run flag (e.g., `EVRUN_NOWAIT` to check once and return,
     *             `EVRUN_ONCE` to wait for and process one event block, `0` for default blocking run).
     *             Default is `0`, which means `ev_run` will block until `ev_break` is called or no active watchers remain.
     */
    inline void
    run(int flag = 0) {
        // This turn's count, in a local until the end of the turn (Huly QB-257): the watchers the
        // loop's pass invokes count into `_nb_invoked_events` (`on()`, the in-loop defer drain), so
        // that member is zeroed only when the loop runs; the deferred and coroutine counts are added
        // in a register, and the two members are written ONCE at the end. This body is inlined into
        // every core's pass, and the zeroing store plus two read-modify-writes per count it used to
        // make were stores to the listener's line on every io pass.
        std::size_t invoked = 0;
        // The libev pass is gated on the loop's OWN liveness rule — `ev_run` keeps
        // looping while `activecnt` (referenced active watchers) is non-zero, and a
        // `NOWAIT` pass with nothing active and nothing pending would still cost a
        // `backend_poll(0)` plus two clock reads (~300–380 ns measured, on every core
        // tick, for the life of any core that once created a coroutine scheduler).
        // The two counts are the loop's own, so a raw `ev_timer_start` from a
        // coroutine awaiter, a fed `_defer_wake` event and a `loop.unref()`-ed
        // watcher are all judged exactly as `ev_run` itself judges them.
        const bool pending = _loop_has_pending(); // walked once here, for the liveness rule and the gate
        if (*_active_count > 0 || pending) {
            _nb_invoked_events = 0;
            int f              = flag;
            // A non-blocking pass asks the gate (out of line, see `_nowait_gate`) whether the loop
            // has anything to do and with which flags; a blocking pass always runs it.
            if (!(flag & EVRUN_NOWAIT) || _nowait_gate(f, pending)) {
                if (_supply)
                    _loop.now_set(_supply_loop_s);
                if (!(flag & EVRUN_NOWAIT))
                    _run_blocking(f); // the in-loop defer drain's runs (see `_defer_wake`)
                else
                    _loop.run(f);
                // The poll reported a ready fd: stay at one poll per pass for the next one (a
                // burst is served at poll latency); a poll that found nothing, or a pass that
                // skipped it, leaves the loop cold. Read off the loop's own counter, so a raw
                // watcher counts.
                _io_hot      = *_io_fed != _io_fed_seen;
                _io_fed_seen = *_io_fed;
            }
            _supply = false;
            invoked = _nb_invoked_events;
        }

        // Deferred callbacks: continuations of the dispatch that just unwound.
        // Drained before coroutines so a `defer()` that wakes a coroutine is
        // picked up by `run_ready()` in the same turn. Not the generation the in-loop
        // drain parked for the next turn (`_drain_deferred` refuses it, Huly QB-448).
        invoked += _drain_deferred();

        // Process ready coroutines.
        //
        // BOUNDED on purpose. `run_ready()` defaults to draining until the ready queue is
        // empty, and a coroutine resumed from that queue may enqueue another one immediately
        // (every waker in qb-io defers through `schedule_via_current`, which pushes straight
        // onto that queue). Two coroutines that resume each other — an ordinary unbuffered
        // `channel<T>` producer/consumer pipeline with no I/O await in the cycle is enough —
        // therefore keep the queue non-empty forever and `run()` NEVER RETURNS. That starves
        // the whole turn: no libev watcher runs again, and a `VirtualCore` driving this
        // listener never reaches `__flush_all__` / `__receive__` / its actor callbacks, so the
        // engine deadlocks with a busy CPU. Measured before this bound: a single
        // `run(EVRUN_NOWAIT)` turn executed 2,000,000 ping-pongs in 162 ms and only came back
        // because the probe's loops were finite.
        //
        // The cap is per TURN, not per coroutine: anything scheduled after it is hit simply
        // runs on the next turn, so nothing is dropped or reordered — the loop just gets a
        // chance to breathe. It is set far above any realistic burst (a workload resuming
        // 64k coroutines in one turn is already pathological), so normal latency is untouched
        // and only the self-feeding shape is affected. `run_ready()`'s own default stays
        // unbounded for the teardown drains that genuinely must empty the queue.
        //
        // A pass NESTED inside a drain -- library code draining the loop from a coroutine body,
        // `Redis::await()`, `Transaction::await()` -- resumes no coroutine: `run_ready()` returns 0
        // when its scheduler is already draining, and that drain resumes what this pass made ready
        // once the running coroutine yields (Huly QB-253). The test lives there, not here: this
        // body is inlined into the core's loop (`VirtualCore::__workflow_loop__`), and a condition
        // added to it reshaped that whole hot loop. The watchers and deferred callbacks above have
        // run either way.
        if (_coro_scheduler)
            invoked += _coro_scheduler->run_ready(kMaxCoroutineResumesPerTurn);
        _nb_invoked_events = invoked;
        _total_events_processed += invoked;
    }

    /// Upper bound on coroutine resumes per `run()` turn — see the `run_ready()` call site in
    /// `run()` for why an unbounded drain lets two mutually-resuming coroutines wedge the loop.
    static constexpr std::size_t kMaxCoroutineResumesPerTurn = 65536;

    /**
     * @brief Queue `fn` to run once, at the tail of the current `run()` turn.
     *
     * The callback executes after every libev watcher for this turn has returned,
     * never inline from the call: once the handler that deferred it has unwound -- or, if
     * that handler pumps the loop itself, at the tail of a turn it runs. Use it when a
     * handler must continue work that is unsafe inline — above all destroying or
     * replacing the object the handler is currently executing on (a reconnect that
     * frees+recreates its connection). It is the non-coroutine twin of the
     * `co_await sleep(0ms)` cooperative yield.
     *
     * Prefer it over `async::callback(fn, tiny_delay)` for "next turn" semantics:
     * no libev timer, no heap `Timeout`, no arbitrary delay. Captured state is
     * released when the callback fires OR when the loop is torn down (`clear()`),
     * whichever comes first — so a strong (`shared_ptr`) capture keeps its target
     * alive exactly until then, leak-free. Same-thread only. A callback deferred by a
     * deferred callback runs on the next `run()` call, never in the one that ran its
     * parent: one generation per turn. On `listener::current` after its destructor the
     * callback is dropped, never run.
     *
     * A deferred callback may RUN THE LOOP -- `run_sync()`, `run_until()`, `run()`, a module's
     * `await()`. That run is a turn of its own, inside the callback's: it runs the callbacks
     * queued behind the one that pumps, and those queued meanwhile, in the order they were
     * queued, each once. So a deferred callback can run while another is on the stack -- as a
     * watcher's handler can, under any loop run from a handler. Until 3.4 a loop run from a
     * deferred callback drained nothing: it waited for ever for anything `defer()` was to
     * deliver, a connect refused at once among them (Huly QB-1246). What a callback relies on
     * across a pump of its own -- an iterator, a lock, "nobody else touches this yet" -- is
     * the callback's to protect, here as in a handler. And such turns NEST: a callback that
     * pumps runs the callbacks queued behind it, so N queued callbacks that each pump the loop
     * are N turns deep on the stack when the last one runs. The stack bounds that depth, and
     * nothing else does.
     *
     * @note The queued `fn` is drained by `run()`: a NOWAIT pump drains at the tail of each
     *       call (`async::run_until`/`run_sync`, or a VirtualCore tick — which gates on
     *       `has_deferred()` so a bare `defer()` still pumps); a blocking run hands what is
     *       queued to its in-loop drain on entry, and that drain serves every pass of the loop
     *       for as long as the run is on the stack -- the passes of a non-blocking pump run from
     *       one of its handlers included -- two generations a poll of the loop apart. A `defer()`
     *       issued *from a coroutine* (which runs after the drain) fires on the next turn.
     * @note Only the drain contains a throwing `fn`; the enqueue here is a plain
     *       allocation and may itself throw `std::bad_alloc` under OOM.
     */
    void
    defer(std::function<void()> fn) {
        if (detail::is_gone_thread_listener(this)) [[unlikely]]
            return; // after the thread's loop no turn will run it: dropped, its captured state released (as clear() does)
        _deferred.push_back(std::move(fn));
        // Under a blocking run, arm the in-loop drain hook (see `_defer_wake`) so the
        // queue is serviced even though `run()` does not return; a non-blocking turn
        // drains at its tail, and a blocking one hands what is queued to the hook on
        // entry (`_run_blocking`). Skipped when the caller is a callback that hook is
        // draining: a callback that defers must land on the NEXT turn, and
        // `_on_defer_wake` re-arms it with a 0-delay one-shot for exactly that -- and
        // skipped while `clear()` runs, which holds the queue and drops it. The
        // pending/active tests keep a burst of defers to a single queue entry.
        if (_blocking_runs != 0 && !_hook_drains && !_defer_held && !_defer_wake.is_pending() && !_defer_wake.is_active())
            _defer_wake.feed_event(EV_CUSTOM);
    }

    /**
     * @brief Request the event loop to break out of its current `run()` cycle.
     * @details This signals the libev loop to stop processing further events in the current
     *          `run()` invocation. If `run()` was called with default blocking behavior,
     *          it will return after the current event (if any) is processed.
     *
     *          It is for the innermost run that only a break ends -- `run()` with no flag, `async::run()` -- and that
     *          run KEEPS it (3.4, Huly QB-1246). The loop keeps a break for the `ev_run` in progress only, and a pump
     *          begins one at every pass (`run_until()`, `run_sync()`: non-blocking passes, `EVRUN_ONCE` turns): a
     *          break asked while a handler or a deferred callback of the run pumped, or before one pumped in the same
     *          iteration, was lost until 3.4, and the run never ended. Since 3.4 a deferred callback queued behind one
     *          that pumps runs inside that pump, where it used to run after it. So the run ends, before it polls
     *          again, once whatever was pumping has returned to it:
     *          - a deferred callback of the run, whatever it pumped with;
     *          - a watcher's handler, when it pumped with the library's own pumps (`async::run(flag)`, `run_once()`,
     *            `run_until()`, `run_for()`, `run_sync()`, `run_once_for()`) or with `EVRUN_ONCE` turns. A pump
     *            written by hand over `run(EVRUN_NOWAIT)` in a handler ends with `relay_break()`: without it the run
     *            does not end when that handler returns -- the request stays kept, and is said again only at the
     *            next of these that the run itself reaches, not one under another pump: a deferred callback it
     *            drains, a pump of the library or an `EVRUN_ONCE` turn that one of its handlers runs, a run one of
     *            them opens, when it ends.
     *          A run of that kind opened under another is ended by its own break alone, and a break the outer one
     *          keeps is the outer one's still when the inner one has ended. A break names no run, though: a deferred
     *          callback that such an inner run drains -- everything queued when it began is handed to it, a callback
     *          queued behind the one that opened it included -- asks its break of the INNER run. With no such run on
     *          the stack -- a pump at the top, a core's passes -- a break ends the pass in progress, as before: it is
     *          not held for a later run. `loop().break_loop(ev::ALL)`, the loop's own call, is not kept; and it is
     *          not narrowed: while the loop holds it, neither this call nor a kept break said again writes over it.
     *
     *          Whatever keeps it, the call still ends the `ev_run` it is made in, as it always did: a pass or a park
     *          of a pump under the run -- before it polls, when it is made as that pass begins. That is the loop's
     *          own rule for a break; the pump goes on with its next pass.
     */
    inline void
    break_one() {
        if (detail::is_gone_thread_listener(this)) [[unlikely]]
            return;
        if (_loop.raw_loop) {                       // no loop yet, no run to break
            if (_loop.break_state() != EVBREAK_ALL) // a break of every run, which the loop holds: not narrowed to one
                _loop.break_loop();
            if (_break_run)
                _break_asked = true; // kept: a pump under the run would make the loop forget it (`_run_until_broken`)
        }
    }

    /**
     * @brief For a pump: says again to the loop a break that was asked while it pumped.
     * @details The last statement of a pump -- a loop of `run(EVRUN_NOWAIT)` passes that waits for something. Every
     *          pass makes the loop forget a break (`break_one()`). `async::run(flag)`, `run_until()`, `run_for()`
     *          and `run_sync()` end with this call, `run_once_for()` makes it after its non-blocking pass, and a
     *          pump written by hand does the same; an `EVRUN_ONCE` turn -- `run_once()`, the park of
     *          `run_once_for()` -- does it by itself as it returns. A no-op when no break is kept (no run that only
     *          a break ends is on the stack, or nobody asked), when the loop still holds one, and under a pass of
     *          another pump: a break said there would end that pass and nothing else, and the pump that runs it
     *          relays in its turn.
     */
    void
    relay_break() noexcept {
        if (_break_asked) [[unlikely]]
            _say_break_again();
    }

    /**
     * @brief Get the number of events invoked during the last call to `run()`.
     * @return The count of events that were processed and dispatched to handlers.
     * @note This counter is reset at the beginning of each `run()` call.
     */
    [[nodiscard]] inline std::size_t
    nb_invoked_event() const {
        return _nb_invoked_events;
    }

    /**
     * @brief How often a NON-BLOCKING `run()` over a loop that owns an fd polls the backend
     *        while nothing was delivered on the previous pass (see the io cadence). Owner
     *        thread only.
     * @param interval Zero or less polls on every pass -- the listener's own default, so a
     *        program that drives `run(EVRUN_NOWAIT)` itself keeps the 3.1 contract (one call,
     *        one poll); a `VirtualCore` sets its core's interval from
     *        `CoreInitializer::setIoPollInterval` (1 µs by default). Takes effect on the next
     *        `run()`.
     */
    inline void
    set_io_poll_interval(qb::duration const interval) noexcept {
        if (detail::is_gone_thread_listener(this)) [[unlikely]]
            return;
        if (!_loop.raw_loop) [[unlikely]]
            _build_loop(); // the interval is kept in the counter's ticks: it needs the calibration
        const auto ns   = std::chrono::duration_cast<std::chrono::nanoseconds>(interval).count();
        _poll_every_tsc = (ns > 0 && _tsc_per_ns > 0.) ? static_cast<std::uint64_t>(_tsc_per_ns * static_cast<double>(ns)) : 0;
    }

    /// The interval `set_io_poll_interval()` holds, in the CPU counter's ticks (0 = every pass).
    [[nodiscard]] inline std::uint64_t
    io_poll_interval_ticks() const noexcept {
        if (detail::is_gone_thread_listener(this)) [[unlikely]]
            return 0;
        return _poll_every_tsc;
    }

    /// The calibration's one reader outside the listener: a `VirtualCore`'s idle floors (Huly QB-251). Internal.
    friend double detail::tsc_per_ns(listener const &loop) noexcept;

    /**
     * @brief Get the total number of events processed since listener creation.
     * @return The cumulative count of all events that have been processed by this listener.
     * @note This counter is never reset and provides a lifetime metric for the listener.
     *       Useful for monitoring and debugging purposes.
     */
    [[nodiscard]] inline std::size_t
    total_events_processed() const {
        if (detail::is_gone_thread_listener(this)) [[unlikely]]
            return 0;
        return _total_events_processed;
    }

    /**
     * @brief Get the number of currently registered event handlers.
     * @return The total number of active event watchers managed by this listener.
     */
    [[nodiscard]] inline std::size_t
    size() const {
        if (detail::is_gone_thread_listener(this)) [[unlikely]]
            return 0;
        return _registered_count;
    }

    /**
     * @brief Whether any `defer()`ed callback is pending drain.
     * @return `true` if the next `run()` turn has deferred work to execute.
     * @details qb-core's `VirtualCore` only pumps the loop when there is io/coroutine
     *          work; it folds this into that gate so a `defer()` issued from a pure-actor
     *          handler (no live qb-io object) is still drained on the next core tick.
     */
    [[nodiscard]] inline bool
    has_deferred() const noexcept {
        if (detail::is_gone_thread_listener(this)) [[unlikely]]
            return false;
        return !_deferred.empty();
    }

    /**
     * @brief Whether a `run()` turn has anything to do.
     * @return `true` if the libev loop holds a referenced active watcher or a pending
     *         event, a `defer()`ed callback awaits its drain, or a coroutine is ready.
     * @details The one gate a driver needs. `size()` only counts handlers registered
     *          through this listener and cannot see a raw `ev_timer_start` from a
     *          coroutine awaiter; `has_coro_scheduler()` says a scheduler EXISTS, not
     *          that it has work. Both were what qb-core's `VirtualCore` used to test, so
     *          a core that had ever touched a coroutine polled libev on every pass for
     *          the rest of its life. This reads the loop's own `ev_active_count()` /
     *          `ev_pending_count()` instead: a watcher the loop would deliver is a
     *          watcher this reports, and nothing else is.
     */
    [[nodiscard]] inline bool
    has_work() const noexcept {
        return _loop_has_work() || !_deferred.empty() || (_coro_scheduler && _coro_scheduler->has_work());
    }

    /**
     * @brief Arm the cross-thread wake that ends a `run_once_for()` park. Owner thread only.
     * @details Idempotent. Starts the loop's `ev_async` watcher — which creates the loop's
     *          wake pipe (an eventfd on Linux, a pipe on macOS, a loopback socket pair on
     *          Windows) on first use — then `unref()`s it, so an armed listener with no other
     *          watcher still reports `has_work() == false` and `run()` still skips the libev
     *          pass. A driver arms it once, right before its first `run_once_for()`; a core
     *          that never parks with io never pays for the pipe.
     */
    inline void
    arm_wake() {
        _require_loop("listener::current.arm_wake()");
        if (_wake_armed)
            return;
        _wake.start();
        _loop.unref();
        _wake_armed = true;
    }

    /**
     * @brief Undo `arm_wake()`. Owner thread only; a no-op when not armed.
     * @details `ref()` before the stop — an `unref()`-ed watcher stopped without its ref
     *          would drive the loop's active count negative. Called from `clear()`, so a
     *          listener is disarmed before its loop is destroyed.
     */
    inline void
    disarm_wake() noexcept {
        if (detail::is_gone_thread_listener(this)) [[unlikely]]
            return; // its destructor disarmed it
        if (!_wake_armed)
            return;
        _loop.ref();
        _wake.stop();
        _wake_armed = false;
    }

    /**
     * @brief Whether `arm_wake()` has been called and not undone.
     */
    [[nodiscard]] inline bool
    is_wake_armed() const noexcept {
        if (detail::is_gone_thread_listener(this)) [[unlikely]]
            return false;
        return _wake_armed;
    }

    /**
     * @brief End a `run_once_for()` park from ANY thread. The one thread-safe call on this class.
     * @details Wraps `ev_async_send`, which libev guarantees safe against a concurrent
     *          `ev_run` on the owner thread. Coalesces: a second send before the loop has
     *          consumed the first is a fenced load and a return. The caller must know the
     *          wake is armed — `arm_wake()` happens-before any `wake()` through whatever
     *          published the listener to the calling thread (qb-core: the mailbox's mutex).
     *          A send that lands while the loop is NOT parked is harmless: the next `ev_run`
     *          pass drains it before it can block (the same pass that heals a stale
     *          `pipe_write_skipped`), costing one non-blocking iteration.
     */
    inline void
    wake() noexcept {
        _wake.send();
    }

    /**
     * @brief One loop turn that may BLOCK, for at most `cap`, until a watcher fires or `wake()` lands.
     * @param cap Longest time the turn may block. Non-positive: does not block.
     * @return The number of events this turn invoked — io dispatches, deferred callbacks and
     *         coroutine resumes, exactly what `nb_invoked_event()` reports after `run()`; a
     *         turn ended by `wake()` alone or by the cap reports 0.
     * @details The park half of `arm_wake()`. Runs `run(EVRUN_ONCE)` with a one-shot cap timer
     *          armed, so the libev pass blocks in the backend poll — the loop's own liveness
     *          rule needs a referenced active watcher to block at all, and the cap timer IS
     *          one, which is also what makes the block bounded. The loop clock is refreshed
     *          first: `ev_timer_start` schedules against `mn_now`, which is as stale as the
     *          time since the last pass, and a timer scheduled against a stale clock expires
     *          early or at once. A turn that already has something to run does not block —
     *          a deferred callback, a ready coroutine, or an event fed to the loop since its
     *          last pass: none of the three shortens `ev_run`'s wait (it computes the block
     *          from watcher deadlines alone), so such a turn is a plain `run(EVRUN_NOWAIT)`.
     *          The cap timer is stopped on every exit, an exception out of a handler included,
     *          so a core that dies inside its park leaves no referenced watcher behind.
     * @note Owner thread only; must be armed (`arm_wake()`) so `wake()` has a target.
     */
    inline std::size_t
    run_once_for(qb::duration const cap) {
        _require_loop("listener::current.run_once_for()");
        const bool prompt = !_deferred.empty() || (_coro_scheduler && _coro_scheduler->has_ready()) || _loop_has_pending();
        if (prompt || cap <= qb::duration::zero()) {
            run(EVRUN_NOWAIT);
            relay_break(); // the pass of a pump: the turn below says a kept break again by itself (`_run_blocking`)
            return _nb_invoked_events;
        }
        struct CapGuard {
            ev::timer &cap;
            ~CapGuard() {
                cap.stop();
            }
        } const guard{_park_cap};
        ev_now_update(_loop);
        _park_cap.start(std::chrono::duration<double>(cap).count());
        run(EVRUN_ONCE);
        return _nb_invoked_events;
    }

    /**
     * @brief Get the coroutine scheduler for this listener
     * @return Reference to the CoroutineScheduler
     *
     * Creates the scheduler on first access. The reference is valid until the
     * listener is destroyed or reset_coro_scheduler() is called. Do not store
     * this reference across listener teardown (single-thread: use within run/run_for).
     */
    [[nodiscard]] inline CoroutineScheduler &
    coro_scheduler() {
        // A door to the thread's scheduler (`detail::bind_thread_scheduler()` is the other): never on a listener
        // whose destructor has run. Until 3.4 this built a scheduler inside the destroyed listener, bound it, and
        // whatever was queued there never ran. The free `coro_scheduler()` reads the thread's record first, and
        // comes here only to create.
        if (detail::is_gone_thread_listener(this)) [[unlikely]]
            detail::refuse_after_thread_loop("coro_scheduler()");
        if (!_coro_scheduler) {
            _coro_scheduler = std::make_unique<CoroutineScheduler>();
            if (_is_thread_current())
                detail::thread_loop().scheduler = _coro_scheduler.get();
            // Set as current for this thread
            CoroutineScheduler::set_current(_coro_scheduler.get());
        }
        return *_coro_scheduler;
    }

    /**
     * @brief Check if coroutine scheduler is initialized
     * @return true if scheduler exists
     */
    [[nodiscard]] inline bool
    has_coro_scheduler() const {
        if (detail::is_gone_thread_listener(this)) [[unlikely]]
            return false;
        return _coro_scheduler != nullptr;
    }

    /**
     * @brief Destroy every frame the coroutine scheduler owns and return it to a fresh state,
     *        keeping the scheduler (and its storage) bound to this loop -- see
     *        `CoroutineScheduler::recycle()`. What a `VirtualCore` does at its teardown: the spawned
     *        frames die with their engine, and the next engine on this thread starts warm. No-op when no
     *        scheduler exists.
     */
    inline void
    recycle_coro_scheduler() {
        if (detail::is_gone_thread_listener(this)) [[unlikely]]
            return;
        if (_scheduler_dying || !_coro_scheduler)
            return;
        // A coroutine local it destroys may ask for a reset from its destructor: served, it freed the scheduler under
        // its own `recycle()`. It finds the scheduler busy with its own frames, as during a reset, and does nothing.
        struct Busy {
            bool &flag;
            explicit Busy(bool &f) noexcept
                : flag(f) {
                flag = true;
            }
            ~Busy() {
                flag = false;
            }
        };
        const Busy busy{_scheduler_dying};
        _coro_scheduler->recycle();
    }

    /**
     * @brief Reset the coroutine scheduler (for test isolation). Destroys the frames the scheduler owns (spawned),
     *        then the scheduler, and clears the thread-local current pointer if it names that scheduler -- another
     *        listener's stays bound; a frame it does not own -- a task driven by hand -- is its task's (Huly QB-1220).
     * The next call to coro_scheduler() will create a new scheduler. Use in test TearDown.
     * @note Asked while one of that scheduler's coroutines is executing -- from a coroutine body its drain resumed,
     *       or one qb-core resumed inline -- it does NOTHING and says so on one line on `stderr`. Under a drain the
     *       reset freed the scheduler under its own `run_ready()`, which went on reading it; under an inline resume
     *       it abandoned every coroutine the scheduler owned, the asking one's root included (Huly QB-1246). The
     *       scheduler lives on; leave the coroutine first. Asked from a destructor that a reset or a `recycle()` is
     *       running, it does nothing either, silently: the scheduler is already busy with its own frames.
     */
    inline void
    reset_coro_scheduler() {
        if (detail::is_gone_thread_listener(this)) [[unlikely]]
            return; // its destructor destroyed it
        QB_LISTENER_TRACE("reset_coro_scheduler() begin has_scheduler=%d", _coro_scheduler != nullptr);
        if (!_coro_scheduler || _scheduler_dying)
            return;
        if (detail::resuming_one_of_its_own(*_coro_scheduler)) [[unlikely]] {
            detail::report_refused_scheduler_reset(_coro_scheduler.get());
            return;
        }
        _destroy_coro_scheduler();
    }

private:
    // The scheduler's destruction, unasked: what `reset_coro_scheduler()` does once it may, and what `~listener`
    // does whatever is executing.
    void
    _destroy_coro_scheduler() {
        // Destroyed while it is still this listener's scheduler, still bound, still named by the thread's record:
        // what its coroutines' destructors wake, spawn or clean up finds IT -- and is destroyed with it, by its
        // own teardown passes -- instead of a scheduler built in its place half-way through. Its destructor
        // unbinds it if it is the current one (Huly QB-1220); another listener's stays bound. A destructor that
        // resets the scheduler again from inside this teardown finds it dying, and does nothing: it used to free
        // the scheduler under its own destructor.
        _scheduler_dying = true;
        delete _coro_scheduler.get();
        (void) _coro_scheduler.release();
        _scheduler_dying = false;
        if (_is_thread_current())
            detail::thread_loop().scheduler = nullptr;
        QB_LISTENER_TRACE("reset_coro_scheduler() end");
    }

public:
    /**
     * @brief Time this loop's watcher dispatch: the callbacks one libev iteration runs -- sockets,
     *        timers, signals, the cross-thread wake -- each iteration one span, into
     *        `dispatch_timing()`. Owner thread only (Huly QB-165).
     * @details Off by default, and off costs nothing: libev calls its dispatch through the same
     *          function pointer either way, and timing only swaps it for one that reads the
     *          monotonic clock around libev's own. The deferred callbacks and the coroutine
     *          resumes `run()` drains after the dispatch are not part of it. Under qb-core,
     *          `CoreInitializer::setPassTiming()` times the core's whole pass instead.
     * @param enabled true to time the dispatch from now on; false to stop (the stats are kept).
     */
    void
    set_dispatch_timing(bool const enabled) noexcept {
        if (detail::is_gone_thread_listener(this)) [[unlikely]]
            return;
        if (!_loop.raw_loop) [[unlikely]]
            _build_loop();
        if (enabled) {
            ev_set_userdata(_loop, this);
            ev_set_invoke_pending_cb(_loop, &listener::_timed_invoke_pending);
        } else {
            ev_set_invoke_pending_cb(_loop, &ev_invoke_pending);
        }
    }

    /**
     * @brief What the timed watcher dispatches took (`set_dispatch_timing()`): how many, their
     *        sum, the last, the longest and the longest of the last one to two seconds.
     * @return All zero when the dispatch was never timed.
     */
    [[nodiscard]] TimingStats
    dispatch_timing() const noexcept {
        if (detail::is_gone_thread_listener(this)) [[unlikely]]
            return {};
        return _dispatch_recorder.snapshot(qb::mono_now());
    }

private:
    // Watcher-dispatch timing (Huly QB-165): installed in place of libev's `ev_invoke_pending` only
    // while timing is on. Not noexcept, like the default it replaces: what a callback lets escape
    // goes where it went before. Cold, so at the end of the object.
    static void
    _timed_invoke_pending(struct ev_loop *loop) {
        auto *const self  = static_cast<listener *>(ev_userdata(loop));
        const auto  start = qb::mono_now();
        ev_invoke_pending(loop);
        self->_dispatch_recorder.record(start, qb::mono_now());
    }

    qb::detail::TimingRecorder _dispatch_recorder;
};

/**
 * @brief The one `listener` per thread — definition site.
 *
 * @details
 * Defined **here, `inline`, and annotated `QB_ABI_ANCHOR`**, rather than out of line in
 * `listener.cpp` where it lived until 3.0.0. Both properties are load-bearing, and both were
 * measured on a host executable + `dlopen`ed plugin that each statically link `libqb-io.a`
 * (the shape a plugin host has), same thread, **no unusual flags at all**:
 *
 * ```
 *   as shipped in 2.6 (defined in listener.cpp)      this definition
 *   [HOST  ] &listener::current=0xc24c28080 size=2   [HOST  ] 0x...080  size=2
 *   [PLUGIN] &listener::current=0xc24c29880 size=0   [PLUGIN] 0x...080  size=2
 * ```
 *
 * The mechanism is in the symbol table, not in the C++. A `thread_local` static data member
 * **defined out of line in a `.cpp`** emits its TLS descriptor as `non-external`
 * (`nm -m`: `(__DATA,__thread_vars) non-external __ZN2qb2io5async8listener7currentE`), i.e.
 * private to its image by construction; only the `_ZTW` wrapper is exported, and every
 * reference inside an image already bound to its own at static-link time. Two images therefore
 * hold two event loops for one thread, and everything the second one registers goes into a loop
 * nobody runs — silently, in RTLD_LOCAL and RTLD_GLOBAL alike. Defined `inline` in the header
 * the same descriptor is **weak-external**, which dyld coalesces across images.
 *
 * `QB_ABI_ANCHOR` is what keeps that true once a consumer compiles `-fvisibility=hidden`:
 * without it the weak definition goes hidden and the coalescing stops (measured, same harness).
 *
 * @note Moving this definition back into a `.cpp` reintroduces the split with no diagnostic
 *       from any tool. The audit that found it proposed exactly that ("an out-of-line accessor
 *       defined in the archive") as the fix; measured, an archive-defined accessor's
 *       function-local `thread_local` returns **two different addresses** in the two images, for
 *       the same reason. Storage that must be unique per process belongs in the header.
 *
 * @note **Nothing above this definition may name `current` in code** -- no member defined in the
 *       class body, no template written before this line (Huly QB-1246). On Darwin this variable --
 *       dynamically initialised -- is reached through a *replaceable* thread wrapper (`_ZTW`), and
 *       clang gives that wrapper the linkage of the declaration the translation unit's first emitted
 *       reference names: a reference written above names the in-class declaration, which is not
 *       `inline`, and emits the wrapper `external` (strong); one written below names this definition
 *       and emits it `weak_odr hidden` (`nm -m`: `weak private external`). Two translation units of
 *       one link whose first reference came from above are a duplicate `thread-local wrapper routine
 *       for qb::io::async::listener::current`: a warning of the runner image's ld, a link error on a
 *       developer's Mac (measured with clang 19 `-target arm64-apple-macos14` and Apple clang 21 on
 *       the macOS runner; on Linux it is `weak_odr` in every case, and the MSVC ABI has none). That is why
 *       `release()` is defined below; `scripts/check-header-linkage.py` (THREAD-WRAP) refuses the shape.
 */
namespace detail {
#ifdef QB_LISTENER_ASAN
/// The sanitize lane's guard (Huly QB-1246): once its destructor has completed, the thread's `listener::current`
/// is POISONED, so whatever reads it afterwards -- the destructor of an object with static storage, of a
/// thread_local built before it -- is a use-after-poison the lane reports, instead of a read of a destroyed object
/// nothing sees. No data member: the layout of `listener` is the same in a sanitized and an unsanitized
/// translation unit. A function-local thread_local built BEFORE the listener, in the initialiser of
/// `listener::current`, so that it is destroyed AFTER it. (clang reports the touch at every optimisation level,
/// MSVC too; g++ only where the access is not folded into a direct thread-local reference, which is `-O0`.)
struct listener_poisoner {
    listener_poisoner() noexcept {}
    ~listener_poisoner() noexcept;
};
inline void
arm_listener_poisoner() noexcept {
    thread_local listener_poisoner poisoner;
    (void) poisoner;
}
#else
inline void
arm_listener_poisoner() noexcept {}
#endif
} // namespace detail

QB_ABI_ANCHOR inline thread_local listener listener::current{(detail::arm_listener_poisoner(), listener::thread_current_tag{})};

#ifdef QB_LISTENER_ASAN
inline detail::listener_poisoner::~listener_poisoner() noexcept {
    __asan_poison_memory_region(&listener::current, sizeof(listener));
}
#endif

// After `listener::current`'s definition, never in the class: the note on its declaration, and on the definition above.
inline void
listener::release(IRegisteredKernelEvent *kevent) {
    if (!kevent)
        return;
    if (kevent->_detached_by_clear) // on no listener's list any more: no listener is reached, its own may be gone
        kevent->_detached_by_clear = false;
    else if (!current._unlink(kevent))
        return; // not registered here (a second release)
    delete kevent;
}

namespace detail {
/// The thread's loop, to arm a watcher on: read on the thread's record while the thread's listener has its loop --
/// no thread_local with a destructor on the way, which is what `sleep()`, the `wait_*()` family, the timed channel
/// operations, the timeouts and qb-core's `ask` paid on every call. Otherwise the listener is asked: it builds its
/// loop on a thread that has none yet, and stops the process by name once it is gone (`listener::loop()`).
[[nodiscard]] inline ev::loop_ref
thread_listener_loop() {
    if (struct ev_loop *const raw = thread_loop().raw) [[likely]]
        return ev::loop_ref(raw);
    return listener::current.loop();
}
} // namespace detail

/**
 * @brief Initialize the asynchronous event system for the current thread.
 * @details Ensures that `listener::current` is available and ready for use.
 *          Typically called once per thread that will use `qb-io` asynchronous features standalone.
 *          Not usually needed when using `qb-core` as `qb::Main` handles this for its `VirtualCore` threads.
 * @ingroup Async
 */
inline void
init() noexcept {
    // No-op: listener::current is a thread_local that initialises itself
    // automatically.  Code that needs an explicitly clean event-loop state
    // (e.g. unit-test TearDown) should call listener::current.clear()
    // directly instead of relying on this function.
    //
    // WARNING: do NOT call listener::current.clear() here.  async::init()
    // is called from multi-threaded test fixtures that have already created
    // server/client objects in the same thread-local listener.  Clearing the
    // listener destroys those objects' registered kernel events (their
    // io::base::_async_event references become dangling), which silently
    // breaks accept sockets, I/O watchers, and timeouts.
}

/// The gate of every free function that RUNS the thread's loop (`async::run()`, `run_once()`, `run_until()`,
/// `run_for()`, `run_sync()`): not after the thread's `listener::current` is destroyed -- a run needs the loop, and
/// the request stops the process on the line that names `api_name` (Huly QB-1246) -- and not from inside a
/// coroutine drain. Both are read on the thread's record: the listener's members, which a core calls on every
/// pass, carry neither test.
inline void
ensure_not_inside_ready_drain(char const *api_name) {
    detail::thread_loop_t const &thread = detail::thread_loop();
    if (thread.phase == detail::thread_loop_phase::gone) [[unlikely]]
        detail::refuse_after_thread_loop(api_name);
    if (thread.scheduler != nullptr && thread.scheduler->is_draining_ready()) {
#ifndef NDEBUG
        assert(!thread.scheduler->is_draining_ready()
               && "async::run/run_once/run_until must not be called from inside a "
                  "coroutine or actor handler already executing under "
                  "CoroutineScheduler::run_ready()");
#endif
        throw std::logic_error(std::string(api_name)
                               + " must not be called from inside a coroutine or actor "
                                 "handler already executing under run_ready()");
    }
}

/**
 * @brief Run the event loop for the current thread.
 *
 * Executes the current thread's `listener::current.run(flag)`.
 * This is the primary way to process asynchronous events in a standalone `qb-io` application.
 *
 * @param flag The libev run flag (e.g., `EVRUN_NOWAIT`, `EVRUN_ONCE`). See `listener::run()`.
 * @return The number of events that were invoked during this run.
 * @ingroup Async
 */
inline std::size_t
run(int flag = 0) {
    ensure_not_inside_ready_drain("async::run()");
    listener::current.run(flag);
    listener::current.relay_break(); // a pass of a pump: a break asked under it is the run's to hear (`break_one()`)
    return listener::current.nb_invoked_event();
}

/**
 * @brief Defer `func` to run at the tail of the current event-loop turn.
 * @ingroup Async
 *
 * The one correct primitive for "continue after this event handler unwinds":
 * `func` runs once, after every libev watcher for this turn has returned, never
 * inline from the call: once the handler that deferred it has unwound, or at the tail of a
 * turn that handler runs itself. Forwards to the current
 * thread's `listener::defer()` — see it for the full semantics and lifetime rules.
 *
 * Choose deliberately:
 *  - **`defer(func)`** — run on the next loop turn, no delay, no timer. Use when a
 *    handler must destroy/replace the object it is running on (e.g. a reconnect).
 *  - **`callback(func, delay)`** with `delay > 0` — run after a real timed delay.
 *  - **`func()`** directly — run inline, right now.
 *
 * Note: `callback(func)` (and `callback(func, d<=0)`) run `func` INLINE, not on the
 * next turn — do not use them to break re-entrancy; use `defer()` for that.
 */
template <typename _Func>
inline void
defer(_Func &&func) {
    listener::current.defer(std::forward<_Func>(func));
}

/**
 * @brief Run the event loop once for the current thread, waiting for at least one event.
 *
 * Equivalent to `listener::current.run(EVRUN_ONCE)`.
 *
 * @warning With libev monotonic clock and timerfd enabled (`QB_EV_USE_TIMERFD=ON`),
 *          `EVRUN_ONCE` can block for libev's internal maximum waittime when there are
 *          only `ev_io` watchers and no heap timers (`timercnt == 0`). Prefer
 *          `run_until` / `run(EVRUN_NOWAIT)` in pumps, or keep timerfd disabled (default).
 *
 * @return The number of events invoked.
 * @ingroup Async
 */
inline std::size_t
run_once() {
    ensure_not_inside_ready_drain("async::run_once()");
    listener::current.run(EVRUN_ONCE);
    return listener::current.nb_invoked_event();
}

/**
 * @brief Run the event loop for the current thread until a condition is met.
 *
 * Repeatedly calls `listener::current.run(EVRUN_NOWAIT)` as long as the `status` is true.
 * @param status Reference to a boolean condition. The loop continues as long as `status` is true.
 * @return The total number of events invoked across all `run(EVRUN_NOWAIT)` calls.
 * @ingroup Async
 */
inline std::size_t
run_until(bool const &status) {
    ensure_not_inside_ready_drain("async::run_until()");
    std::size_t nb_invoked_events = 0;
    while (status) {
        listener::current.run(EVRUN_NOWAIT);
        const auto processed = listener::current.nb_invoked_event();
        nb_invoked_events += processed;
        if (!processed) {
            // A pass that did nothing parks the loop in its own backend wait: a watcher that
            // fires -- a socket turning readable, a timer -- ends the park at poll latency, and
            // the 1 ms cap only bounds a wait that nothing ends. The blind `sleep_for(50us)`
            // this replaces returned 1-2.5 ms later on Windows (a sub-millisecond wait is served
            // at the millisecond plus the kernel's coalescing, Huly QB-196) and ~110 us later on
            // Linux, with the reply already sitting in the socket: a pgsql FETCH loop paid 2 ms
            // per round trip on a 300 us path, and a loop with nothing to do woke ~9000 times a
            // second on Linux (Huly QB-1103). `status` is read without synchronisation, so it is
            // flipped on this thread -- by a handler, a callback or a coroutine the loop runs --
            // and nothing legitimate waits on the cap. The cap timer is a heap timer, so a
            // timerfd build never computes MAX_BLOCKTIME2 here.
            nb_invoked_events += listener::current.run_once_for(std::chrono::milliseconds(1));
        }
    }
    listener::current.relay_break(); // a break asked under this pump is the run's to hear (`break_one()`)
    return nb_invoked_events;
}

/**
 * @brief Request the parent (current thread's) event loop to break.
 * @details Calls `listener::current.break_one()`: see it for which run that ends, and for how the request is kept
 *          across a pump.
 * @ingroup Async
 */
inline void
break_parent() noexcept {
    listener::current.break_one();
}

namespace detail {
/// INTERNAL (Huly QB-251): ticks of `qb::tsc_ticks()` per nanosecond as `loop` calibrated them at construction
/// (`qb::detail::calibrate_tsc_per_ns`), 0 when the counter does not move. A `VirtualCore` converts its idle floors
/// into ticks with it, so a thread calibrates once. Not part of the public API.
inline double
tsc_per_ns(listener const &loop) noexcept {
    loop._require_loop("the counter's calibration of listener::current"); // calibrated where the loop is built
    return loop._tsc_per_ns;
}

/// INTERNAL (Huly QB-1246): feeds `revents` to an io object's own watcher, so that its loop's next dispatch runs the
/// object's handler -- how `disconnect()` asks for its teardown. DROPPED once the thread's loop is gone: an object
/// with static storage that disconnects from its destructor (a module's client does) runs after its thread's
/// `listener::current`; nothing will dispatch its watcher again, and the loop the event would be queued on is freed
/// memory -- until 3.4 a write into it at every such exit. The caller sets its own state first: `disconnect_now()`
/// then completes the teardown in the call, and a plain `disconnect()` leaves the transport to the object's
/// destructor, as for an object destroyed while connected.
template <typename _Watcher>
inline void
feed_own_watcher(_Watcher &watcher, int const revents) noexcept {
    if (!thread_loop_gone()) [[likely]]
        watcher.feed_event(revents);
}
} // namespace detail

} // namespace qb::io::async

#endif // QB_IO_ASYNC_LISTENER_H_
