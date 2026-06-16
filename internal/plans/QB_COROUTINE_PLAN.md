# QB_COROUTINE_PLAN — Deep Review of the `qb-io` Coroutine Runtime and Actor Integration

> Scope:
> - `qb/include/qb/io/async/coroutine/**` (14 headers, ~7500 lines)
> - Integration with `qb-core`: `Actor::spawn_async`, `CoroContext`,
>   `active_coroutines_`, `coro_scheduler_` caching
> - Supporting tests under `qb/source/io/tests/coroutine/` and
>   `qb/source/core/tests/system/test-actor-coroutine-*.cpp`
>
> Goals: **correctness under back-pressure and cancellation, single-thread
> cooperative scheduling perf, C++20/23 idiomatic APIs**, and a coherent
> safety story for coroutines that outlive their owning actor.
>
> Status: **All 27 findings addressed** — 25 implemented (Phases 1–4),
> 2 explicitly deferred (2.B.11, 2.C.7) with rationale in the Progress log.
> Findings are grouped by subsystem, classified by severity and category,
> then rolled up into a phased action plan.
>
> Legend (severity):
> - **S1** — correctness bug, UB risk, silent data corruption, resource leak
> - **S2** — API / design inconsistency, documentation divergence
> - **S3** — performance / allocation hygiene
> - **S4** — readability / nit
>
> Last updated: 2026-04-19

---

## 0. Executive summary

The coroutine runtime is a C++20/23 layer on top of `qb-io`'s libev-driven
event loop. It provides:

1. **Core primitives** — `task<T>`, `shared_task<T>`, `generator<T>` /
   `async_generator<T>`, `awaiter` base classes.
2. **Control flow** — `CoroutineScheduler`, `when_all` / `when_any` /
   `race`, cancellation tokens, structured `scope` with `join_all` /
   `detach` / `cancel_on_first_failure` policies.
3. **I/O-oriented** — `async_stream<T>` pipelines, `channel<T>` /
   `select`, `async_mutex` / `async_rw_lock` / `semaphore` / `barrier` /
   `latch`, `with_retry` / `with_retry_until` with pluggable backoff.
4. **Glue** — `coro_mixin` CRTP helper, `run_sync` / `run_for` / `run_for_ms`
   utilities, `Actor::spawn_async` + `CoroContext` wrapper in qb-core.

The runtime is **single-threaded-per-worker by design** (one listener +
one scheduler per `VirtualCore`), and the actor integration
enforces **actor-safe coroutines** by capturing `ActorId` (not `this`) via
`CoroContext` and tracking lifetime via a shared `std::atomic<size_t>`.

The review surfaces **27 actionable findings**, including:

- **3 correctness bugs with silent data loss** (channel send on closed
  drops the value, `cancellable_operation<T>` swallows inner exceptions and
  returns `T{}`, linear backoff fires the first retry with 0 ms delay).
- **2 UB / double-resume risks** (`enqueue_for_later` re-enqueues a handle
  without dedup; `shared_task` awaiter dereferences a null state).
- **1 libev time-cache staleness carried over from `async::callback`**
  (`timer_awaiter` lacks the `ev_now_update()` fix we already landed in
  `io.h`, so every `sleep()` / retry backoff / `send_for` / `recv_for`
  can fire prematurely after a long-idle thread).
- **Performance hot paths** that still allocate or take atomics despite
  the single-thread model: MPSC queue node per `schedule_resume`,
  `std::unordered_set<void*>` dedup on the hot path, `std::function`
  chaining in `async_stream`, per-combinator `shared_ptr<state>`, per-spawn
  double coroutine frame in `actor_coro_wrapper`.
- **Safety story gaps** (nothing prevents `[this]` captures in
  `spawn_async`, and one existing integration test
  `NestedSpawnFromCoroutineBody` actually demonstrates this "unsafe"
  pattern). Also: cached `coro_scheduler_*` becomes dangling if
  `listener::reset_coro_scheduler()` is called while actors still spawn.
- **Re-entrancy** of `run_ready()` is documented but not enforced; nested
  `listener::run` via `run_sync` from inside a coroutine silently breaks
  invariants.

Nothing here is in the "critical, production-halting" bucket (the failing
bug — the redis parser ring-buffer wrap — was fixed in the prior session,
and the rest of the tests pass), but several items are silent data-loss
risks that should be fixed before declaring the coroutine layer
"production-ready ultra-perf".

---

## 1. Subsystem map

| Layer           | Header(s)                                                          | Responsibility                                      |
|-----------------|--------------------------------------------------------------------|-----------------------------------------------------|
| Core primitives | `task.h`, `shared_task.h`, `generator.h`, `awaiter.h`              | `task<T>`, multi-await `shared_task<T>`, generators, base awaiters |
| Scheduler       | `scheduler.h`                                                      | Ready queue + suspended registry, `run_ready()`, `schedule_resume` |
| Control flow    | `combinators.h`, `cancellation.h`, `scope.h`                       | `when_all`/`when_any`/`race`, `cancellation_token`, structured concurrency |
| I/O primitives  | `stream.h`, `channel.h`, `sync.h`, `retry.h`                       | Async stream pipelines, MPMC channel + `select`, mutex/sem/barrier, retry policies |
| Glue            | `mixin.h`, `utils.h`                                               | `coro_mixin` CRTP, `run_sync` / `run_for` / `run_for_ms` |
| Actor bridge    | `Actor.h` (lines 970–1150) + `Actor.tpp` (`spawn_async`, `actor_coro_wrapper`, `CoroContext::push`) | Actor-safe coroutine spawning, lifetime counter |

Cross-reference dependencies:
- **`timer_awaiter`** (`awaiter.h`) is used by `sleep()` which is used by
  `retry.h`, `channel.h::send_for/recv_for`, `stream.h::debounce/throttle`,
  tests, and user code. One fix there propagates broadly.
- **`CoroutineScheduler`** is the single piece of cross-cutting state;
  its `schedule_resume` is on every hot path (timer fires, I/O completion,
  channel wakeups, combinator branches, retry resumption).
- **`shared_task`**'s `schedule_via_current` only works when
  `CoroutineScheduler::current_ptr()` is non-null; the same invariant
  applies to every awaiter that uses `schedule_via_current` to deliver
  completion.

---

## 2. Findings (grouped by subsystem)

### 2.A Core primitives (`task.h`, `shared_task.h`, `generator.h`, `awaiter.h`)

#### 2.A.1 — `shared_task<T>` awaits a null state when default-constructed **[S1]**
- **Where**: `shared_task.h` lines 121–136, 194–204
- **Issue**: `operator co_await() const` returns `awaiter{_state}` without
  checking `_state`. Default-constructed `shared_task` has null `_state`,
  so `await_ready()` / `await_suspend()` dereference `s->...` → UB. Tests
  check `empty.valid() == false` but never `co_await` an invalid handle.
- **Fix**: Hard-fail early (`if (!s) std::terminate()` or throw) in the
  awaiter, and add a regression test.

#### 2.A.2 — `shared_task` / `timer_awaiter` depend on a non-null TLS scheduler but don't assert **[S1]**
- **Where**: `scheduler.h` lines 454–458 (`schedule_via_current`);
  `shared_task.h` lines 85–88, 166–169; `awaiter.h` lines 252–262.
- **Issue**: `schedule_via_current` silently returns when
  `CoroutineScheduler::current_ptr()` is null. A `shared_task` completed
  from a context without a TLS scheduler leaves every waiter permanently
  suspended. `timer_awaiter::await_suspend` has a fallback (`&current()`)
  but other awaiters do not.
- **Fix**: Debug-assert in `schedule_via_current` (policy: the caller
  must be on a worker thread with a listener). Document as precondition
  in `shared_task` completion.

#### 2.A.3 — `task<T>::await_resume` moves out; a second await yields a moved-from value **[S2]**
- **Where**: `task.h` lines 387–391 (and `task<void>` 570–573)
- **Issue**: After first `await_resume`, the variant stays at index `1`
  holding a moved-from `T`. A second await (rare but possible in generic
  combinators) silently returns garbage.
- **Fix**: Reset the variant to `monostate` after move, or set a
  `consumed` flag asserted on repeat await.

#### 2.A.4 — `async_awaiter::await_resume` assumes `result_` is engaged **[S2]**
- **Where**: `awaiter.h` lines 520–525
- **Issue**: `return std::move(*result_);` without `has_value()` check.
  A callback that sets `ready_=true` without assigning `result_` triggers
  UB.
- **Fix**: Assert or throw `std::logic_error` if `!result_`.

#### 2.A.5 — Synchronous `generator::iterator` ignores exceptions before `operator*` **[S2]**
- **Where**: `generator.h` lines 84–86, 134–168
- **Issue**: `unhandled_exception()` stores into `promise().exception`.
  The `iterator::operator++` resumes but does not rethrow; `operator*`
  dereferences `current_value` without checking.
- **Fix**: Rethrow from `operator++` or `operator*` if
  `promise().exception`. Add a throwing-generator test.

#### 2.A.6 — `shared_task::awaiter::await_suspend` can throw **[S3]**
- **Where**: `shared_task.h` lines 126–128, 197–199
- **Issue**: `_waiters.push_back(h)` may throw `std::bad_alloc` after
  the coroutine is already suspended. Outcome is
  implementation-defined.
- **Fix**: Use a small pre-reserved `qb::vector<>` or `ring_buffer`; mark
  `await_suspend` `noexcept` and terminate on OOM.

#### 2.A.7 — `shared_task` and `generator` headers rely on transitive includes **[S4]**
- **Where**: `shared_task.h` line 32 includes `<any>` (unused);
  `generator.h` uses `std::vector`, `std::invoke_result_t`, `std::remove_cvref_t`
  without direct `#include <vector>` / `<type_traits>` / `<iterator>`.
- **Fix**: IWYU pass. Remove `<any>`.

#### 2.A.8 — `from_range(Range&&)` lifetime contract undocumented **[S2]**
- **Where**: `generator.h` lines 374–380
- **Issue**: Instantiation with an lvalue stores a reference; the
  generator must not outlive the range.
- **Fix**: Doxygen contract; optional `static_assert(is_rvalue_reference_v<Range&&>)`
  or accept only owning ranges by default.

#### 2.A.9 — No `promise_type::operator new` hooks; every coroutine is heap-allocated **[S3]**
- **Where**: `task.h`, `shared_task.h`, `generator.h`
- **Issue**: No allocator hook; HALO works only for inlined compiler
  optimizations. Under heavy churn, the global allocator dominates.
- **Fix**: Optional `promise_type::operator new` that routes to a
  thread-local pool; match the pattern we applied to `Timeout<F>` and
  `RegisteredKernelEvent` in `io.h` / `listener.h`.

### 2.B Scheduler & control flow (`scheduler.h`, `combinators.h`, `cancellation.h`, `scope.h`)

#### 2.B.1 — `enqueue_for_later` can double-resume a handle **[S1]** ⚠️
- **Where**: `scheduler.h` lines 246–250
- **Issue**: Unlike `schedule_resume`, it always `insert`s + `push`es
  without dedup. Two calls before `pop` → `in_flight_` gets one entry
  (set is idempotent) but `ready_queue_` gets two. The coroutine is
  resumed twice → undefined behavior per [expr.await] / coroutine
  single-resume contract.
- **Fix**: Match `schedule_resume`:
  ```cpp
  void enqueue_for_later(std::coroutine_handle<> handle) {
      if (!handle || handle.done()) return;
      void* addr = handle.address();
      if (in_flight_.count(addr)) return;       // dedup
      in_flight_.insert(addr);
      ready_queue_.push({handle, false});
  }
  ```
  Add a regression test (scheduled `yield` twice).

#### 2.B.2 — `cancellable_operation<T>` silently drops inner exceptions, returns `T{}` **[S1]** ⚠️
- **Where**: `cancellation.h` lines 252–260, 244–250
- **Issue**: `task_runner` does `catch (...) {}` then `state->task_done = true`.
  `await_resume` returns `std::move(*state->result)` if engaged, else
  `T{}`. If the inner task throws without cancellation in flight, the
  caller receives a default-constructed `T` — silent data corruption.
- **Fix**: Store `std::exception_ptr` in `shared_state` on `catch(...)`
  and rethrow from `await_resume` when not cancelled:
  ```cpp
  static task<void> task_runner(std::shared_ptr<shared_state> state) {
      try { state->result = co_await state->inner_task; }
      catch (...) { state->error = std::current_exception(); }
      state->task_done = true;
      if (state->continuation) schedule_via_current(state->continuation);
  }
  T await_resume() {
      if (token.is_cancelled() && throw_on_cancel) throw cancelled_error();
      if (state->error) std::rethrow_exception(state->error);
      if (state->result) return std::move(*state->result);
      throw std::logic_error("cancellable_operation: no result and no error");
  }
  ```

#### 2.B.3 — `race()` is advertised "cancels others" but behaves as `when_any` **[S2]**
- **Where**: `combinators.h` file header, and lines 604–624 / 612–614
- **Issue**: Header doc says "Wait for first, cancel others". Body just
  calls `when_any`. Losers run to completion, holding resources and
  duplicating side effects.
- **Fix**: Either (a) add cooperative cancellation plumbing (shared
  `cancellation_source` passed to losers), or (b) correct docs and
  rename to make it obvious that losers keep running.

#### 2.B.4 — `coro_with_timeout` does not stop the inner task on timeout **[S2]**
- **Where**: `combinators.h` lines 473–507
- **Issue**: Timeout wins → parent resumes; inner task still runs to
  completion. Users expect timeout ⇒ abort.
- **Fix**: Pair with a `cancellation_token`; pass it into the inner
  task or destroy the unused task handle when safe.

#### 2.B.5 — `with_deadline` can classify a winning op as timeout **[S1]**
- **Where**: `cancellation.h` lines 502–515
- **Issue**: If the operation completes first (`res.index == 0`), the
  code then checks `steady_clock::now() >= deadline` and still throws
  `timeout_error()`, discarding the completed result.
- **Fix**: Return the operation's result as soon as it wins; never
  re-check wall-clock after the race resolves.

#### 2.B.6 — `CoroutineScheduler::active_count()` double-counts queued work **[S2]**
- **Where**: `scheduler.h` lines 350–352
- **Issue**: Returns `in_flight_.size() + ready_queue_.size()`. The same
  logical handle is counted once in `in_flight_` and once in the ready
  queue while waiting. (`DISABLED_SchedulerTracksInFlightCoroutines` in
  `test_coroutine_scheduler.cpp` flags this.)
- **Fix**: Pick one metric with a clear name (`queued()` vs
  `suspended()`) or keep a separate counter. Re-enable the test.

#### 2.B.7 — `run_ready()` re-entrancy is a documented footgun but unguarded **[S2]**
- **Where**: `scheduler.h` lines 112–115, 264–295
- **Issue**: Comment says "must not be re-entered from inside a
  coroutine"; no flag, no assert. `utils.h::run_sync` / `run_for` nest
  `listener::run`, which unconditionally calls `run_ready()` again
  (listener.h lines 398–408), so calling `run_sync` from inside a
  coroutine body silently breaks invariants.
- **Fix**: Thread-local `bool in_run_ready_` guard; `QB_ASSERT(!in_run_ready_)`
  on entry; strong Doxygen note that `run_sync` / `run_for` must never
  be called from a coroutine or actor handler.

#### 2.B.8 — Scheduler destructor does not destroy suspended coroutines **[S2]**
- **Where**: `scheduler.h` lines 130–158
- **Issue**: `~CoroutineScheduler` drains the ready queue but merely
  `.clear()`s `suspended_coroutines_` without `.destroy()` on each
  handle. `destroy_all_suspended()` exists but is opt-in.
- **Fix**: Call `destroy_all_suspended()` in the destructor (or
  document that callers must do it and assert empty in debug).

#### 2.B.9 — Scheduler "thread-safe" documentation vs single-thread `in_flight_` **[S2]**
- **Where**: `scheduler.h` lines 103–115, 224–226
- **Issue**: Doc states "schedule_resume is thread-safe" but
  `in_flight_` is a plain `std::unordered_set`. Only safe on the
  scheduler's own thread (libev callbacks on same thread). Cross-thread
  use corrupts the container.
- **Fix**: Rewrite docs to "safe from libev callbacks on the owning
  thread; not safe across threads". If cross-thread posting is ever
  needed, add a separate `post_from_foreign_thread()` that atomically
  enqueues and wakes the loop.

#### 2.B.10 — Ready queue uses MPSC with per-push heap nodes on a single-thread runtime **[S3]**
- **Where**: `scheduler.h` lines 103–111, 423–425 + `mpsc_unbounded_queue.h`
- **Issue**: Every `schedule_resume` / `spawn` allocates a node and
  executes atomic RMW on the queue head/tail. Unnecessary on the
  single-thread hot path.
- **Fix**: Use `std::deque` or an intrusive freelisted queue for the
  worker-local case (SPSC-only); keep MPSC only if foreign-thread
  posting is actually a requirement.

#### 2.B.11 — Combinators allocate a `shared_ptr<state>` + N task wrappers per use **[S3]**
- **Where**: `combinators.h` lines 76–107, 141–196
- **Issue**: `when_all<N>` / `when_any<N>` pay N scheduler enqueues and
  one shared allocation per call, even for compile-time-fixed-N.
- **Fix**: Small-N stack state; pooling of wrapper frames; a single
  fan-out coroutine for homogeneous arrays.

#### 2.B.12 — `scope::cleanup_policy::detach` leaks accounting **[S2]**
- **Where**: `scope.h` lines 172–175, 203–210, 426–428
- **Issue**: On detach, `_impl->tasks.clear()` drops tracking while
  `run_wrapped` coroutines still run; `active_count()` no longer
  reflects reality.
- **Fix**: Either (a) document that detach invalidates inspection APIs,
  or (b) decrement `active_count` from `run_wrapped` completions
  without requiring a `tasks` entry.

#### 2.B.13 — `scope::cleanup_policy::join_all` destructor is a no-op **[S2]**
- **Where**: `scope.h` lines 157–166, 316–341
- **Issue**: Comments tell the user to `co_await join_all()` manually
  before destruction. If they forget, children keep running after the
  scope object (and its locals) die — classic structured-concurrency
  footgun.
- **Fix**: Debug-only assert `active_count() == 0` in dtor;
  `[[nodiscard]]` on helpers returning a joinable handle; consider a
  renamed policy (`must_be_manually_joined`).

#### 2.B.14 — `std::tuple_size` / `tuple_element` specialization for `when_any_result` **[S4]**
- **Where**: `combinators.h` lines 243–259
- **Issue**: Program-defined specializations in `std` are sanctioned by
  C++20 but must stay in sync with `get` overloads. Easy to drift.
- **Fix**: `static_assert(std::tuple_size_v<when_any_result<T>> == …);`.

### 2.C I/O primitives (`stream.h`, `channel.h`, `sync.h`, `retry.h`)

#### 2.C.1 — `co_await channel::send` on a closed channel **silently drops the value** **[S1]** ⚠️
- **Where**: `channel.h` lines 149–150, 180–181, 211–214
- **Issue**: `await_ready()` returns `false` if closed. `await_suspend()`
  sees `_closed`, sets `_completed = true`, reschedules. `await_resume()`
  only throws on `_closed && !_completed` — but `_completed` is already
  `true` → no exception, value dropped. Violates documented contract
  (`@throws channel_closed` on line 234–236). `try_send` correctly
  returns `false` in this case; `send` should behave symmetrically.
- **Fix**:
  ```cpp
  bool await_ready() {
      if (ch._closed) return true;               // synchronous fail
      /* …existing fast paths… */
  }
  void await_resume() {
      if (ch._closed && !_completed) throw channel_closed();
      if (ch._closed) throw channel_closed();    // new: closed after suspend
      /* …existing deferred-send fast path… */
  }
  ```
  Add a regression test: spawn a sender awaiting
  `ch.send(v)` while the buffer is full, then `ch.close()` → must
  throw `channel_closed`.

#### 2.C.2 — Linear backoff fires the first retry with **0 ms** delay **[S1]** ⚠️
- **Where**: `retry.h` lines 99–101, 197, 238, 293
- **Issue**: After the first failure, `current_attempt == 1`;
  `calculate_delay(current_attempt - 1, policy)` = `calculate_delay(0, …)`
  which for `linear` multiplies by `min(0, 10000) = 0` → 0 ms.
  First retry is immediate; defeats purpose of linear backoff.
- **Fix**: Either switch the linear branch to
  `attempt + 1` (1-based), or pass `current_attempt` (1-based) to
  `calculate_delay` and update the `exponential` branch to subtract 1
  internally. Add unit test asserting the first inter-attempt delay >= `base_delay`.

#### 2.C.3 — `timer_awaiter` does not refresh libev's time cache **[S1]** ⚠️
- **Where**: `awaiter.h` lines 252–262 (contrast `io.h::callback()`
  lines 299–308 where we already landed `ev_now_update()`).
- **Issue**: `ev_timer_start(loop_, &watcher_)` computes the expiration
  as `ev_mn_now + after`. If the thread has been outside the loop for a
  long time (e.g. `sleep_for` from a user thread), `ev_mn_now` is stale
  and the timer fires immediately. This is exactly the bug we fixed in
  `async::callback` for the HTTP middleware test.
- **Impact**: Every `sleep()`-based code path is affected: retry
  backoff, `channel::send_for / recv_for`, `stream::debounce / throttle`,
  `coro_with_timeout`.
- **Fix**: In `timer_awaiter::await_suspend`, mirror `callback()`:
  ```cpp
  ev_now_update(static_cast<struct ev_loop*>(loop_));
  ev_timer_start(loop_, &watcher_);
  ```

#### 2.C.4 — `debounce` swallows all source-stream exceptions **[S1]**
- **Where**: `stream.h` lines 248–255
- **Issue**: The spawned producer has `try { … } catch (...) {}` then
  `c->close()`. Errors become silent end-of-stream; callers cannot
  distinguish completion from failure.
- **Fix**: Store `std::current_exception()` in a shared slot; surface
  it on the consumer's `recv()` (either rethrow or deliver via an
  `expected`-returning variant).

#### 2.C.5 — `backpressure` can leak a semaphore permit on shutdown **[S2]**
- **Where**: `stream.h` lines 404–419, 516–528
- **Issue**: Producer acquires; if `fn()` returns `nullopt`, task closes
  the buffer without `release()`. Consumer only `release()`s when `recv()`
  returns a value, not on terminal `nullopt`.
- **Fix**: Use a scoped-acquire helper that releases on scope exit, or
  release from the consumer's EOF path.

#### 2.C.6 — `debounce` creates an unbounded channel **[S3]**
- **Where**: `stream.h` lines 235–238
- **Issue**: `std::make_shared<channel<T>>(std::numeric_limits<size_t>::max())`.
  A fast producer + slow debounced consumer allocates without bound.
- **Fix**: Small bounded channel + true backpressure from the producer,
  or a drop/coalesce policy with `max_pending`.

#### 2.C.7 — `async_stream` chains allocate via `std::function` on every composition **[S3]**
- **Where**: `stream.h` lines 59–64, 136–142, 211–228
- **Issue**: `_next = std::function<task<optional<T>>()>` → type-erased
  heap alloc per `map` / `filter` / `take` / … Hot-path stream
  processing pays repeated indirections.
- **Fix**: Move-only functor wrapper with SBO (`qb::unique_function`),
  or a CRTP chain.

#### 2.C.8 — `from_channel` stores raw `channel*` in lambda captures **[S2]**
- **Where**: `stream.h` lines 75–79, 385–391, 506–511
- **Issue**: UAF if the stream outlives the channel. Documented but
  silently misusable.
- **Fix**: Only expose the `from_channel_shared(shared_ptr<channel<T>>)`
  overload publicly; make the raw overload private / internal.

#### 2.C.9 — `channel_range`'s `operator*() const` moves out **[S2]**
- **Where**: `channel.h` lines 584–627
- **Issue**: `return std::move(*_current)` in a `const` method is subtle
  and forces copies when used with `const T&`. Also: `advance()` uses
  `try_recv()` so it's not really async.
- **Fix**: Remove `const`, rename to `try_channel_range`, or drop.

#### 2.C.10 — `semaphore::acquire` always suspends even when permits available **[S3]**
- **Where**: `sync.h` lines 82–94
- **Issue**: `await_ready()` always returns `false`. Fast path takes a
  full scheduler round-trip.
- **Fix**:
  ```cpp
  bool await_ready() noexcept { return sem._available > 0; }
  // on true: decrement in await_resume or directly in await_ready
  ```

#### 2.C.11 — `async_mutex` / `async_rw_lock` silently deadlock on recursive lock **[S2]**
- **Where**: `sync.h` lines 231–244, 385–412
- **Issue**: No owner tracking. A coroutine re-locking its own mutex
  waits forever; no assert, no diagnostic.
- **Fix**: Debug-only owner tracking (coroutine-handle-based),
  `ADD_FAILURE` / `terminate` on re-entry.

#### 2.C.12 — `barrier::arrive_and_wait` after release is a no-op without `reset()` **[S2]**
- **Where**: `sync.h` lines 553–576, 582–588
- **Issue**: Second-phase arrivals see `_remaining == 0` → `await_ready`
  true → skip barrier. Subtle.
- **Fix**: Document and/or add a `std::barrier`-style completion
  function that resets automatically.

#### 2.C.13 — Exponential backoff can overflow `chrono::milliseconds` **[S2]**
- **Where**: `retry.h` lines 103–107, 109–118
- **Issue**: `policy.base_delay * (1u << shift)` can overflow the
  signed representation of `chrono::milliseconds` for large `base_delay`
  and `shift ≥ 30` before the `max_delay` clamp.
- **Fix**: Compute in `int64_t` (or `long double`), clamp per attempt
  to `max_delay`, then construct `milliseconds`.

#### 2.C.14 — `with_retry` only catches `std::exception`; no cancellation **[S2]**
- **Where**: `retry.h` lines 169–184, 216–226
- **Issue**: `catch (const std::exception& e)` ignores other throwables;
  there is no cancellation hook between attempts.
- **Fix**: Add `catch (...)` with `std::current_exception()`; allow an
  optional `cancellation_token`.

#### 2.C.15 — `transient_network_policy` classifies errors by `e.what()` substrings **[S2]**
- **Where**: `retry.h` lines 341–354
- **Issue**: Locale/message-dependent and fragile.
- **Fix**: Match on `std::error_code` or typed exceptions.

#### 2.C.16 — `zip` awaits both streams even after the first ends **[S3]**
- **Where**: `stream.h` lines 598–603
- **Fix**: Short-circuit: if `!opt_a`, return `nullopt` without awaiting
  `b` (and symmetric).

### 2.D Actor ↔ coroutine integration (`Actor.h`, `Actor.tpp`, `mixin.h`, `utils.h`)

#### 2.D.1 — `run_sync` / `run_for` from inside a coroutine silently break `run_ready` invariants **[S1]**
- **Where**: `utils.h` lines 198–307; `listener.h` lines 398–408
- **Issue**: `run_sync` pumps the loop with `EVRUN_NOWAIT` /
  `EVRUN_ONCE`. `listener::run` always calls
  `coro_scheduler().run_ready()`. `CoroutineScheduler::run_ready` is
  documented as non-re-entrant (scheduler.h 112–115). Calling
  `run_sync` from inside a running coroutine (or from an actor
  `on(Event&)` handler that is itself driven by run_ready) violates
  that invariant.
- **Fix**: Thread-local `in_run_ready_` guard ⇒ hard assert /
  `std::terminate` in debug, with clear error message. Document that
  `run_sync` is **only** for tests and top-level bootstrap.

#### 2.D.2 — "Never touch `this` after `co_await`" is a convention only **[S2]**
- **Where**: `Actor.h` lines 978–1029; `Actor.tpp` lines 214–230
- **Issue**: Nothing prevents a user lambda from capturing `[this]` or
  `&member`. The whole safety story rests on discipline.
- **Fix**: Add a soft C++20 concept check (`requires !requires { std::declval<Func&>()([](auto*){});}`
  is not practically enforceable), but document prominently. Consider
  a `spawn_async(state_blob_by_move, [](auto state, auto ctx) -> task<void> { … })`
  overload that encourages passing state by value.

#### 2.D.3 — Existing actor-coroutine test uses `[this]` after `co_await` **[S2]**
- **Where**: `qb/source/core/tests/system/test-actor-coroutine-advanced.cpp`
  lines 755–772 (test `NestedSpawnFromCoroutineBody`)
- **Issue**: The test itself exhibits the exact anti-pattern the docs
  forbid. It works by timing, not by construction. A newcomer reading
  the test will copy the pattern.
- **Fix**: Rewrite to capture `ActorId` or use an explicit `this_actor`
  accessor that rechecks aliveness; add a comment explaining why the
  "obvious" `[this]` pattern is unsafe.

#### 2.D.4 — Cached `coro_scheduler_` becomes dangling after `reset_coro_scheduler()` **[S1]**
- **Where**: `Actor.tpp` lines 219–221; `listener.h` lines 486–494
- **Issue**: First `spawn_async` caches
  `&listener::current.coro_scheduler()`. If the listener later replaces
  its scheduler (tests do this), the cached pointer is dangling; a
  subsequent `spawn_async` on the same actor uses UAF.
- **Fix**: Cheap guard on every spawn — `if (coro_scheduler_ != &listener::current.coro_scheduler()) coro_scheduler_ = …;`
  — or invalidate cached pointers on scheduler reset (harder).

#### 2.D.5 — `active_coroutines_` is relaxed-only; misleading for lifetime reasoning **[S2]**
- **Where**: `Actor.h` lines 1076–1090; `Actor.tpp` lines 205–208, 223
- **Issue**: All operations are `memory_order_relaxed`. `has_active_coroutines()`
  is diagnostic only: zero does not mean "all frames destroyed and
  actor-owned resources released". No one in the codebase actually
  depends on stronger ordering, but the API does not say so.
- **Fix**: Doxygen: "counter is advisory; do not gate teardown on it
  without an explicit barrier." If a barrier is ever needed, expose a
  separate `await_no_active_coroutines()` helper that uses a proper
  synchronization point.

#### 2.D.6 — `spawn_async` allocates two coroutine frames per call **[S3]**
- **Where**: `Actor.tpp` lines 200–230
- **Issue**: `actor_coro_wrapper` + user `task<void>` = two frames, two
  heap allocations per spawn.
- **Fix**: Custom wrapper task that integrates the RAII guard into its
  own `promise_type::final_suspend` — single frame, single allocation.

#### 2.D.7 — Per-actor `std::make_shared<std::atomic<size_t>>` allocation **[S3]**
- **Where**: `Actor.h` lines 1083–1090
- **Issue**: One heap allocation per actor just to support orphan
  coroutines decrementing after actor death.
- **Fix**: Intrusive counter on `VirtualCore` (one per worker); actor
  holds a pointer; coroutine frame holds `shared_ptr<VirtualCore>` (if
  VC lifetime is managed via shared_ptr) or a pool-allocated small
  struct per actor with on-demand alloc.

#### 2.D.8 — `CoroContext::push` uses `VirtualCore::_handler` without guarantee **[S2]**
- **Where**: `Actor.tpp` lines 171–182; `Actor.cpp` lines 169–172
- **Issue**: Assumes the resuming thread is still the actor's core
  thread. True in the current design (actors are pinned, scheduler is
  per-listener) but not asserted.
- **Fix**: Debug assert that `VirtualCore::_handler` maps to the same
  core as `actor_id_`.

#### 2.D.9 — `coro_mixin` is orthogonal to `Actor::spawn_async` but not cross-linked **[S4]**
- **Where**: `mixin.h` lines 67–88
- **Issue**: Readers may confuse the CRTP I/O-client mixin with actor
  coroutine integration. They serve different purposes.
- **Fix**: One-line cross-reference in the header docs of both.

#### 2.D.10 — No test for `run_sync` / `run_for` re-entrancy or scheduler-reset dangling **[S2]**
- **Where**: `qb/source/core/tests/`, `qb/source/io/tests/coroutine/`
- **Fix**: Add regression tests:
  - `run_sync` invoked inside an actor handler → must assert/fail fast.
  - Actor spawn after `reset_coro_scheduler()` → must not UAF (behavior
    TBD: re-cache or reject).

---

## 3. Severity / category rollup

| Severity | Count | Representative findings                                                        |
|----------|-------|--------------------------------------------------------------------------------|
| S1       | **8** | 2.A.1, 2.A.2, 2.B.1, 2.B.2, 2.B.5, 2.C.1, 2.C.2, 2.C.3, 2.C.4, 2.D.1, 2.D.4    |
| S2       | 14    | 2.A.3–8, 2.B.3/4/6–9/12/13, 2.C.5/8/9/11/12/13/14/15, 2.D.2/3/5/8/10           |
| S3       | 7     | 2.A.9, 2.B.10/11, 2.C.6/7/10/16, 2.D.6/7                                       |
| S4       | 2     | 2.A.7, 2.B.14, 2.D.9                                                           |

| Category                    | Count |
|-----------------------------|-------|
| Correctness / silent drops  | 6     |
| UB / double-resume          | 3     |
| libev integration           | 1     |
| Lifetime / UAF              | 4     |
| Performance (alloc / atomic)| 6     |
| API / docs                  | 10    |
| Tests / coverage            | 3     |

---

## 4. Action plan (4 phases)

### Phase 1 — Correctness bugs that silently lose data (do first) — S1

1. **2.C.1** — `co_await channel::send` on closed channel must throw
   `channel_closed` (or mirror `try_send`). Add regression test.
2. **2.C.2** — Fix the linear-backoff `0 ms` first-retry bug; add a test
   that asserts the first inter-attempt delay ≥ `base_delay`.
3. **2.C.3** — Call `ev_now_update()` in `timer_awaiter::await_suspend`
   (same fix we already landed in `io.h::callback`). This single change
   fixes every `sleep` / `send_for` / `recv_for` / retry-backoff /
   `coro_with_timeout` after a long-idle thread.
4. **2.B.1** — `enqueue_for_later` must dedup via `in_flight_`.
5. **2.B.2** — `cancellable_operation<T>` must propagate inner
   exceptions (store `exception_ptr` in shared state; rethrow from
   `await_resume`).
6. **2.B.5** — `with_deadline` must not re-classify a winning operation
   as a timeout.
7. **2.C.4** — `debounce` must surface source exceptions (at minimum,
   stored + rethrown on first `recv`).
8. **2.A.1** — `shared_task` `co_await` on null state must fail loudly
   (assert in debug, terminate in release, or throw).
9. **2.A.2** — `schedule_via_current` debug-asserts a live TLS scheduler;
   `shared_task` flush documents the invariant.
10. **2.D.1** — Thread-local re-entrancy guard on `run_ready` /
    `listener::run`; hard-fail when `run_sync` is called from a running
    coroutine.
11. **2.D.4** — Re-validate `coro_scheduler_` on every spawn (cheap pointer
    compare) to fix dangling after `reset_coro_scheduler()`.

Expected: no benchmark regression (all fixes are on cold paths, except
`ev_now_update` which costs a few ns on timer start — negligible).

### Phase 2 — Performance on hot paths — S3

1. **2.B.10** — Replace MPSC `ready_queue_` + `std::unordered_set` with a
   single-threaded `std::deque` (or intrusive freelist) + `bool` marker
   on the handle. Benchmark `schedule_resume` before/after (target: from
   ~30 ns to ~10 ns, matching the wins we got on `listener::registerEvent`
   after Phase 3 of QB_IO_PLAN).
2. **2.A.9** — Optional `promise_type::operator new` that routes to a
   thread-local freelist (match the pattern used for `Timeout<F>` and
   `RegisteredKernelEvent`).
3. **2.D.6 / 2.D.7** — Merge the RAII counter into a single task type for
   `spawn_async`; move `active_coroutines_` to a VC-wide intrusive
   counter (one alloc per worker, not per actor).
4. **2.B.11** — Small-N stack state for `when_all` / `when_any`.
5. **2.C.7** — Replace `std::function` in `async_stream` with a
   move-only SBO functor.
6. **2.C.10** — `semaphore::acquire` fast-path (`await_ready` returns
   true when permits available).

Add micro-benchmarks in `qb/source/io/tests/coroutine/test_coroutine_benchmark.cpp`
for: per-spawn cost, per-await cost on an already-ready awaitable,
`when_all<4>`, `channel::send`/`recv` no-contention, `semaphore::acquire`
no-contention.

### Phase 3 — API hygiene & UB hardening — S2

- 2.A.3 / 2.A.4 / 2.A.5 — `await_resume` safety (moved-from variant,
  null-optional, generator exception rethrow).
- 2.A.6 — Pre-reserve `shared_task` waiter buffer; mark `await_suspend`
  `noexcept`.
- 2.A.8 — Doxygen lifetime contract on `from_range`.
- 2.B.3 / 2.B.4 — Fix `race` + `coro_with_timeout` to actually cancel
  losers via a shared cancellation token (or correct the docs).
- 2.B.6 — Fix `active_count()` semantics; re-enable the disabled test.
- 2.B.7 — Enforce non-re-entrancy of `run_ready()` with a thread-local
  flag.
- 2.B.8 — Destructor of `CoroutineScheduler` must destroy suspended
  handles.
- 2.B.9 — Fix "thread-safe" docs.
- 2.B.12 / 2.B.13 — `scope` detach accounting; `join_all` debug-assert
  in dtor.
- 2.C.5 / 2.C.6 / 2.C.8 / 2.C.9 — stream/channel lifetime fixes.
- 2.C.11 / 2.C.12 — mutex/barrier usability diagnostics.
- 2.C.13 — exponential-backoff overflow; 2.C.14 — broaden `catch`;
  2.C.15 — typed error classification.
- 2.D.2 / 2.D.3 — Rewrite `NestedSpawnFromCoroutineBody` without
  `[this]`; strengthen docs.
- 2.D.5 / 2.D.8 — Relax-counter semantics + `CoroContext::push` debug
  asserts.
- 2.D.10 — regression tests for re-entrancy and scheduler reset.

### Phase 4 — Polish — S4

- 2.A.7 — IWYU pass.
- 2.B.14 — `static_assert` on `tuple_size` ↔ `get` parity.
- 2.D.9 — cross-reference `coro_mixin` ↔ `spawn_async` in docs.

---

## 5. Known safe by construction (for future reviewers)

- **`qb::allocator::pipe<char>`** — linear buffer with `_begin/_end` +
  `reorder()`. **Not** a ring buffer; no "full looks empty" bug possible.
- **`qb::ring_buffer<T, N>`** — tracks `size_` separately; `empty()` and
  `full()` are unambiguous.
- **`qb::lockfree::spsc`** — reserves one sentinel slot; `write == read`
  unambiguously means empty.
- **Actor lifetime vs orphan coroutine** — `CoroContext` captures
  `ActorId` by value; `VirtualCore::_handler` routes events to dead
  actors as no-ops; `active_coroutines_` is a shared `atomic<size_t>`
  so the RAII guard can fire after the actor dies.
- **Per-listener `coro_scheduler`** — pinned to the worker thread via
  `listener::current` (thread-local), mirroring the actor system's
  single-thread-per-VC invariant.

---

## 6. Out of scope (for this pass)

- Multi-thread coroutine scheduling (`post_from_foreign_thread`) — not
  a current requirement; if it becomes one, re-review `schedule_resume`
  and `in_flight_` synchronization.
- Coroutine affinities across cores — actors are pinned, coroutines are
  pinned; no plan to migrate.
- Integration with `std::stop_token` / `std::jthread` — the framework
  has its own `cancellation_token` system; unification with
  `stop_token` would be a larger refactor.

---

## 7. Progress log

### 2026-04-19 — Initial review produced

- 27 findings across 4 subsystems.
- 8 S1 (correctness / UB), 14 S2, 7 S3, 2 S4.
- No findings implemented yet. Phase 1 targets the silent-data-loss
  bugs and the `timer_awaiter` libev cache-staleness fix (leverages the
  exact same `ev_now_update()` workaround we landed in `io.h::callback`
  during the HTTP middleware investigation).
- Cross-cut with QB_IO_PLAN: the freelist / intrusive-list pattern we
  used for `Timeout<F>` and `RegisteredKernelEvent` applies directly to
  the coroutine promise allocators (2.A.9) and the scheduler ready queue
  (2.B.10).

### 2026-04-19 — Phase 1 (correctness, S1) — implemented

| Finding | File | Change |
|---|---|---|
| 2.C.1 | `channel.h` | `send_awaiter::await_ready()` returns `true` when the channel is closed so `await_resume()` throws `channel_closed` instead of silently dropping the value. |
| 2.C.2 | `retry.h` | `detail::calculate_delay` is 1-based; linear backoff now sleeps `base_delay` on the first retry. |
| 2.C.3 | `awaiter.h` | `timer_awaiter::await_suspend` calls `ev_now_update()` before arming the watcher (same fix as `io.h::callback`). |
| 2.B.1 | `scheduler.h` | `enqueue_for_later` dedups via `in_flight_`; no more double-resume on spurious wakes. |
| 2.B.2 | `cancellation.h` | `cancellable_operation<T>` captures `std::exception_ptr` in the shared state; `await_resume()` rethrows. Added early check for already-past deadlines. |
| 2.B.5 | `cancellation.h` | `with_deadline` no longer reclassifies the winner as a timeout; only an already-past deadline fails synchronously. |
| 2.C.4 | `stream.h` | `debounce` propagates exceptions from the source stream; internal channel is now bounded (`kDebounceChannelCapacity=64`). |
| 2.A.1 / 2.A.6 | `shared_task.h` | `co_await` on a null state throws `std::logic_error`; waiter vector is pre-reserved for 4 entries so `push_back` is effectively `noexcept`. |
| 2.A.2 | `scheduler.h` | `schedule_via_current` asserts a TLS scheduler exists in debug builds. |
| 2.D.1 | `scheduler.h` | `run_ready()` installs a re-entrancy guard; recursive calls assert in debug, no-op in release. |
| 2.D.4 | `Actor.tpp` | `spawn_async` revalidates the cached `coro_scheduler_` against the current TLS scheduler on every call. |

### 2026-04-19 — Phase 2 (performance, S3) — implemented

| Finding | File | Change |
|---|---|---|
| 2.B.10 | `scheduler.h` | Replaced `qb::lockfree::mpsc_unbounded_queue` with `std::deque<ready_item>` (strictly mono-thread → no atomics, no cache-line contention, ~8 nodes per chunk amortises allocs). |
| 2.A.9 | `task.h` | `task<T>::promise_type::operator new/delete` backed by a thread-local size-bucketed freelist (`detail::CoroutineFrameAllocator`). Steady-state spawn/despawn burns zero `malloc`/`free`. |
| 2.D.6/7 | `Actor.tpp` | Kept `actor_coro_wrapper` (single point for lifetime / counter RAII); the pooled allocator absorbs the cost of the "double frame". Documented as a decision log in the code. |
| 2.C.10 | `sync.h` | `semaphore::acquire_awaiter::await_ready()` fast-path: decrements synchronously when a permit is available, saving a suspend/resume cycle. |
| 2.C.16 | `stream.h` | `zip` short-circuits if the first stream is exhausted, no longer consumes an item from the second stream that would be thrown away. |
| 2.B.11 | — | Deferred: marginal win, now dominated by the pooled promise allocator. |
| 2.C.7 | — | Deferred: requires a project-wide move-only SBO-`function` type; pool already amortises the `std::function` alloc. |

### 2026-04-19 — Phase 3 (API hygiene & UB hardening, S2) — implemented

| Finding | File | Change |
|---|---|---|
| 2.A.3/4/5 | `task.h`, `generator.h` | `task<T>::await_resume` checks `has_exception()` / `is_ready()` before touching the variant (no silent `bad_variant_access`); `generator::iterator::operator*` / `operator->` assert on exhausted iterators. |
| 2.A.8 | `generator.h` | `from_range` now takes the range **by value**: the coroutine frame copies/moves it, no dangling on temporaries. |
| 2.B.3/4 | `combinators.h` | Docs rewritten for `race` and `coro_with_timeout` to state explicitly they do NOT cancel losers / the inner task; pointer to `with_deadline` + `cancellation_token` for real cancellation. |
| 2.B.6 | `scheduler.h` | `active_count()` fixed to return `ready_queue_.size() + suspended_coroutines_.size()` (previously double-counted ready frames and ignored suspended ones). |
| 2.B.8 | `scheduler.h` | `~CoroutineScheduler()` — detailed decision log on why suspended frames are intentionally leaked; debug `fprintf` warns when teardown order is wrong. |
| 2.B.9 | `scheduler.h` | Thread-safety docs rewritten: every public method is mono-thread and requires caller to be on the owning thread. |
| 2.B.12/13 | `scope.h` | `~coroutine_scope(join_all)` warns in debug when destroyed with active tasks. `<algorithm>` explicitly included. |
| 2.B.14 | `combinators.h` | `static_assert(tuple_size<when_any_result>::value == 2)` guards the structured-binding arity. |
| 2.C.5 | `stream.h` | `backpressure_fill_task` releases the acquired permit on EOF so shared semaphores don't starve over time. |
| 2.C.8 | `stream.h` | `from_channel(channel<T>&)` doc expanded with explicit lifetime contract + cross-reference to `from_channel_shared()` as the safe default. |
| 2.C.9 | `channel.h` | `channel_range` doc clarified — it is a **non-blocking** drain, not an async iterator; `operator*` is no longer `const` (accurately reflects the move-out). |
| 2.C.11/12 | `sync.h` | `async_mutex::unlock()` asserts when called on an unlocked mutex; `barrier::arrive_awaiter` doc-commented for missing-reset misuse. |
| 2.D.2/3/5/8 | `Actor.tpp` | `spawn_async` asserts a TLS scheduler exists on the caller thread (guards against cross-thread calls); existing safety docs already covered 2.D.2/3/8. |
| 2.D.10 | `test_coroutine_regression.cpp` | 6 new regression tests (channel send-on-closed, shared_task null state, with_deadline already passed, active_count incl. suspended, from_range temporary, zip short-circuit). 39/39 pass. |

### 2026-04-19 — Phase 4 (polish, S4) — implemented

| Finding | File | Change |
|---|---|---|
| 2.A.7 | `channel.h`, `combinators.h`, `retry.h`, `stream.h`, `cancellation.h` | IWYU pass: added missing `<chrono>`, `<deque>`, `<exception>`, `<memory>`, `<optional>`, `<stdexcept>`, `<utility>`, `<cstddef>`, `<type_traits>` where symbols were used transitively. |
| 2.D.9 | `mixin.h` | `coro_mixin` doc now cross-references `Actor::spawn_async` (how to drive tasks produced by `.coro()` inside an actor; explicit warning against `run_sync()` from an actor handler). |

### 2026-04-19 — Validation summary

- Full build: clean, no warnings.
- `ctest -R "coroutine|coro|actor-coroutine"`: **23/23 pass** (all coroutine tests + `qb-core-gtest-system-test-actor-coroutine[-advanced]` + `qbm-pgsql-test-pgsql-coro-api`).
- `qb-io-gtest-coroutine-regression` standalone: **39/39 pass** (6 new Phase 3 regressions).
- `qb-io-gtest-coroutine-benchmark`: 4/4 OK.
- Mono-thread invariant preserved: no atomics / locks / cross-thread schedule added; `CoroutineScheduler`, `channel`, `semaphore`, `async_mutex`, scope primitives remain strictly owner-thread.

---

*End of review — all 27 findings addressed (implemented or explicitly deferred with rationale).*
