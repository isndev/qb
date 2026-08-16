/**
 * @file qb/io/async/coroutine/task.h
 * @brief C++20 coroutine task type for QB async I/O
 *
 * This file defines the task<T> class template, the primary coroutine type
 * for the QB framework. It integrates with libev for suspend/resume operations
 * and provides zero-cost abstraction in optimized builds.
 *
 * CRITICAL IMPLEMENTATION NOTES:
 * ==============================
 *
 * 1. VARIANT INITIALIZATION (CRITICAL BUG PREVENTION):
 *    The promise_type's result_ variant MUST be explicitly initialized to
 *    std::monostate (index 0). Without explicit initialization, the variant
 *    may contain a default-constructed T instead, causing await_ready() to
 *    return true prematurely and await_resume() to return uninitialized values.
 *
 *    REQUIRED: promise_type() : result_(std::in_place_index<0>) {}
 *
 * 2. LAMBDA COROUTINE CAPTURES (DANGLING REFERENCE PREVENTION):
 *    When creating coroutines from lambdas, temporary lambda objects create
 *    dangling references after the first suspension point.
 *
 *    UNSAFE - Temporary lambda:
 *    @code
 *    auto t = [&data]() -> task<void> {
 *        co_await sleep(100ms);
 *        use(data);  // DANGLING! Lambda destroyed before resume
 *    }();
 *    @endcode
 *
 *    SAFE - Store lambda in variable:
 *    @code
 *    auto coro_fn = [&data]() -> task<void> {
 *        co_await sleep(100ms);
 *        use(data);
 *    };
 *    auto t = coro_fn();  // Lambda stays alive
 *    @endcode
 *
 *    BEST - Use regular functions or capture by pointer:
 *    @code
 *    task<void> process_data(Data* data) {
 *        co_await sleep(100ms);
 *        use(*data);
 *    }
 *    @endcode
 *
 * 3. SYMMETRIC TRANSFER:
 *    This implementation uses symmetric transfer (returning coroutine_handle
 *    from await_suspend and final_suspend) to prevent stack overflow in deep
 *    coroutine chains. The compiler handles the transfer without recursion.
 *
 * 4. MOVE SEMANTICS:
 *    task<T> is move-only. The handle is transferred on move, and the moved-from
 *    task becomes empty. Always use std::move when passing tasks to spawn() or
 *    other functions.
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

#ifndef QB_IO_ASYNC_COROUTINE_TASK_H
#define QB_IO_ASYNC_COROUTINE_TASK_H

#include <array>
#include <atomic>
#include <coroutine>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>
#include <qb/utility/prefix.h> // QB_LOCKFREE_CACHELINE_BYTES (canonical cache-line size)

// Debug trace macro (defined early for use in promise_type)
#ifdef QB_DEBUG_COROUTINES
#include <iostream>
#define QB_CORO_TRACE(id, stage) std::cerr << "[CORO] " << id << ": " << stage << "\n"
#else
#define QB_CORO_TRACE(id, stage) ((void) 0)
#endif

namespace qb::io::async {

// Forward declaration
class CoroutineScheduler;

// Defined in scheduler.h. Called from task<T>::final_suspend to hand a completed
// DETACHED (spawned) coroutine frame to the current scheduler for destruction:
// final_suspend cannot destroy its own frame (it is suspended in it), and a
// spawned frame has no continuation/owner to free it otherwise.
// `inline` is LOAD-BEARING, not decoration: scheduler.h defines this `inline`, so the only
// definition in the program is the one that TU emits. A non-inline first declaration makes every
// TU that sees task.h WITHOUT scheduler.h (i.e. entering through <qb/io.h>, or through this
// header directly) emit an undefined reference no archive can satisfy — and it silences the
// -Wundefined-inline that would otherwise say so. Do not drop it. See check-installed-headers.sh.
inline void defer_frame_destruction(std::coroutine_handle<>) noexcept;

// Defined in scheduler.h. Scrubs a coroutine handle from the current scheduler's ready /
// in-flight / suspended bookkeeping. Called from task<T>::~task (and move-assignment) just
// before destroying a still-in-flight frame: a waker may have already queued it via
// schedule_via_current() (e.g. a when_any/race loser subtree reclaimed in the same drain, or
// any wait-list awaiter — mutex/semaphore/channel/scope/...), so without this the freed frame
// would dangle in the ready queue for run_ready() to resume → heap-use-after-free. Mirrors the
// libev awaiter_base::unschedule() teardown for the parked-handle (wait-list) awaiters, applied
// centrally at every depth. CoroutineScheduler is incomplete here, hence the free-function hop.
// `inline` is LOAD-BEARING — see defer_frame_destruction above. This one is called from
// task<T>::~task, i.e. from destroying ANY task, so without it <qb/io.h> alone cannot link a
// program that owns a task<T>.
inline void forget_frame_if_current(std::coroutine_handle<>) noexcept;

// Defined OUT OF LINE in qb/io/logger.cpp, next to the `qb::io::cerr` it reports through — the
// same placement, and for the same reason, as `qb::detail::report_unhandled_coroutine_exception`
// in Actor.cpp: one policy, one place, and no I/O machinery pulled into this header. Deliberately
// NOT `inline` (contrast the two declarations above, whose definitions live in scheduler.h): this
// one has a real definition in the archive every qb-io consumer already links for `qb::io::cerr`.
//
// Called from task<T>::final_suspend on the DETACHED path only, and only when the promise
// actually holds one — the awaiter carries a promise POINTER rather than a copy of the
// exception_ptr, so the no-exception path (every ordinary completion) costs a null check.
// Called from task<T>::final_suspend on the DETACHED path only — no continuation and no `task<T>`
// owner — which is the exact state in which a stored exception can never be observed. Nothing
// awaits the frame, so `await_resume()` (the only place that rethrows) never runs and the
// exception dies with the promise. `Actor::spawn` / `spawn_detached` already report through
// `qb::detail::report_unhandled_coroutine_exception` because their wrapper coroutines CATCH
// (VirtualCore.h:1139-1146, 1160-1168) and therefore leave the wrapper promise clean — so they do
// not reach here and cannot double-report. The free-function path,
// `qb::io::async::coro_scheduler().spawn(t)`, has no such wrapper, and is what this covers.
void report_detached_coroutine_exception(std::exception_ptr ep) noexcept;

// ============================================================================
// Coroutine frame freelist allocator (Finding 2.A.9)
// ============================================================================
//
// Coroutine frames are churned at very high rate on the hot path
// (spawn → await → complete → free). Because the framework is strictly
// mono-thread per worker, a *thread-local* size-bucketed freelist can serve
// the vast majority of new/delete pairs without any lock or atomic, and
// without going through the global allocator.
//
// Design:
//   * Bucket size = ceil(frame_size / kAlign). kAlign = QB_LOCKFREE_CACHELINE_BYTES
//     (64 B on all current targets) so a pooled block is always cache-line aligned —
//     a coroutine frame may embed a cache-line-aligned qb::Event by value, and the
//     optimiser emits aligned AVX moves for that copy under -march=native.
//   * kMaxBucket buckets → frames up to kAlign * kMaxBucket (= 4 KiB with a 64 B
//     line) are pooled; larger frames fall through to ::operator new (these are rare
//     — they'd correspond to coroutines that hold a large by-value state, which is
//     discouraged anyway).
//   * Freed blocks are intrusively linked via their first 8 B, LIFO.
//   * Thread-exit draining: the free-list lives in a thread_local RAII holder
//     (BucketPool) whose destructor walks every bucket and ::operator delete's
//     each parked block. Without it, a spawned VirtualCore worker thread would
//     leak its entire frame pool on exit — the thread_local head pointers are
//     destroyed at thread teardown, orphaning the heap blocks they referenced
//     (visible as ROOT LEAKs under `leaks --atExit` from a worker thread; on the
//     main thread the pool is reachable until process exit and was never flagged).
//     At thread exit `live_frames` is 0 (all frames already deallocated back into
//     the pool), so the holder only ever frees idle, balanced pool memory.
//
// The allocator is used by `task<T>::promise_type::operator new/delete`.
// Both sized and unsized delete are provided as CLASS members, which is always legal.
// The GLOBAL sized overloads are NOT: libstdc++ declares ::operator delete(void*, size_t
// [, align_val_t]) only under __cpp_sized_deallocation, and clang leaves that off (<= 18).
namespace detail {

class CoroutineFrameAllocator {
public:
    // Cache line: a coroutine frame may hold a by-value qb::Event subtype, which
    // is QB_LOCKFREE_CACHELINE_ALIGNMENT. The frame inherits that alignment and the
    // optimiser emits aligned AVX moves for the by-value copy under -march=native;
    // plain ::operator new only guarantees 16 B on x86-64, so the pool must return
    // cache-line-aligned blocks or it SIGSEGVs. Use the canonical cache-line constant
    // (overridable via KNOWN_L1_CACHE_LINE_SIZE) rather than a magic 64 so the pool
    // always tracks the platform's true line size / the alignment Events are built with.
    static constexpr std::size_t kAlign     = QB_LOCKFREE_CACHELINE_BYTES;
    static constexpr std::size_t kMaxBucket = 64; // frames up to kAlign * kMaxBucket are pooled
    static_assert(kAlign >= alignof(std::max_align_t), "coroutine frame pool alignment must cover the platform's max scalar alignment");

    // Per-thread count of coroutine frames allocated through this pool and not
    // yet freed. Cheap diagnostic (thread_local — coroutines are mono-thread per
    // VirtualCore, so no cross-core contention). Tests assert it returns to its
    // baseline after coroutines complete, guarding against frame leaks.
    QB_ABI_ANCHOR static inline thread_local long live_frames = 0;

    [[nodiscard]] static void *
    allocate(std::size_t size) {
        ++live_frames;
        const std::size_t idx = bucket_index(size);
        if (idx == 0 || idx > kMaxBucket) {
            return ::operator new(size, std::align_val_t{kAlign});
        }
        // buckets() lazily constructs the thread_local BucketPool on first use,
        // which sets pool_alive(). allocate() is only ever called from a running
        // coroutine (never from a destructor during thread teardown), so the pool
        // is always in its alive window here.
        auto &head = buckets()[idx - 1];
        if (head) {
            void *p = head;
            head    = *static_cast<void **>(p);
            return p;
        }
        return ::operator new(idx * kAlign, std::align_val_t{kAlign});
    }

    static void
    deallocate(void *p, std::size_t size) noexcept {
        if (!p)
            return;
        --live_frames;
        const std::size_t idx = bucket_index(size);
        // Oversized frames, and the rare frame freed after this thread's pool was
        // already torn down (thread_local destruction order), go straight back to
        // the global allocator. Pushing onto a destroyed free-list would corrupt
        // freed memory / re-leak the block. allocate() mirrors this for `idx`, so
        // the size matches the aligned ::operator new used on the fallback path.
        if (idx == 0 || idx > kMaxBucket || !pool_alive()) {
            ::operator delete(p, std::align_val_t{kAlign});
            return;
        }
        auto &head               = buckets()[idx - 1];
        *static_cast<void **>(p) = head;
        head                     = p;
    }

private:
    static std::size_t
    bucket_index(std::size_t size) noexcept {
        // Guarantee that each slot can store the intrusive `void*` that we
        // overlay when free.
        if (size < sizeof(void *))
            size = sizeof(void *);
        return (size + kAlign - 1) / kAlign;
    }

    // kMaxBucket slots — each slot is a LIFO head pointer for its size class.
    using BucketArray = std::array<void *, kMaxBucket>;

    // True while this thread's BucketPool is constructed and not yet destroyed.
    // allocate()/deallocate() consult it so a frame freed during thread_local
    // teardown (after ~BucketPool already ran) bypasses the dead free-list and
    // is returned straight to the global allocator instead of corrupting it.
    //
    // MUST stay a SEPARATE function-local thread_local (not a BucketPool member):
    // its trivially-destructible bool is constructed first (BucketPool's ctor sets
    // it) and so outlives ~BucketPool, which is exactly what lets a post-teardown
    // deallocate() read a valid `false`. Folding it into BucketPool would reintroduce
    // the use-after-teardown this guards against.
    QB_ABI_ANCHOR static bool &
    pool_alive() noexcept {
        thread_local bool alive = false;
        return alive;
    }

    // Thread_local RAII holder for the bucket free-lists. Its destructor runs at
    // thread exit and returns every parked block to the global allocator, so a
    // spawned worker thread does not leak its frame pool when it terminates.
    // Blocks were allocated bucket-sized (idx * kAlign) and cache-line aligned, and are
    // freed with the aligned -- UNSIZED -- ::operator delete; the size is a hint, not a contract.
    struct BucketPool {
        BucketArray heads{};
        BucketPool() noexcept {
            pool_alive() = true;
        }
        ~BucketPool() noexcept {
            pool_alive() = false;
            for (std::size_t i = 0; i < kMaxBucket; ++i) {
                void *p = heads[i];
                // (i + 1) * kAlign is the block size, deliberately NOT passed: see above.
                while (p) {
                    void *next = *static_cast<void **>(p);
                    ::operator delete(p, std::align_val_t{kAlign});
                    p = next;
                }
                heads[i] = nullptr;
            }
        }
    };

    QB_ABI_ANCHOR static BucketArray &
    buckets() noexcept {
        thread_local BucketPool pool;
        return pool.heads;
    }
};

// Mixin helper: inherit from this (or paste via explicit forwarding) to get
// the pooled new/delete pair. We inject the operators via a macro rather
// than a CRTP base to keep the promise layout identical (coroutine frames
// are sensitive to promise size / alignment).
#define QB_CORO_PROMISE_POOLED_NEW_DELETE()                                                               \
    static void *operator new(std::size_t sz) {                                                           \
        return ::qb::io::async::detail::CoroutineFrameAllocator::allocate(sz);                            \
    }                                                                                                     \
    static void operator delete(void *p, std::size_t sz) noexcept {                                       \
        ::qb::io::async::detail::CoroutineFrameAllocator::deallocate(p, sz);                              \
    }                                                                                                     \
    static void operator delete(void *p) noexcept {                                                       \
        /* Unsized delete fallback (used when sized-deallocation is off).   */                            \
        /* We cannot recover the original bucket index, so we surrender the */                            \
        /* block to the global allocator — correct but slower. Modern C++20 */                            \
        /* compilers default to sized deallocation, so this path is cold.   */                            \
        /* Match allocate()'s aligned ::operator new.                       */                            \
        ::operator delete(p, std::align_val_t{::qb::io::async::detail::CoroutineFrameAllocator::kAlign}); \
    }

/**
 * @brief Storage stand-in for a coroutine result type.
 *
 * `task<void>` is the framework's DEFAULT task type, but `void` is not an object type:
 * it cannot sit in a `std::tuple`, a `std::vector`, or a `std::optional`. Every aggregate
 * combinator that STORES per-branch results (`when_all`, `when_any(vector)`, `parallel`,
 * `parallel_map`) therefore has to route the result type through this trait instead of
 * using `Task::value_type` directly — otherwise the whole family rejects `task<void>` with
 * a hard compile error deep inside `<tuple>`/`<optional>` ("field has incomplete type
 * 'void'").
 *
 * `void` maps to `std::monostate`: a real, empty, default-constructible object type. That
 * keeps ONE uniform rule and preserves positional indexing for MIXED packs — e.g.
 * `co_await when_all(void_task(), int_task())` binds to `[std::monostate, int]` — instead
 * of silently renumbering the tuple when a branch happens to return nothing.
 *
 * Pair it with `if constexpr (std::is_void_v<...>)` at the `co_await`-and-store site: a
 * void branch is awaited for its effect and its slot is simply left value-initialised.
 */
template <typename T>
struct value_slot {
    using type = T;
};
template <>
struct value_slot<void> {
    using type = std::monostate;
};
template <typename T>
using value_slot_t = typename value_slot<T>::type;

} // namespace detail

// schedule_via_current is defined in scheduler.h - include after this file
// or use the externally defined version

/**
 * @brief Coroutine task type for QB async operations
 *
 * task<T> is the primary return type for coroutines in the QB framework.
 * It integrates with the libev event loop for suspend/resume operations.
 *
 * USAGE GUIDELINES:
 * ================
 *
 * Basic Usage:
 * @code
 * qb::io::async::task<int> fetch_data() {
 *     co_await qb::io::async::sleep(std::chrono::milliseconds(100));
 *     co_return 42;
 * }
 * @endcode
 *
 * Spawning Tasks:
 * @code
 * auto t = fetch_data();
 * qb::io::async::coro_scheduler().spawn(std::move(t));  // Note: std::move required
 * @endcode
 *
 * Awaiting Tasks:
 * @code
 * task<void> caller() {
 *     int result = co_await fetch_data();  // Suspends until fetch_data completes
 *     std::cout << "Got: " << result << "\n";
 * }
 * @endcode
 *
 * CRITICAL: Lambda Coroutine Safety
 * ==================================
 * When creating coroutines from lambdas, avoid temporary lambda objects:
 *
 * WRONG - Dangling reference:
 * @code
 * for (int i = 0; i < 5; ++i) {
 *     tasks.push_back([&i]() -> task<int> {  // WRONG: captures &i
 *         co_await sleep(10ms);
 *         co_return i * 10;  // UNDEFINED: i may be out of scope
 *     }());
 * }
 * @endcode
 *
 * CORRECT - Pass by value:
 * @code
 * auto worker = [](int id) -> task<int> {  // id passed by value
 *     co_await sleep(10ms);
 *     co_return id * 10;
 * };
 * for (int i = 0; i < 5; ++i) {
 *     tasks.push_back(worker(i));  // Safe: i copied into parameter
 * }
 * @endcode
 *
 * @tparam T The return type of the coroutine (void for no return value)
 * @ingroup Coroutine
 */
template <typename T = void>
class task {
public:
    using value_type = T;

    /**
     * @brief Promise type for task<T> coroutines
     *
     * Implements the coroutine protocol for integration with libev.
     */
    struct promise_type {
        // Finding 2.A.9: pooled frame allocation (see detail::CoroutineFrameAllocator).
        QB_CORO_PROMISE_POOLED_NEW_DELETE()

        /**
         * @brief Storage for the coroutine result
         *
         * CRITICAL: This variant MUST be explicitly initialized to index 0
         * (std::monostate) in the constructor. Without explicit initialization,
         * the variant may contain a default-constructed T, causing premature
         * ready state and undefined behavior.
         *
         * Index 0: no result yet (monostate) - INITIAL STATE
         * Index 1: success (T) - set by return_value()
         * Index 2: exception (exception_ptr) - set by unhandled_exception()
         */
        std::variant<std::monostate, T, std::exception_ptr> result_;

        /**
         * @brief Continuation handle for coroutine composition
         *
         * When this coroutine completes, this handle is resumed.
         */
        std::coroutine_handle<> continuation_;

        /**
         * @brief Pointer to the scheduler managing this coroutine
         */
        CoroutineScheduler *scheduler_ = nullptr;

        /**
         * @brief Coroutine ID for debugging/tracing
         */
#ifdef QB_DEBUG_COROUTINES
        static inline std::atomic<std::size_t> next_id{0};
        std::size_t                            coro_id_ = next_id.fetch_add(1, std::memory_order_relaxed);
#endif

        /**
         * @brief Constructor - CRITICAL: Explicitly initializes result_ to monostate
         *
         * This explicit initialization is REQUIRED to prevent undefined behavior.
         * Without it, the variant may contain a default-constructed T instead of
         * std::monostate, causing is_ready() to return true prematurely.
         */
        promise_type()
            : result_(std::in_place_index<0>) {}

        /**
         * @brief Construct the task return object
         * @return task bound to this promise
         */
        task
        get_return_object() {
            return task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        /**
         * @brief Initial suspension point
         *
         * Always suspend initially. The scheduler will resume when ready.
         * This allows proper scheduling and lifecycle management.
         */
        std::suspend_always
        initial_suspend() noexcept {
            return {};
        }

        /**
         * @brief Final suspension point
         *
         * Resumes the continuation if one exists, then destroys this coroutine.
         * Uses symmetric transfer for optimal performance.
         */
        auto
        final_suspend() noexcept {
            struct final_awaiter {
                std::coroutine_handle<> continuation_;
                CoroutineScheduler     *scheduler_;
                // The PROMISE, not a copy of its exception_ptr. `final_suspend` runs on every
                // coroutine completion, and on libc++ `exception_ptr`'s copy constructor and
                // destructor are declared out of line — so carrying one here would add two real
                // calls per completion to a hot path, for a value that is null in every run that
                // does not throw. A raw pointer costs a move; the exception is read only on the
                // branch that is about to discard it. The frame is alive throughout (we are
                // suspended IN it, and `defer_frame_destruction` only queues the destroy), so
                // dereferencing it in `await_suspend` is well defined.
                promise_type *promise_;

                bool
                await_ready() const noexcept {
                    return false;
                }

                std::coroutine_handle<>
                await_suspend(std::coroutine_handle<> h) noexcept {
                    // Awaited task: symmetric-transfer to the continuation,
                    // avoiding stack overflow in deep chains. The awaiting
                    // task<T> object owns and frees this frame.
                    if (continuation_)
                        return continuation_;
                    // Detached (spawned) task: no continuation and no task<T>
                    // owner. Hand the frame to the scheduler to destroy on the
                    // current run_ready() drain (we cannot destroy it here — we
                    // are suspended in it). Without this the spawned frame leaks.
                    if (scheduler_) {
                        // …and nobody will ever read its result, so an exception stored by
                        // `unhandled_exception()` is about to be destroyed unobserved. Say so.
                        if (promise_ && promise_->has_exception())
                            report_detached_coroutine_exception(promise_->exception());
                        defer_frame_destruction(h);
                    }
                    return std::noop_coroutine();
                }

                void
                await_resume() noexcept {}
            };
            return final_awaiter{continuation_, scheduler_, this};
        }

        /**
         * @brief Handle unhandled exceptions
         *
         * Stores the exception for rethrowing in await_resume().
         */
        void
        unhandled_exception() noexcept {
            result_.template emplace<2>(std::current_exception());
        }

        /**
         * @brief Store the return value
         * @param value The value to return
         */
        void
        return_value(T value) noexcept(std::is_nothrow_move_constructible_v<T>) {
            result_.template emplace<1>(std::move(value));
        }

        /**
         * @brief Check if the coroutine has completed
         * @return true if result is available (success or exception)
         */
        [[nodiscard]] bool
        is_ready() const noexcept {
            return result_.index() != 0;
        }

        /**
         * @brief Check if the coroutine completed with an exception
         * @return true if an exception was thrown
         */
        [[nodiscard]] bool
        has_exception() const noexcept {
            return result_.index() == 2;
        }

        /**
         * @brief Get the return value
         * @return Reference to the stored value
         * @pre is_ready() && !has_exception()
         */
        T &
        value() noexcept {
            return std::get<1>(result_);
        }

        /**
         * @brief Get the stored exception
         * @return Reference to the exception pointer
         * @pre has_exception()
         */
        std::exception_ptr &
        exception() noexcept {
            return std::get<2>(result_);
        }

#ifdef QB_DEBUG_COROUTINES
        ~promise_type() {
            QB_CORO_TRACE(coro_id_, "promise_destroyed");
        }
#endif
    };

    using handle_type = std::coroutine_handle<promise_type>;

    /**
     * @brief Default-construct an empty task (null handle).
     * @details Owns nothing; `done()`/`await_ready()` are `true`. Used as a movable,
     *          assignable holder (e.g. a slot that later receives a real task, or is
     *          reset to release its frame). Never `co_await` an empty task.
     */
    task() noexcept
        : handle_(nullptr) {}

    /**
     * @brief Construct from a coroutine handle
     * @param h The coroutine handle
     * @private
     */
    explicit task(handle_type h) noexcept
        : handle_(h) {}

    /**
     * @brief Destructor
     *
     * Destroys the coroutine frame. The task must not be in the ready queue.
     */
    ~task() {
        if (handle_) {
            // Destroyed while still in flight (NOT at final_suspend) → a waker may have already
            // queued this frame in the scheduler (schedule_via_current); scrub it before freeing so
            // run_ready() cannot pop a dangling handle (UAF). Gated on !done(): the hot
            // completed-task teardown pays only one done() check. See forget_frame_if_current.
            if (!handle_.done()) {
                forget_frame_if_current(handle_);
            }
            handle_.destroy();
        }
    }

    /**
     * @brief Move constructor
     */
    task(task &&other) noexcept
        : handle_(std::exchange(other.handle_, {})) {}

    /**
     * @brief Move assignment
     */
    task &
    operator=(task &&other) noexcept {
        if (handle_) {
            if (!handle_.done()) {
                forget_frame_if_current(handle_); // see ~task: scrub a still-queued frame before free
            }
            handle_.destroy();
        }
        handle_ = std::exchange(other.handle_, {});
        return *this;
    }

    // Non-copyable
    task(const task &)            = delete;
    task &operator=(const task &) = delete;

    /**
     * @brief Awaitable: check if ready
     * @return true if coroutine is complete
     */
    [[nodiscard]] bool
    await_ready() const noexcept {
        if (!handle_)
            return true;
        if (handle_.done())
            return true;
        return handle_.promise().is_ready();
    }

    /**
     * @brief Awaitable: suspend and set continuation
     * @param caller The coroutine awaiting this task
     * @return Handle to resume (symmetric transfer)
     *
     * Sets the continuation and returns this task's handle for symmetric transfer.
     * The continuation will be resumed when this coroutine completes via final_suspend.
     *
     * IMPLEMENTATION NOTE: This is defined inline to avoid circular dependency
     * with CoroutineScheduler. Symmetric transfer prevents stack overflow.
     */
    std::coroutine_handle<>
    await_suspend(std::coroutine_handle<> caller) noexcept {
        handle_.promise().continuation_ = caller;
        // Symmetric transfer: return the handle to resume
        // The compiler will resume it directly without recursion
        return handle_;
    }

    /**
     * @brief Awaitable: get the result
     * @return The return value
     * @throws Any exception thrown by the coroutine
     *
     * Finding 2.A.3: the variant index is consulted explicitly. The old
     * path called `value()` → `std::get<1>(result_)`, which would throw
     * `std::bad_variant_access` if the coroutine happened to complete via
     * `unhandled_exception()` **and** the caller forgot to check
     * `has_exception()`. That can hide the real error. Here we always
     * surface the stored exception first.
     */
    T
    await_resume() {
        // Guard the empty/moved-from task: handle_ is null after a default-construct,
        // a move, or detach(). promise() on a null handle is UB (it dereferences a null
        // coroutine frame → SEGV in the result-variant access). Fail loudly instead.
        if (!handle_)
            throw std::logic_error("task<T>::await_resume called on an empty/moved-from task");
        auto &promise = handle_.promise();
        if (promise.has_exception()) {
            std::rethrow_exception(promise.exception());
        }
        if (!promise.is_ready()) {
            // Should never happen: await_suspend must have kept us
            // suspended until the promise completed. Fail loudly rather
            // than hand back a garbage T via an uninitialised variant.
            throw std::logic_error("task<T>::await_resume called before the coroutine completed");
        }
        return std::move(promise.value());
    }

    /**
     * @brief Get the coroutine handle
     * @return The handle (may be null)
     */
    [[nodiscard]] handle_type
    handle() const noexcept {
        return handle_;
    }

    /**
     * @brief Detach the coroutine handle from this task
     * @return The handle (may be null)
     *
     * This transfers ownership of the handle to the caller.
     * The task will no longer own the handle and will not
     * destroy it in its destructor.
     */
    [[nodiscard]] handle_type
    detach() noexcept {
        return std::exchange(handle_, {});
    }

    /**
     * @brief Check if the coroutine has completed
     * @return true if done
     */
    [[nodiscard]] bool
    done() const noexcept {
        return !handle_ || handle_.done();
    }

    /**
     * @brief Check if the coroutine is valid
     * @return true if handle is not null
     */
    [[nodiscard]] explicit
    operator bool() const noexcept {
        return handle_ != nullptr;
    }

private:
    handle_type handle_;
};

/**
 * @brief Specialization for void return type
 * @ingroup Coroutine
 */
template <>
class task<void> {
public:
    using value_type = void;

    /**
     * @brief Promise type for task<void>
     */
    struct promise_type {
        // Finding 2.A.9: pooled frame allocation (see detail::CoroutineFrameAllocator).
        QB_CORO_PROMISE_POOLED_NEW_DELETE()

        /**
         * @brief Stored exception (if any)
         */
        std::exception_ptr exception_;

        /**
         * @brief Continuation for coroutine composition
         */
        std::coroutine_handle<> continuation_;

        /**
         * @brief Scheduler pointer
         */
        CoroutineScheduler *scheduler_ = nullptr;

        /**
         * @brief Completion flag
         */
        bool completed_ = false;

#ifdef QB_DEBUG_COROUTINES
        static inline std::atomic<std::size_t> next_id{0};
        std::size_t                            coro_id_ = next_id.fetch_add(1, std::memory_order_relaxed);
#endif

        task
        get_return_object() noexcept {
            return task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        std::suspend_always
        initial_suspend() noexcept {
#ifdef QB_DEBUG_COROUTINES
            QB_CORO_TRACE(coro_id_, "initial_suspend");
#endif
            return {};
        }

        auto
        final_suspend() noexcept {
            struct final_awaiter {
                std::coroutine_handle<> continuation_;
                CoroutineScheduler     *scheduler_;
                promise_type           *promise_; // see the note on task<T>'s final_awaiter
#ifdef QB_DEBUG_COROUTINES
                std::size_t coro_id_;
#endif

                bool
                await_ready() const noexcept {
                    return false;
                }

                std::coroutine_handle<>
                await_suspend(std::coroutine_handle<> h) noexcept {
#ifdef QB_DEBUG_COROUTINES
                    QB_CORO_TRACE(coro_id_, "final_suspend");
#endif
                    // Awaited: symmetric-transfer to the continuation (owner frees
                    // this frame). Detached (spawned): hand the frame to the
                    // scheduler to destroy on the run_ready() drain — it has no
                    // owner and cannot destroy itself here, so it would leak.
                    if (continuation_)
                        return continuation_;
                    if (scheduler_) {
                        // …and no owner means no `await_resume()`, so a stored exception would
                        // be destroyed unobserved. Report it rather than drop it.
                        if (promise_ && promise_->exception_)
                            report_detached_coroutine_exception(promise_->exception_);
                        defer_frame_destruction(h);
                    }
                    return std::noop_coroutine();
                }

                void
                await_resume() noexcept {}
            };
            return final_awaiter{
                continuation_, scheduler_, this
#ifdef QB_DEBUG_COROUTINES
                ,
                coro_id_
#endif
            };
        }

        void
        unhandled_exception() noexcept {
            exception_ = std::current_exception();
        }

        void
        return_void() noexcept {
            completed_ = true;
        }

        [[nodiscard]] bool
        is_ready() const noexcept {
            return completed_ || exception_ != nullptr;
        }

        [[nodiscard]] bool
        has_exception() const noexcept {
            return exception_ != nullptr;
        }

#ifdef QB_DEBUG_COROUTINES
        ~promise_type() {
            QB_CORO_TRACE(coro_id_, "promise_destroyed");
        }
#endif
    };

    using handle_type = std::coroutine_handle<promise_type>;

    /** @brief Default-construct an empty task (null handle); owns nothing. */
    task() noexcept
        : handle_(nullptr) {}

    explicit task(handle_type h) noexcept
        : handle_(h) {}

    ~task() {
        if (handle_) {
            // See task<T>::~task: scrub a still-in-flight frame from the scheduler before freeing
            // it, so a waker that already queued it cannot leave a dangling handle for run_ready().
            if (!handle_.done()) {
                forget_frame_if_current(handle_);
            }
            handle_.destroy();
        }
    }

    task(task &&other) noexcept
        : handle_(std::exchange(other.handle_, {})) {}

    task &
    operator=(task &&other) noexcept {
        if (handle_) {
            if (!handle_.done()) {
                forget_frame_if_current(handle_); // see ~task: scrub a still-queued frame before free
            }
            handle_.destroy();
        }
        handle_ = std::exchange(other.handle_, {});
        return *this;
    }

    task(const task &)            = delete;
    task &operator=(const task &) = delete;

    [[nodiscard]] bool
    await_ready() const noexcept {
        if (!handle_)
            return true;
        if (handle_.done())
            return true;
        return handle_.promise().is_ready();
    }

    /**
     * @brief Awaitable: suspend and set continuation
     * @param caller The coroutine awaiting this task
     * @return Handle to resume (symmetric transfer)
     *
     * Sets the continuation and returns this task's handle for symmetric transfer.
     * The continuation will be resumed when this coroutine completes via final_suspend.
     *
     * IMPLEMENTATION NOTE: This is defined inline to avoid circular dependency
     * with CoroutineScheduler. Symmetric transfer prevents stack overflow.
     */
    std::coroutine_handle<>
    await_suspend(std::coroutine_handle<> caller) noexcept {
        handle_.promise().continuation_ = caller;
        // Symmetric transfer: return the handle to resume
        return handle_;
    }

    void
    await_resume() {
        // Guard the empty/moved-from task (see task<T>::await_resume): promise() on a
        // null handle is UB. Fail loudly instead of dereferencing a null frame.
        if (!handle_)
            throw std::logic_error("task<void>::await_resume called on an empty/moved-from task");
        if (handle_.promise().has_exception()) {
            std::rethrow_exception(handle_.promise().exception_);
        }
    }

    [[nodiscard]] handle_type
    handle() const noexcept {
        return handle_;
    }

    /**
     * @brief Detach the coroutine handle from this task
     * @return The handle (may be null)
     *
     * This transfers ownership of the handle to the caller.
     * The task will no longer own the handle and will not
     * destroy it in its destructor.
     */
    [[nodiscard]] handle_type
    detach() noexcept {
        return std::exchange(handle_, {});
    }

    [[nodiscard]] bool
    done() const noexcept {
        return !handle_ || handle_.done();
    }
    [[nodiscard]] explicit
    operator bool() const noexcept {
        return handle_ != nullptr;
    }

private:
    handle_type handle_;
};

} // namespace qb::io::async

// The two free functions declared near the top of this file (defer_frame_destruction,
// forget_frame_if_current) are DEFINED in scheduler.h, which cannot be included above because it
// needs a complete task<T>. Pulling it here -- after task<T> is complete, still inside the guard
// -- is the position that works in BOTH orders: entering through task.h completes task<T> and
// then scheduler.h; entering through scheduler.h reaches task.h at its line 63, and this include
// is a no-op because scheduler.h's own guard is already set.
//
// Without it, task.h ALONE (and generator.h, which calls forget_frame_if_current and includes
// only task.h) compiles clean and cannot link ~task<T> -- i.e. cannot destroy the very type the
// header exists to provide. `-c` and `-fsyntax-only` both pass; only a link says so. Adding
// `inline` to the declarations is necessary (it is the ODR fix and it re-arms
// -Wundefined-inline) but NOT sufficient: an inline function must be defined in every TU that
// uses it, and that is what this include guarantees.
#include "scheduler.h"

#endif // QB_IO_ASYNC_COROUTINE_TASK_H
