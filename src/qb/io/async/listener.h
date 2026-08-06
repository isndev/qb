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
#include <qb/utility/branch_hints.h>
#include <qb/utility/type_traits.h>
#include <thread>
#include <vector>
#include "event/base.h"
#include "coroutine/scheduler.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(QB_DEBUG_CORO_LIFECYCLE) && QB_DEBUG_CORO_LIFECYCLE
// Standard C++20 __VA_OPT__ elides the comma when no trailing args are passed
// (MSVC needs the conformant preprocessor /Zc:preprocessor, enabled by qb's build).
#define QB_LISTENER_TRACE(fmt, ...) std::fprintf(stderr, "[listener] " fmt "\n" __VA_OPT__(, ) __VA_ARGS__)
#else
#define QB_LISTENER_TRACE(fmt, ...) ((void) 0)
#endif

namespace qb::io::async {

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
 *         `epoll_wait` / `qev_run` on one thread while another thread closes the same loop or its
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
     * @note This is typically initialized automatically when `qb::Main` starts its `VirtualCore` threads,
     *       or by calling `qb::io::async::init()` for standalone `qb-io` usage.
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
                    ::operator delete(p, sizeof(RegisteredKernelEvent), std::align_val_t{alignof(RegisteredKernelEvent)});
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
    ev::dynamic_loop _loop; /**< The libev event loop */

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
    std::deque<std::function<void()>> _deferred;
    bool                              _in_defer_drain = false;

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
    // current turn, never re-entrant from inside a handler" contract, and it holds
    // identically under `run(0)`, `EVRUN_ONCE` and `EVRUN_NOWAIT`.
    //
    // Leftovers (a callback that itself defers — excluded from this pass by the
    // snapshot bound) must NOT be re-fed: `qev_feed_event` lands in the pass that is
    // still draining and would run them in the same turn. They are re-armed with a
    // 0-delay one-shot instead, which expires on the next loop iteration.
    ev::timer _defer_wake;

    void
    _on_defer_wake(ev::timer &, int) {
        const std::size_t n = _drain_deferred();
        _nb_invoked_events += n;
        _total_events_processed += n;
        if (!_deferred.empty() && !_defer_wake.is_active())
            _defer_wake.start(0.); // next turn, never this pass
    }

    // Run every callback queued BEFORE this pass; one that itself defers is left
    // for the next loop turn (the snapshot count bounds the drain and stops a
    // self-re-deferring callback from starving the loop). Never runs re-entrantly.
    std::size_t
    _drain_deferred() {
        if (_in_defer_drain || _deferred.empty())
            return 0; // branch-only fast path when nothing is queued
        // RAII re-entrancy guard (mirrors CoroutineScheduler::run_ready): the flag is
        // cleared on every exit path, so a nested run()/drain is refused and the guard
        // can never be stranded — even if a container op below were to throw.
        struct DrainGuard {
            bool &flag;
            explicit DrainGuard(bool &f) noexcept
                : flag(f) {
                flag = true;
            }
            ~DrainGuard() noexcept {
                flag = false;
            }
        } guard{_in_defer_drain};
        std::size_t n       = _deferred.size();
        std::size_t drained = 0;
        while (n-- > 0 && !_deferred.empty()) {
            std::function<void()> fn = std::move(_deferred.front());
            _deferred.pop_front();
            if (fn) {
                // Runs inside the loop: an escaping exception would unwind through
                // libev's C frames (UB). Contain it, exactly as `Timeout` does.
                try {
                    fn();
                } catch (...) {
                }
                ++drained;
            }
        }
        return drained;
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
        const char *env = std::getenv("QB_EV_BACKEND");
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
        if (!(qev_supported_backends() & req)) {
            QB_LOG_INFO("[qb-io] QB_EV_BACKEND='" << env << "' not built into libev; using auto");
            return EVFLAG_AUTO;
        }
        /* Probe: a backend can be compiled in yet fail at runtime (io_uring under a
         * restrictive seccomp profile, kqueue on some sandboxes, ...). Create and
         * immediately destroy a throwaway loop so the real loop below never throws. */
        if (struct qev_loop *probe = qev_loop_new(req)) {
            qev_loop_destroy(probe);
            return req;
        }
        QB_LOG_INFO("[qb-io] QB_EV_BACKEND='" << env << "' unavailable at runtime; using auto");
        return EVFLAG_AUTO;
    }

public:
    /**
     * @brief Constructor
     *
     * Creates a new listener with a dynamic event loop. The backend is normally
     * auto-detected (libev's `EVFLAG_AUTO`: e.g. epoll/io_uring on Linux, kqueue/
     * select on macOS). It can be pinned for testing/benchmarking via the
     * `QB_EV_BACKEND` environment variable (see `_resolve_backend_flags()`).
     */
    listener()
        : _loop(_resolve_backend_flags())
        , _defer_wake(_loop) {
        _defer_wake.set<listener, &listener::_on_defer_wake>(this);
        // Lowest priority: libev invokes pendings highest-priority-first, so the
        // drain lands after every other watcher pending in the same iteration.
        // Safe here — the watcher is neither active nor pending at construction.
        qev_set_priority(static_cast<qev_timer *>(&_defer_wake), EV_MINPRI);
    }

    /**
     * @brief libev backend actually in use by this listener's loop.
     * @return One of the `EVBACKEND_*` values (e.g. `EVBACKEND_EPOLL`).
     */
    [[nodiscard]] inline unsigned int
    backend() const noexcept {
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
     * libev, `qev_run` can choose a ~1.5e6 s waittime when `timercnt == 0`, which would wedge
     * thread teardown (`~listener` / explicit `clear()`).
     * @note This is automatically called by the listener's destructor.
     */
    void
    clear() {
        QB_LISTENER_TRACE("clear() begin registeredEvents=%zu has_coro_scheduler=%d", _registered_count, _coro_scheduler != nullptr);
        // Drop pending deferred callbacks: they must not run against a listener being
        // torn down (they may touch registrations we are about to destroy). Each
        // std::function's destructor releases its captured state (e.g. a shared_ptr
        // held only to keep a target alive across the defer) cleanly — no leak, no
        // execution. Watchers are detached below before the flush runs(), so no WATCHER
        // can post anything here; a released closure's destructor still can (see below).
        //
        // Drain by SWAPPING the queue out, never `_deferred.clear()` in place. Releasing a
        // closure's captured state runs arbitrary destructors — a captured shared_ptr can be
        // the last reference to an object whose teardown calls `defer()` again — and a
        // push_back into a deque that is halfway through its own clear() is undefined. After
        // the swap the member is empty, so any such re-entrant defer lands in a fresh queue
        // and is picked up by the next turn of this loop (in practice one extra pass).
        while (!_deferred.empty()) {
            std::deque<std::function<void()>> dropped;
            dropped.swap(_deferred);
            dropped.clear();
        }
        _defer_wake.stop(); // also clears a pending feed; nothing is left to drain
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
            // re-enters `unregisterEvent()` on ANOTHER still-linked node, freeing it. A
            // cached `next` pointing at that node then becomes dangling and the next
            // `cur->stop()` is a use-after-free. Popping through `_unlink()` also keeps
            // `_registered_count` exact — the previous "null the head + zero the count"
            // prelude let such a re-entrant `_unlink` underflow the counter to SIZE_MAX.
            while (_registered_head) {
                IRegisteredKernelEvent *cur = _registered_head;
                (void) _unlink(cur);
                cur->stop();
                cur->_detached_by_clear = true;
                if (cur->_destroy_owner && !_is_dispatching(cur)) {
                    // Loop-owned, self-deleting handler (e.g. an `async::callback`
                    // Timeout) whose one-shot timer never fired: nothing else will
                    // reclaim it. Destroy the owner now — its `~base` re-enters
                    // `unregisterEvent(cur)` and frees this wrapper via the
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
            for (int i = 0; i < 4; ++i)
                run(EVRUN_NOWAIT);
        }
        QB_LISTENER_TRACE("clear() end (scheduler NOT reset)");
    }

    /**
     * @brief Destructor
     *
     * Cleans up by calling `clear()` to remove all registered events and their watchers.
     */
    ~listener() noexcept {
        QB_LISTENER_TRACE("~listener() begin");
        clear();
        QB_LISTENER_TRACE("~listener() about to reset _coro_scheduler");
        reset_coro_scheduler();
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
        // `ev::stat` observer, …), and libev is built as C (qb/src/qb/vendor/qev, LANGUAGES C):
        // letting an exception unwind through `qev_invoke_pending`/`qev_run` is UB — it
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
     */
    void
    unregisterEvent(IRegisteredKernelEvent *kevent) {
        if (!kevent)
            return;
        if (_unlink(kevent) || kevent->_detached_by_clear) {
            kevent->_detached_by_clear = false;
            delete kevent;
        }
    }

    /**
     * @brief Get a reference to the underlying libev event loop.
     * @return `ev::loop_ref` (a reference wrapper to `qev_loop*`) for this listener.
     * @details Useful for advanced direct interaction with libev if needed, though most
     *          operations are handled through the listener's API.
     */
    [[nodiscard]] inline ev::loop_ref
    loop() const {
        return _loop;
    }

    /**
     * @brief Run the event loop to process pending events.
     *
     * Executes the event loop with the specified libev run flag.
     * This call blocks or returns based on the flag and event activity.
     * It also resets the `_nb_invoked_events` counter before running.
     *
     * After processing libev events, any ready coroutines are also executed.
     *
     * @param flag The libev run flag (e.g., `EVRUN_NOWAIT` to check once and return,
     *             `EVRUN_ONCE` to wait for and process one event block, `0` for default blocking run).
     *             Default is `0`, which means `qev_run` will block until `qev_break` is called or no active watchers remain.
     */
    inline void
    run(int flag = 0) {
        _nb_invoked_events = 0;
        _loop.run(flag);

        // Deferred callbacks: continuations of the dispatch that just unwound.
        // Drained before coroutines so a `defer()` that wakes a coroutine is
        // picked up by `run_ready()` in the same turn.
        const std::size_t deferred_count = _drain_deferred();
        _nb_invoked_events += deferred_count;
        _total_events_processed += deferred_count;

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
        if (_coro_scheduler) {
            std::size_t coro_count = _coro_scheduler->run_ready(kMaxCoroutineResumesPerTurn);
            _nb_invoked_events += coro_count;
            _total_events_processed += coro_count;
        }
    }

    /// Upper bound on coroutine resumes per `run()` turn — see the `run_ready()` call site in
    /// `run()` for why an unbounded drain lets two mutually-resuming coroutines wedge the loop.
    static constexpr std::size_t kMaxCoroutineResumesPerTurn = 65536;

    /**
     * @brief Queue `fn` to run once, at the tail of the current `run()` turn.
     *
     * The callback executes after every libev watcher for this turn has returned,
     * so it never runs re-entrantly from inside an event handler. Use it when a
     * handler must continue work that is unsafe inline — above all destroying or
     * replacing the object the handler is currently executing on (a reconnect that
     * frees+recreates its connection). It is the non-coroutine twin of the
     * `co_await sleep(0ms)` cooperative yield.
     *
     * Prefer it over `async::callback(fn, tiny_delay)` for "next turn" semantics:
     * no libev timer, no heap `Timeout`, no arbitrary delay. Captured state is
     * released when the callback fires OR when the loop is torn down (`clear()`),
     * whichever comes first — so a strong (`shared_ptr`) capture keeps its target
     * alive exactly until then, leak-free. Same-thread only.
     *
     * @note The queued `fn` is drained by `run()`; drive the loop with a NOWAIT pump
     *       (`async::run_until`/`run_sync`, or a VirtualCore tick — which gates on
     *       `has_deferred()` so a bare `defer()` still pumps), not a single blocking
     *       `run(EVRUN_ONCE)` that could park before the drain. A `defer()` issued
     *       *from a coroutine* (which runs after the drain) fires on the next turn.
     * @note Only the drain contains a throwing `fn`; the enqueue here is a plain
     *       allocation and may itself throw `std::bad_alloc` under OOM.
     */
    void
    defer(std::function<void()> fn) {
        _deferred.push_back(std::move(fn));
        // Arm the in-loop drain hook (see `_defer_wake`) so the queue is serviced even
        // when `run()` never returns. Skipped while draining: a callback that defers
        // must land on the NEXT turn, and `_on_defer_wake` re-arms it with a 0-delay
        // one-shot for exactly that. The pending/active tests keep a burst of defers to
        // a single queue entry.
        if (!_in_defer_drain && !_defer_wake.is_pending() && !_defer_wake.is_active())
            _defer_wake.feed_event(EV_CUSTOM);
    }

    /**
     * @brief Request the event loop to break out of its current `run()` cycle.
     * @details This signals the libev loop to stop processing further events in the current
     *          `run()` invocation. If `run()` was called with default blocking behavior,
     *          it will return after the current event (if any) is processed.
     */
    inline void
    break_one() {
        _loop.break_loop();
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
     * @brief Get the total number of events processed since listener creation.
     * @return The cumulative count of all events that have been processed by this listener.
     * @note This counter is never reset and provides a lifetime metric for the listener.
     *       Useful for monitoring and debugging purposes.
     */
    [[nodiscard]] inline std::size_t
    total_events_processed() const {
        return _total_events_processed;
    }

    /**
     * @brief Get the number of currently registered event handlers.
     * @return The total number of active event watchers managed by this listener.
     */
    [[nodiscard]] inline std::size_t
    size() const {
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
        return !_deferred.empty();
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
        if (!_coro_scheduler) {
            _coro_scheduler = std::make_unique<CoroutineScheduler>(_loop);
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
        return _coro_scheduler != nullptr;
    }

    /**
     * @brief Reset the coroutine scheduler (for test isolation).
     *
     * Destroys the current scheduler and clears the thread-local current pointer.
     * The next call to coro_scheduler() will create a new scheduler.
     * Use in test TearDown to avoid leftover state affecting the next test.
     */
    inline void
    reset_coro_scheduler() {
        QB_LISTENER_TRACE("reset_coro_scheduler() begin has_scheduler=%d", _coro_scheduler != nullptr);
        if (_coro_scheduler) {
            _coro_scheduler->destroy_all_suspended();
        }
        CoroutineScheduler::set_current(nullptr);
        _coro_scheduler.reset();
        QB_LISTENER_TRACE("reset_coro_scheduler() end");
    }
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
 */
QB_ABI_ANCHOR inline thread_local listener listener::current = {};

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

inline void
ensure_not_inside_ready_drain(char const *api_name) {
    if (listener::current.has_coro_scheduler() && listener::current.coro_scheduler().is_draining_ready()) {
#ifndef NDEBUG
        assert(!listener::current.coro_scheduler().is_draining_ready()
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
    return listener::current.nb_invoked_event();
}

/**
 * @brief Defer `func` to run at the tail of the current event-loop turn.
 * @ingroup Async
 *
 * The one correct primitive for "continue after this event handler unwinds":
 * `func` runs once, after every libev watcher for this turn has returned, so it
 * never executes re-entrantly from inside a handler. Forwards to the current
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
 *          only `qev_io` watchers and no heap timers (`timercnt == 0`). Prefer
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
            // Avoid busy spinning when the loop is temporarily idle.
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    }
    return nb_invoked_events;
}

/**
 * @brief Request the parent (current thread's) event loop to break.
 * @details Calls `listener::current.break_one()`.
 * @ingroup Async
 */
inline void
break_parent() noexcept {
    listener::current.break_one();
}

} // namespace qb::io::async

#endif // QB_IO_ASYNC_LISTENER_H_
