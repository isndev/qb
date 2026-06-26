/**
 * @file qb/io/async/coroutine/cancellation.h
 * @brief Cancellation support for coroutines
 *
 * This file provides cancellation mechanisms for coroutines:
 * - cancellation_token: Signal cancellation to operations
 * - cancellable_task: Wrapper that checks cancellation
 * - check_cancelled: Awaiter for cancellation
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
 * @ingroup Coroutine
 */

#ifndef QB_IO_ASYNC_COROUTINE_CANCELLATION_H
#define QB_IO_ASYNC_COROUTINE_CANCELLATION_H

#include "scheduler.h"
#include "task.h"
#include "utils.h"
// No <mutex> / <atomic> needed: cancellation_token is designed for same-thread
// use only (one qb-io VirtualCore thread). Cross-thread cancellation must go
// through the qb actor event system — an actor on Thread B sends a Cancel event
// to the actor on Thread A, which then calls token.cancel() on its own thread.
#include <qb/system/time.h> // qb::duration
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace qb::io::async {

/**
 * @brief Exception thrown when operation is cancelled
 */
class cancelled_error : public std::runtime_error {
public:
    cancelled_error()
        : std::runtime_error("Operation was cancelled") {}
};

// Forward declaration
class cancellation_token;

/**
 * @brief Tag type to construct an empty (state-less) cancellation_token without allocation.
 * @details
 * An empty token owns no shared state, never cancels, and allocates nothing. It is the
 * lazy placeholder used by `qb::Actor`'s coroutine scope until a real token is first
 * needed (so actors that never spawn a scoped coroutine pay zero cost). Querying
 * (`is_cancelled`) or `cancel()`-ing an empty token is a safe no-op; never pass an empty
 * token to an awaiter — it would suspend forever.
 */
struct null_token_t {
    explicit constexpr null_token_t() noexcept = default;
};

/** @brief Inline constexpr tag value; see `qb::io::async::null_token_t`. */
inline constexpr null_token_t null_token{};

/**
 * @brief Token for coordinating cancellation across coroutines
 *
 * A cancellation_token can be shared between multiple coroutines.
 * When cancel() is called, all coroutines checking this token
 * will be notified and can gracefully exit.
 *
 * Usage:
 * @code
 * cancellation_token token;
 *
 * // Spawn worker
 * coro_scheduler().spawn(worker_task(token));
 *
 * // Later, cancel it
 * token.cancel();
 * @endcode
 */
class cancellation_token {
public:
    /// Opaque registration id returned by `on_cancel`, accepted by `remove_on_cancel`.
    /// `0` means "not registered" (empty token, or fired inline because already cancelled).
    using id_type = std::uint64_t;

    // Plain bool and no mutex: single-thread cooperative scheduler.
    // cancel() and on_cancel() are always called on the same VirtualCore thread.
    struct state {
        bool    cancelled{false};
        id_type next_id{0};
        // Keyed callbacks so a completing awaiter can DEREGISTER itself (remove_on_cancel)
        // instead of leaving a dead entry behind. Without this a long-lived (actor-scope)
        // token accumulated one std::function per sleep/ask/cancellable for the actor's
        // whole life — unbounded growth in any `co_await ctx.sleep(...)` loop.
        std::vector<std::pair<id_type, std::function<void()>>> callbacks;
    };

private:
    std::shared_ptr<state> _state;

public:
    cancellation_token()
        : _state(std::make_shared<state>()) {}

    /**
     * @brief Construct an empty token: no shared state, no allocation.
     * @details Use `qb::io::async::null_token`. An empty token never cancels and is the
     *          lazy placeholder for `Actor`'s coroutine scope. @see null_token_t
     */
    explicit cancellation_token(null_token_t) noexcept
        : _state(nullptr) {}

    cancellation_token(const cancellation_token &)            = default;
    cancellation_token(cancellation_token &&)                 = default;
    cancellation_token &operator=(const cancellation_token &) = default;
    cancellation_token &operator=(cancellation_token &&)      = default;

    /** @brief True iff this token owns shared state (i.e. is **not** empty). */
    [[nodiscard]] explicit
    operator bool() const noexcept {
        return _state != nullptr;
    }

    /**
     * @brief Cancel all operations using this token.
     *
     * Must be called on the same VirtualCore thread as the coroutines using
     * this token. For cross-thread cancellation, send a qb actor event to the
     * owning thread and call cancel() from its event handler.
     */
    void
    cancel() {
        if (_state && !_state->cancelled) {
            _state->cancelled = true;
            auto callbacks    = std::move(_state->callbacks);
            for (auto &cb : callbacks)
                if (cb.second)
                    cb.second();
        }
    }

    bool
    is_cancelled() const noexcept {
        return _state && _state->cancelled;
    }

    /**
     * @brief Register a callback invoked when cancel() is called.
     *
     * If already cancelled, the callback is invoked immediately (same thread).
     *
     * @return A registration id for `remove_on_cancel`, or `0` if the token is empty or was
     *         already cancelled (callback ran inline / was dropped — nothing to deregister).
     * @note Awaiters that complete **normally** (without cancellation) should deregister via
     *       `remove_on_cancel(id)` in their teardown, otherwise a long-lived token accumulates
     *       dead callbacks without bound (e.g. a `co_await ctx.sleep(...)` loop on the actor scope).
     *       Fire-and-forget cleanup callbacks (that genuinely live for the token's lifetime) may
     *       ignore the returned id.
     */
    id_type
    on_cancel(std::function<void()> callback) const {
        if (!_state) // empty token never cancels — drop the callback.
            return 0;
        if (_state->cancelled) {
            callback();
            return 0;
        }
        const id_type id = ++_state->next_id; // 0 reserved for "not registered".
        _state->callbacks.emplace_back(id, std::move(callback));
        return id;
    }

    /**
     * @brief Deregister a callback previously registered with `on_cancel`.
     * @param id The id returned by `on_cancel` (a `0` id is ignored).
     * @details Idempotent and O(n) over the (normally tiny) live-callback set; safe to call
     *          after cancellation fired (the entry is already gone → no-op). Same-thread only.
     */
    void
    remove_on_cancel(id_type id) const noexcept {
        if (!_state || !id)
            return;
        auto &cbs = _state->callbacks;
        for (auto it = cbs.begin(); it != cbs.end(); ++it) {
            if (it->first == id) {
                *it = std::move(cbs.back()); // swap-with-back: order is irrelevant on cancel.
                cbs.pop_back();
                return;
            }
        }
    }

    void
    throw_if_cancelled() const {
        if (_state && _state->cancelled)
            throw cancelled_error();
    }

    std::shared_ptr<state>
    get_state() const {
        return _state;
    }
};

/**
 * @brief Awaiter that suspends until cancellation is requested
 *
 * Usage:
 * @code
 * co_await check_cancelled(token);  // Throws if cancelled
 * @endcode
 */
struct cancellation_awaiter {
    cancellation_token      token;
    std::coroutine_handle<> _handle;
    // Shared liveness flag: the on_cancel callback outlives this awaiter inside
    // the token's callback list. If the awaiting coroutine frame is destroyed
    // while still suspended here (e.g. a when_any loser or scope cancellation),
    // a later cancel() would otherwise call schedule_via_current() on a freed
    // handle. The destructor clears the flag so the stale callback no-ops.
    std::shared_ptr<bool>       _alive     = std::make_shared<bool>(true);
    cancellation_token::id_type _cancel_id = 0; ///< on_cancel registration, deregistered on teardown.

    // Explicit constructor: the user-declared destructor below makes this type a
    // non-aggregate, so the brace-init in check_cancelled() needs a constructor.
    cancellation_awaiter(cancellation_token t, std::coroutine_handle<> h)
        : token(std::move(t))
        , _handle(h) {}

    [[nodiscard]] bool
    await_ready() const {
        return token.is_cancelled();
    }

    void
    await_suspend(std::coroutine_handle<> h) {
        _handle    = h;
        auto alive = _alive;
        _cancel_id = token.on_cancel([h, alive]() {
            if (*alive)
                schedule_via_current(h);
        });
    }

    void
    await_resume() {
        token.throw_if_cancelled();
    }

    ~cancellation_awaiter() {
        if (_alive)
            *_alive = false;
        // Deregister so a normally-completed wait leaves no dead callback on a long-lived token.
        token.remove_on_cancel(_cancel_id);
    }
};

/**
 * @brief Create awaiter that checks/waits for cancellation
 * @param token The cancellation token to check
 * @return Awaiter that throws cancelled_error when cancelled
 * @ingroup Coroutine
 */
inline cancellation_awaiter
check_cancelled(const cancellation_token &token) {
    return cancellation_awaiter{token, std::coroutine_handle<>{}};
}

/**
 * @brief Awaiter that yields control back to scheduler
 * Checks cancellation each time it resumes
 *
 * Single-thread: enqueues the current handle at the end of the ready queue
 * without allocating an intermediate coroutine.
 */
struct yield_awaiter {
    cancellation_token token;

    [[nodiscard]] bool
    await_ready() const {
        return false;
    }

    void
    await_suspend(std::coroutine_handle<> h) {
        enqueue_for_later_via_current(h);
    }

    void
    await_resume() {
        token.throw_if_cancelled();
    }
};

/**
 * @brief Yield control and check cancellation
 * @param token Cancellation token to check
 * @return Awaiter that yields and throws if cancelled
 * @ingroup Coroutine
 */
inline yield_awaiter
yield_or_cancel(const cancellation_token &token) {
    return yield_awaiter{token};
}

/**
 * @brief Wrapper for cancellable operations with automatic cleanup
 * @tparam T Return type of the wrapped task
 *
 * Stores the inner task and shared state in a shared_ptr so that the spawned
 * task_runner coroutine (a static function) can safely access it even after the
 * outer coroutine resumes via cancellation and the awaiter is destroyed.
 * Never use a [this] or [&] lambda as a coroutine body — the compiler stores
 * a pointer to the local lambda, not a copy, causing dangling access on resume.
 */
template <typename T>
class cancellable_operation {
    struct shared_state {
        task<T>                 inner_task;
        std::optional<T>        result;
        std::exception_ptr      error;            ///< inner task exception, propagated on resume
        bool                    task_done{false}; // single-thread: no atomic needed
        std::coroutine_handle<> continuation;
        // The detached task_runner driving inner_task. On early cancel we tear it
        // down through this handle (then drop inner_task) so both frames + the
        // inner op's watcher are reclaimed immediately instead of lingering.
        std::coroutine_handle<> runner_handle{};

        explicit shared_state(task<T> &&t)
            : inner_task(std::move(t)) {}
    };

    std::shared_ptr<shared_state> _shared;
    cancellation_token            _token;
    bool                          _throw_on_cancel;

public:
    cancellable_operation(task<T> &&t, cancellation_token token, bool throw_on_cancel = true)
        : _shared(std::make_shared<shared_state>(std::move(t)))
        , _token(std::move(token))
        , _throw_on_cancel(throw_on_cancel) {}

    struct awaiter {
        std::shared_ptr<shared_state> state;
        cancellation_token            token;
        bool                          throw_on_cancel;
        cancellation_token::id_type   _cancel_id = 0;

        // User-declared dtor below makes this a non-aggregate → provide the ctor the
        // brace-init in operator co_await() relies on.
        awaiter(std::shared_ptr<shared_state> s, cancellation_token t, bool toc)
            : state(std::move(s))
            , token(std::move(t))
            , throw_on_cancel(toc) {}

        ~awaiter() {
            // Deregister the cancel hook on normal completion so a long-lived token does
            // not retain a dead callback per cancellable() wrap.
            token.remove_on_cancel(_cancel_id);
        }

        [[nodiscard]] bool
        await_ready() const {
            return token.is_cancelled() && throw_on_cancel;
        }

        void
        await_suspend(std::coroutine_handle<> h) {
            state->continuation  = h;
            state->runner_handle = coro_scheduler().spawn_tracked(task_runner(state));
            if (throw_on_cancel) {
                // Route the cancel wake-up through the shared task_done guard so
                // it cannot double-resume (or resume a destroyed frame) after the
                // task_runner path already completed. Capture the shared_state
                // (outlives the awaiter), never a bare handle.
                auto s     = state;
                _cancel_id = token.on_cancel([s]() {
                    if (!s->task_done) {
                        s->task_done = true;
                        // Neither the runner nor the inner task will ever deliver now.
                        // Tear them down so both frames + the inner op's watcher are
                        // reclaimed immediately instead of lingering for the inner
                        // op's full duration. Order matters: destroy the runner FIRST
                        // (so inner_task's continuation_ — which points at the runner
                        // frame — can never be used), THEN drop the inner task frame.
                        if (s->runner_handle)
                            coro_scheduler().cancel_spawned(std::exchange(s->runner_handle, {}));
                        // Scrub the inner frame from the scheduler queues before its
                        // task<T> owner frees it: if the inner op's own watcher fired in
                        // this same tick it is already queued, and destroying it via the
                        // dtor would leave a dangling handle for run_ready() to pop (UAF).
                        if (auto ih = s->inner_task.handle())
                            coro_scheduler().forget(ih);
                        s->inner_task = task<T>{};
                        if (s->continuation)
                            schedule_via_current(s->continuation);
                    }
                });
            }
        }

        T
        await_resume() {
            // Cancellation takes priority: if the caller asked for throwing
            // cancellation and a cancel signal raced with completion, we
            // honor the cancellation contract.
            if (token.is_cancelled() && throw_on_cancel) {
                throw cancelled_error();
            }
            // Finding 2.B.2: propagate inner-task exceptions. The previous
            // version silently discarded them and returned `T{}` — data
            // corruption for every non-void return type.
            if (state->error) {
                std::rethrow_exception(state->error);
            }
            if (state->result)
                return std::move(*state->result);
            // No result, no error, not cancelled → the inner task completed
            // by returning a value that was not transferred. This should
            // never happen in practice; fail loudly rather than silently.
            throw std::logic_error("cancellable_operation: inner task completed without "
                                   "delivering a value or an exception");
        }

    private:
        static task<void>
        task_runner(std::shared_ptr<shared_state> state) {
            try {
                state->result = co_await state->inner_task;
            } catch (...) {
                state->error = std::current_exception();
            }
            if (!state->task_done) {
                state->task_done = true;
                if (state->continuation)
                    schedule_via_current(state->continuation);
            }
        }
    };

    awaiter
    operator co_await() {
        return awaiter{_shared, _token, _throw_on_cancel};
    }
};

// Specialization for void
template <>
class cancellable_operation<void> {
    struct shared_state {
        task<void>              inner_task;
        std::exception_ptr      error;            ///< inner task exception, propagated on resume
        bool                    task_done{false}; // single-thread
        std::coroutine_handle<> continuation;
        // See the non-void specialization: the detached task_runner driving
        // inner_task, torn down (with inner_task) on early cancel.
        std::coroutine_handle<> runner_handle{};

        explicit shared_state(task<void> &&t)
            : inner_task(std::move(t)) {}
    };

    std::shared_ptr<shared_state> _shared;
    cancellation_token            _token;
    bool                          _throw_on_cancel;

public:
    cancellable_operation(task<void> &&t, cancellation_token token, bool throw_on_cancel = true)
        : _shared(std::make_shared<shared_state>(std::move(t)))
        , _token(std::move(token))
        , _throw_on_cancel(throw_on_cancel) {}

    struct awaiter {
        std::shared_ptr<shared_state> state;
        cancellation_token            token;
        bool                          throw_on_cancel;
        cancellation_token::id_type   _cancel_id = 0;

        awaiter(std::shared_ptr<shared_state> s, cancellation_token t, bool toc)
            : state(std::move(s))
            , token(std::move(t))
            , throw_on_cancel(toc) {}

        ~awaiter() {
            token.remove_on_cancel(_cancel_id);
        }

        [[nodiscard]] bool
        await_ready() const {
            return token.is_cancelled() && throw_on_cancel;
        }

        void
        await_suspend(std::coroutine_handle<> h) {
            state->continuation  = h;
            state->runner_handle = coro_scheduler().spawn_tracked(task_runner(state));
            if (throw_on_cancel) {
                // See the non-void specialization: guard the cancel wake-up with
                // the shared task_done flag to avoid double-resume / use-after-free,
                // and tear down the detached runner + inner task on cancel so neither
                // frame lingers past cancellation.
                auto s     = state;
                _cancel_id = token.on_cancel([s]() {
                    if (!s->task_done) {
                        s->task_done = true;
                        if (s->runner_handle)
                            coro_scheduler().cancel_spawned(std::exchange(s->runner_handle, {}));
                        // Scrub the inner frame from the scheduler queues before its
                        // task<void> owner frees it (same-tick watcher-fired UAF guard;
                        // see the non-void specialization).
                        if (auto ih = s->inner_task.handle())
                            coro_scheduler().forget(ih);
                        s->inner_task = task<void>{};
                        if (s->continuation)
                            schedule_via_current(s->continuation);
                    }
                });
            }
        }

        void
        await_resume() {
            if (token.is_cancelled() && throw_on_cancel)
                throw cancelled_error();
            // Finding 2.B.2: propagate inner-task exceptions (void overload).
            if (state->error) {
                std::rethrow_exception(state->error);
            }
        }

    private:
        static task<void>
        task_runner(std::shared_ptr<shared_state> state) {
            try {
                co_await state->inner_task;
            } catch (...) {
                state->error = std::current_exception();
            }
            if (!state->task_done) {
                state->task_done = true;
                if (state->continuation)
                    schedule_via_current(state->continuation);
            }
        }
    };

    awaiter
    operator co_await() {
        return awaiter{_shared, _token, _throw_on_cancel};
    }
};

/**
 * @brief Wrap a task with cancellation support
 * @param task Task to wrap
 * @param token Cancellation token
 * @param throw_on_cancel Whether to throw on cancellation
 * @return Cancellable operation wrapper
 * @ingroup Coroutine
 *
 * @warning The wrapped @p task must NOT cancel its OWN controlling @p token synchronously from
 *          within its own body (directly, or via a linked/child token, or by killing the actor
 *          whose scope owns this token). Cancellation is delivered as an `on_cancel` hook that
 *          eagerly tears the inner frame down; if that hook fires while the inner task is the
 *          frame currently executing on the stack, it would destroy a running coroutine.
 *          Cancellation is meant to be driven from OUTSIDE the operation (a coordinator, an actor
 *          lifecycle kill, a sibling branch) — which is how every framework path uses it. A proper
 *          fix for self-cancellation needs scheduler running-frame tracking; see [[qb-io-async-callback-noexcept-footguns]].
 */
template <typename T>
auto
make_cancellable(task<T> &&task, cancellation_token token, bool throw_on_cancel = true) {
    return cancellable_operation<T>(std::move(task), std::move(token), throw_on_cancel);
}

/**
 * @brief Awaiter for sleep with immediate wake on cancellation
 *
 * Registers the coroutine handle with the token so that cancel() schedules
 * it immediately instead of polling every 10ms.
 */
/**
 * @brief Awaiter for sleep with immediate wake on cancellation.
 *
 * Uses a shared_ptr<sleep_state> and a static free function (not a lambda) for the
 * timer coroutine, so the coroutine frame captures its arguments by value as function
 * parameters — not via a lambda closure that could dangle when await_suspend returns.
 */
struct cancellable_sleep_awaiter {
    // Plain bool: single-thread cooperative — the on_cancel callback and the
    // timer_task coroutine run on the same thread and never concurrently.
    struct sleep_state {
        bool                    resumed{false};
        std::coroutine_handle<> handle;
        // The detached timer_task driving the sleep. On early cancel we tear it
        // down through this handle so its frame + ev_timer are reclaimed now
        // instead of lingering parked on the full original duration.
        std::coroutine_handle<> timer_handle{};
    };

    qb::duration                duration;
    cancellation_token          token;
    cancellation_token::id_type _cancel_id = 0;

    // User-declared dtor below → non-aggregate; provide the ctor cancellable_sleep() uses.
    cancellable_sleep_awaiter(qb::duration d, cancellation_token t)
        : duration(d)
        , token(std::move(t)) {}

    ~cancellable_sleep_awaiter() {
        // Without this, a `co_await ctx.sleep(...)` loop on the long-lived actor scope token
        // accumulated one dead callback per iteration (the original unbounded-growth bug).
        token.remove_on_cancel(_cancel_id);
    }

    [[nodiscard]] bool
    await_ready() const {
        return token.is_cancelled();
    }

    void
    await_suspend(std::coroutine_handle<> h) {
        auto state    = std::make_shared<sleep_state>();
        state->handle = h;
        _cancel_id    = token.on_cancel([state]() {
            if (!state->resumed) {
                state->resumed = true;
                // Reclaim the detached timer_task NOW rather than letting it stay
                // parked on the (e.g. multi-second) original sleep — otherwise its
                // frame leaks until the timer fires, or for the rest of the thread's
                // life on a core whose listener is never torn down (Main::start(false)).
                if (state->timer_handle)
                    coro_scheduler().cancel_spawned(std::exchange(state->timer_handle, {}));
                schedule_via_current(state->handle);
            }
        });
        // Keep our own shared_ptr ref (don't move) so we can record the spawned
        // helper's handle on the shared state for the cancel path above.
        state->timer_handle = coro_scheduler().spawn_tracked(timer_task(duration, state));
    }

    void
    await_resume() {
        token.throw_if_cancelled();
    }

private:
    // Static function: d and s are VALUE parameters stored in the coroutine frame.
    // Never use a lambda here — the compiler may store a pointer to the local lambda
    // object rather than a copy, causing a dangling reference after await_suspend returns.
    static task<void>
    timer_task(qb::duration d, std::shared_ptr<sleep_state> s) {
        co_await sleep(d);
        if (!s->resumed) {
            s->resumed = true;
            schedule_via_current(s->handle);
        }
    }
};

/**
 * @brief Sleep that can be cancelled; wakes immediately when token is cancelled
 * @param duration Sleep duration
 * @param token Cancellation token
 * @return Task that completes after duration or when cancelled
 * @ingroup Coroutine
 */
inline task<void>
cancellable_sleep(qb::duration duration, cancellation_token token) {
    co_await cancellable_sleep_awaiter{duration, std::move(token)};
}

namespace detail {
struct with_deadline_timeout_state {
    bool                    completed{false}; // single-thread
    int                     result{0};
    std::coroutine_handle<> handle;
    // The detached deadline_timer_task driving the timeout sleep. Torn down the
    // instant this branch is resolved by another path — a cancel, or (the common
    // case) the operation winning the `when_any` race, which destroys this awaiter's
    // frame and runs the dtor below. Without this the timer stays parked on the full
    // remaining-until-deadline sleep, leaking its frame + ev_timer (and on a core whose
    // listener is never torn down, for the rest of the thread's life).
    std::coroutine_handle<> timer_handle{};
};

struct with_deadline_timeout_awaiter {
    std::shared_ptr<with_deadline_timeout_state> state;
    std::chrono::steady_clock::time_point        deadline;
    cancellation_token                           token;
    cancellation_token::id_type                  _cancel_id = 0;

    with_deadline_timeout_awaiter(std::shared_ptr<with_deadline_timeout_state> s, std::chrono::steady_clock::time_point dl,
                                  cancellation_token t)
        : state(std::move(s))
        , deadline(dl)
        , token(std::move(t)) {}

    ~with_deadline_timeout_awaiter() {
        token.remove_on_cancel(_cancel_id);
        // Reclaim the detached deadline timer if it is still parked. This fires when the
        // operation branch wins the when_any race: `when_any` destroys this (losing) branch
        // frame, which runs this dtor while the timer is still sleeping. cancel_spawned is a
        // no-op once the timer has fired (its frame is no longer owned and timer_handle was
        // cleared in deadline_timer_task), so the normal timeout/cancel paths are unaffected.
        if (state && state->timer_handle)
            coro_scheduler().cancel_spawned(std::exchange(state->timer_handle, {}));
    }

    [[nodiscard]] bool
    await_ready() const {
        if (token.is_cancelled()) {
            state->result    = 1;
            state->completed = true;
            return true;
        }
        return false;
    }

    void
    await_suspend(std::coroutine_handle<> h) {
        state->handle = h;
        _cancel_id    = token.on_cancel([s = state]() {
            if (!s->completed) {
                s->completed = true;
                s->result    = 1;
                // Cancel resolved the wait: tear the detached timer down now rather than
                // leaving it parked on the full remaining-until-deadline sleep.
                if (s->timer_handle)
                    coro_scheduler().cancel_spawned(std::exchange(s->timer_handle, {}));
                schedule_via_current(s->handle);
            }
        });

        auto now       = std::chrono::steady_clock::now();
        auto remaining = std::chrono::duration_cast<qb::duration>(deadline - now);
        if (remaining.count() <= 0) {
            if (!state->completed) {
                state->completed = true;
                state->result    = 0;
                schedule_via_current(h);
            }
            return;
        }

        // spawn_tracked (not spawn) so the dtor / cancel hook can reclaim the detached
        // timer the instant another path resolves this branch.
        state->timer_handle = coro_scheduler().spawn_tracked(deadline_timer_task(state, remaining));
    }

    int
    await_resume() const {
        return state->result;
    }

private:
    static task<void>
    deadline_timer_task(std::shared_ptr<with_deadline_timeout_state> s, qb::duration remaining) {
        co_await sleep(remaining);
        // The timer has fired: its frame self-reclaims at final_suspend, so clear the
        // tracked handle. A stale handle here would let a later dtor/cancel cancel_spawned()
        // an unrelated frame that reused the address.
        s->timer_handle = {};
        if (!s->completed) {
            s->completed = true;
            s->result    = 0;
            schedule_via_current(s->handle);
        }
    }
};

// Free function: s, dl, tok are VALUE parameters in the coroutine frame.
// Must NOT be a lambda defined inside with_deadline — the lambda would be a
// local variable in with_deadline's frame and the compiler may store only a
// pointer to it in the spawned coroutine frame. If with_deadline's frame is
// destroyed first (e.g. after throwing timeout_error), that pointer dangles.
inline task<int>
with_deadline_run_timeout(std::shared_ptr<with_deadline_timeout_state> s, std::chrono::steady_clock::time_point dl, cancellation_token tok) {
    co_return co_await with_deadline_timeout_awaiter{std::move(s), dl, std::move(tok)};
}

} // namespace detail

/**
 * @brief Helper to run operation with deadline
 *
 * Single sleep until deadline (no 10ms polling). Cancel wakes immediately via token callback.
 *
 * @param operation Task to run
 * @param deadline Max time point
 * @param token Cancellation token (optional)
 * @return Result or throws timeout_error / cancelled_error
 * @ingroup Coroutine
 */
template <typename T>
task<T>
with_deadline(task<T> &&operation, std::chrono::steady_clock::time_point deadline, cancellation_token token = {}) {
    // Early short-circuit: if the deadline is already in the past when we
    // enter, the contract is already violated — no point running the
    // operation. Throwing synchronously here is safe and matches user
    // expectations ("must complete **before** deadline"). Finding 2.B.5
    // is strictly about not reclassifying a *winning* operation after the
    // race has already been resolved; this pre-check runs *before* the
    // race and has a different intent.
    if (std::chrono::steady_clock::now() >= deadline) {
        throw timeout_error();
    }

    auto state = std::make_shared<detail::with_deadline_timeout_state>();

    // Use a free function (not a lambda) to avoid the dangling-lambda-pointer bug:
    // if we used a local lambda here, with_deadline's frame might be destroyed
    // (after throwing timeout_error) while the spawned timeout coroutine still
    // holds a pointer to that local lambda object.
    auto res = co_await when_any(std::move(operation), detail::with_deadline_run_timeout(state, deadline, token));

    // Finding 2.B.5: the operation branch **won** the race — that is
    // authoritative. Do not second-guess it against wall-clock time here:
    // the old code could reclassify a completed result as a timeout under
    // load or at the millisecond boundary, silently discarding a valid
    // value. `when_any` already guarantees that the loser is irrelevant.
    if (res.index == 0) {
        co_return res.template get<T>();
    }
    if (res.template get<int>() == 1) {
        throw cancelled_error();
    }
    throw timeout_error();
}

} // namespace qb::io::async

#endif // QB_IO_ASYNC_COROUTINE_CANCELLATION_H
