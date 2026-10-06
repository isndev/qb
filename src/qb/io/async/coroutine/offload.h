/**
 * @file qb/io/async/coroutine/offload.h
 * @brief `co_await offload(fn, args...)`: run a blocking or CPU-bound callable on a pool thread
 *        and resume on the loop that awaited
 *
 * qb-io is single-threaded per loop, and a coroutine that calls something slow -- a file read on a
 * slow disk, a DNS lookup, a deliberately slow KDF, a compression of megabytes -- stalls every other
 * coroutine, watcher and actor of its thread for as long as it runs. `offload` moves that call to a
 * small process-wide pool and suspends the coroutine until it has returned:
 *
 * @code
 * qb::io::async::task<long long> checksum(std::vector<char> bytes) {
 *     co_return co_await qb::io::async::offload(   // the loop runs on while a pool thread sums
 *         [](std::vector<char> const &b) {
 *             long long s = 0;
 *             for (char const c : b)
 *                 s += c;
 *             return s;
 *         },
 *         std::move(bytes));                        // moved into the call: values only
 * }                                                // resumed on this loop with the sum
 * @endcode
 *
 * THE CONTRACT, IN FOUR RULES
 * ===========================
 * 1. **Values in, values out.** `fn` and `args` are copied (decayed) at the call, like
 *    `std::thread`'s, invoked on a pool thread as `std::invoke(std::move(fn), std::move(args)...)`
 *    and destroyed THERE, right after the call. The result is a value -- a reference result does not
 *    compile -- constructed on the pool thread and handed to the coroutine by `co_await`.
 * 2. **The callable runs on ANOTHER thread.** It must not touch an actor, a qb-io object, a
 *    coroutine, or anything the awaiting loop owns. `listener::current` inside it is the POOL
 *    thread's own listener, not the awaiting loop's.
 * 3. **The coroutine resumes on the thread that awaited**, through that thread's scheduler, in a
 *    turn of its loop -- never on a pool thread. An exception the callable throws is rethrown by
 *    `co_await`.
 * 4. **A frame destroyed while its call runs is never resumed.** A losing `when_any` branch, a
 *    cancelled scope, an actor's `ctx.offload` woken by its kill: the call still runs to its end (a
 *    running callable is never interrupted), its result is destroyed on the awaiting loop, and
 *    `offload_stats::discarded` counts it. A kill does not destroy a bare `offload`'s frame: it
 *    waits on, as a bare `sleep` does -- inside an actor, `ScopedCoroContext::offload` is the form.
 *
 * THE POOL
 * ========
 * One per process, started by the first `offload` -- no thread exists before -- with two threads
 * unless `set_offload_threads()` said otherwise first. Jobs are served FIFO from an unbounded queue.
 * At process exit the pool is stopped: a job not started yet is dropped, a running one finishes,
 * so an offload still running then delays the exit by what it has left.
 *
 * HOW THE RESUME CROSSES THREADS
 * ==============================
 * Each thread that awaits an offload gets a completion port (qb/io/async/offload.cpp): its own
 * `ev_async` watcher on that thread's loop, started while an offload of the thread is in flight
 * and stopped once none is. A pool thread that finishes a job queues it on the port and sends the
 * watcher -- under the port's mutex, so it never sends to a loop that is being destroyed -- and the
 * watcher's callback, on the loop, schedules the waiting coroutine. While started, the watcher is
 * referenced: the loop counts as busy (`listener::has_work()`), so a qb-core `VirtualCore` keeps
 * pumping it and parks INSIDE it, where the send ends the park; `async::run()` returns only once
 * the offload completed, as it would for a pending timer. Nothing of this exists for a thread that
 * never calls `offload`: no watcher, no pipe, no branch on any existing path.
 *
 * @see qb/readme/3_qb_io/coroutines.md, "Offloading blocking work"
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

#ifndef QB_IO_ASYNC_COROUTINE_OFFLOAD_H
#define QB_IO_ASYNC_COROUTINE_OFFLOAD_H

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>

#include "awaiter.h"
#include "task.h"

namespace qb::io::async {

/**
 * @brief A snapshot of the offload pool's counters, process-wide.
 * @details Read with `current_offload_stats()`. Each field is a cumulative count since the process
 *          started, except `threads` and `queued`, which are the state at the reading. The counters
 *          are relaxed atomics read one after the other: the snapshot is not a single instant, and a
 *          job may be counted `submitted` and not yet `queued` or `completed`.
 * @ingroup Coroutine
 */
struct offload_stats {
    std::uint64_t threads   = 0; ///< pool threads running: 0 until the first `offload`
    std::uint64_t submitted = 0; ///< jobs handed to the pool
    std::uint64_t completed = 0; ///< jobs whose callable has returned or thrown
    std::uint64_t discarded = 0; ///< completed jobs whose awaiting frame was gone: result destroyed, nothing resumed
    std::uint64_t queued    = 0; ///< jobs waiting for a pool thread at the reading
};

/**
 * @brief Set the number of pool threads, before the pool starts.
 * @param n Threads to start at the first `offload`; at least 1.
 * @return `true` if taken; `false` once the pool has started (the first `offload` started it), or
 *         for `n == 0`. The default is 2.
 * @details The pool is process-wide: call this once, early -- from `main`, before an engine or a
 *          loop that offloads starts. Thread-safe.
 * @ingroup Coroutine
 */
bool set_offload_threads(std::size_t n) noexcept;

/**
 * @brief Read the offload pool's counters. Thread-safe; starts no thread.
 * @ingroup Coroutine
 */
[[nodiscard]] offload_stats current_offload_stats() noexcept;

namespace detail {

class offload_port;

/**
 * @brief The state one offload shares between its awaiter, on the loop that awaits, and the pool.
 * @details One allocation per offload: the callable, its arguments, the result and the exception
 *          live in the derived `offload_task`. Two references from submission on -- the awaiter's
 *          and the job's own, which travels pool -> port -> loop and is released by the loop's
 *          drain (or by a pool thread when the awaiting thread has exited). `awaiter` is read and
 *          written on the awaiting thread only; `error` and the result are written by the pool
 *          thread before the port's mutex publishes the job to that thread.
 */
struct offload_job {
    offload_job()                               = default;
    offload_job(const offload_job &)            = delete;
    offload_job &operator=(const offload_job &) = delete;
    virtual ~offload_job()                      = default;

    /// Pool thread: invoke the callable, keep its result or its exception, destroy the callable.
    virtual void run() noexcept = 0;

    void
    release() noexcept {
        if (refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
            delete this;
    }

    std::atomic<std::uint32_t>    refs{1};
    awaiter_base                 *awaiter = nullptr; ///< the frame to resume; null once abandoned
    offload_job                  *next    = nullptr; ///< the port's completion list (under its mutex)
    std::shared_ptr<offload_port> port;              ///< set at submission
    std::exception_ptr            error;
};

/// Submit `job` from the awaiting thread: takes the job's second reference, registers it on this
/// thread's port and queues it on the pool (starting the pool on the first call). Throws what the
/// pool start or the queue throws (`std::system_error`, `std::bad_alloc`), with nothing submitted.
void offload_submit(offload_job *job);

template <typename R>
struct offload_result : offload_job {
    using result_type = R;
    std::optional<R> value;

    R
    take() {
        if (error)
            std::rethrow_exception(error);
        return std::move(*value);
    }
};

template <>
struct offload_result<void> : offload_job {
    using result_type = void;
    void
    take() {
        if (error)
            std::rethrow_exception(error);
    }
};

template <typename R, typename Fn, typename... Args>
struct offload_task final : offload_result<R> {
    template <typename F, typename... A>
    explicit offload_task(F &&f, A &&...a)
        : call(std::in_place, std::forward<F>(f), std::forward<A>(a)...) {}

    void
    run() noexcept override {
        try {
            if constexpr (std::is_void_v<R>)
                std::apply([](Fn &fn, Args &...args) { std::invoke(std::move(fn), std::move(args)...); }, *call);
            else
                this->value.emplace(
                    std::apply([](Fn &fn, Args &...args) -> R { return std::invoke(std::move(fn), std::move(args)...); }, *call));
        } catch (...) {
            this->error = std::current_exception();
        }
        call.reset(); // the callable and its arguments die on the pool thread, right after the call
    }

    std::optional<std::tuple<Fn, Args...>> call;
};

/// The job of `offload(fn, args...)`, built once for both front ends (`offload()` and an actor's
/// `ScopedCoroContext::offload`): `fn` and `args` decayed and copied, the result type checked.
template <typename F, typename... Args>
[[nodiscard]] auto
make_offload_job(F &&fn, Args &&...args) {
    using Fn = std::decay_t<F>;
    using R  = std::invoke_result_t<Fn, std::decay_t<Args>...>;
    static_assert(!std::is_reference_v<R>,
                  "offload: the callable must return a value or void -- a reference would point into the pool thread's "
                  "world; return by value");
    static_assert(std::is_void_v<R> || std::is_move_constructible_v<R>, "offload: the result type must be move-constructible");
    return std::unique_ptr<offload_result<R>>{new offload_task<R, Fn, std::decay_t<Args>...>(std::forward<F>(fn), std::forward<Args>(args)...)};
}

} // namespace detail

/**
 * @brief The awaitable `offload()` returns. Await it once; it cannot be copied or moved.
 * @tparam R The callable's result type (a value, or `void`).
 * @ingroup Coroutine
 */
template <typename R>
class offload_awaiter final : public awaiter_base {
public:
    explicit offload_awaiter(detail::offload_result<R> *job) noexcept
        : job_(job) {}

    void
    await_suspend(std::coroutine_handle<> h) override {
        handle_    = h;
        scheduler_ = CoroutineScheduler::current_ptr();
        if (!scheduler_)
            scheduler_ = &CoroutineScheduler::current();
        register_suspended();
        job_->awaiter = this;
        try {
            detail::offload_submit(job_);
        } catch (...) {
            // Nothing was submitted: the frame resumes at once with the exception (a throwing
            // `await_suspend` resumes the coroutine), so it must not stay in the suspended set.
            job_->awaiter = nullptr;
            unregister_suspended();
            throw;
        }
    }

    R
    await_resume() {
        unregister_suspended();
        awaiter_base::await_resume();
        return job_->take();
    }

    ~offload_awaiter() override {
        unschedule(); // full scrub: a drained-but-not-yet-resumed frame must leave the ready queue too
        if (job_) {
            job_->awaiter = nullptr; // abandoned, if the call is still running: the drain discards it
            job_->release();
        }
    }

private:
    detail::offload_result<R> *job_;
};

/**
 * @brief Run `fn(args...)` on the offload pool and suspend until it has returned.
 * @param fn   A callable; copied (decayed) here and destroyed on the pool thread after the call.
 * @param args Its arguments; copied (decayed) here, moved into the call.
 * @return An awaitable whose `co_await` yields the callable's result -- or rethrows its exception --
 *         on the thread that awaited.
 * @details The four rules -- values only; the callable runs on another thread and touches nothing
 *          of the loop; the resume happens on the awaiting thread; a destroyed frame is never
 *          resumed -- and how the resume crosses threads are in this header's file comment.
 *          Nothing runs until the awaitable is awaited, and an awaitable never awaited submits
 *          nothing. The first `offload` of the process starts the pool; a pool that cannot start
 *          makes the `co_await` throw `std::system_error`, and the next one retries.
 * @ingroup Coroutine
 */
template <typename F, typename... Args>
[[nodiscard]] auto
offload(F &&fn, Args &&...args) {
    auto job = detail::make_offload_job(std::forward<F>(fn), std::forward<Args>(args)...);
    using R  = typename decltype(job)::element_type::result_type;
    return offload_awaiter<R>{job.release()};
}

namespace detail {

/**
 * @brief A task that awaits the offload of `job`: the form a cancellation wrapper can own.
 * @details What qb-core's `ScopedCoroContext::offload` hands to `cancellable()`, so a killed actor's
 *          wait unwinds at once: the cancellation destroys this frame, the awaiter's destructor marks
 *          the job abandoned, and its result is discarded on the loop when the call returns. The
 *          job travels as a pointer, never the callable by value: a coroutine parameter of an
 *          over-aligned type is the one LLVM before 22 spills at the wrong alignment (Huly QB-213).
 */
template <typename R>
task<R>
offload_as_task(std::unique_ptr<offload_result<R>> job) {
    co_return co_await offload_awaiter<R>{job.release()};
}

} // namespace detail

} // namespace qb::io::async

#endif // QB_IO_ASYNC_COROUTINE_OFFLOAD_H
