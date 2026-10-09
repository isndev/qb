# C++20 coroutines

> **Audience:** Adopter · **Status:** stable · **Verified-against:** qb 3.2.1 (C++20 default, C++23 supported) — 90fa721f

`qb::io::async` ships a native C++20/23 coroutine layer — `task<T>`, awaiters, combinators, channels, structured-concurrency scopes, generators, streams, retry, and cancellation — running directly on the same single-threaded libev loop as the rest of `qb-io`, so asynchronous code reads as straight-line sequential code.

**Prerequisites:** [The async runtime](./async_system.md), [qb-io overview](./README.md) — **See also:** [What has no coroutine form](./gaps.md) · [Transports](./transports.md) · [Protocols](./protocols.md) · [Async, lifecycle, and allocation invariants](../7_reference/io_invariants.md)

## Summary

A coroutine in `qb-io` is any function that returns `qb::io::async::task<T>` and uses `co_await`, `co_return`, or (for generators) `co_yield`. Tasks suspend at `co_await` and hand control back to the thread's event loop; the loop resumes them when the awaited timer, socket, channel, lock, or inner task completes. Everything runs on one thread per scheduler, cooperatively — two coroutines on the same thread are never concurrent, which is what makes the synchronization primitives lock-free and the actor-integration rules tractable.

Include the whole layer with one header:

```cpp
#include <qb/io/async/coroutine.h>   // task, awaiters, scheduler, combinators, …
```

`<qb/io/async.h>` pulls it in transitively, so any program that already uses the async runtime has the coroutine API available. The TCP connect awaiter lives in `<qb/io/async/tcp/connector.h>` (guarded by `__cpp_impl_coroutine`) and is reached through `<qb/io/async.h>`.
<!-- src: qb/src/qb/io/async/coroutine.h, qb/src/qb/io/async.h:53 -->

The framework targets C++20 by default; coroutine support requires a compiler with working C++20 coroutines.
<!-- src: qb/README.md (C++20 requirement); connector.h gated on __cpp_impl_coroutine -->

Every timed coroutine API on this page takes a `qb::duration` (a `std::chrono::nanoseconds` span; any `std::chrono::duration` converts implicitly). Deadlines that need an absolute point use `std::chrono::steady_clock::time_point` (the type behind `qb::mono_time`). Raw `double`-seconds arguments are not part of this surface.
<!-- src: qb/src/qb/io/async/coroutine/awaiter.h:330, cancellation.h:1119 -->

## The execution model

```mermaid
flowchart TB
    L["qb::io::async::listener<br/>one per thread · owns the libev loop"]
    L --> Sched["CoroutineScheduler<br/>one per listener · thread-local"]
    Sched --> A["spawn(task&lt;void&gt;&&)<br/>detached, fire-and-forget"]
    Sched --> B["spawn(Callable)<br/>closure-owning overload"]
    Sched --> C["schedule_resume(handle)<br/>wake a continuation after co_await"]
    Sched --> D["run_ready()<br/>drain the ready queue each loop tick"]
```

| Property | Value | Source |
|---|---|---|
| Schedulers per thread | one (`thread_local`, owned by the listener) | `scheduler.h:158-162`, `utils.h:212` |
| Concurrency model | cooperative, single-threaded | `scheduler.h:158-162` |
| Interleaving point | `co_await` only | `scheduler.h:754-765` (Factbook) |
| OS mutexes / atomics on the hot path | none, within one thread | `scheduler.h:45-54`, `sync.h:33-36` |
| Cross-thread wake-up | route through the `qb-core` actor mailbox; a call made on another thread: `offload` | `scheduler.h:163-166`; `offload.h:49-59` |

Because all coroutines on a thread share one scheduler and one event loop, only one runs at a time and another can start only at a suspension point. Mutual exclusion between two coroutines on the same thread is therefore a property of the model, not something you lock for. Pushing or resuming a coroutine from a *different* thread is undefined behavior — the scheduler holds no mutex; cross-thread signaling must go through the actor mailbox (see [Safe integration with `qb::Actor`](#safe-integration-with-qbactor)). The one crossing the layer makes for you is [`offload`](#offloading-blocking-work): the call runs on a pool thread, and the coroutine is resumed back on its own.
<!-- src: qb/src/qb/io/async/coroutine/scheduler.h:156-166 -->

## Quick start (standalone)

Outside `qb-core`, drive the loop yourself: initialize the thread's listener, then hand the root task to `run_sync`, which pumps the loop until that task completes and returns its value.

```cpp
// src: derived from examples/03-coroutines/01-first-coroutine.cpp
#include <qb/io/async/coroutine.h>
#include <chrono>
#include <iostream>

using namespace qb::io::async;
using namespace std::chrono_literals;

task<int> compute_value(int base) {
    co_await sleep(100ms);            // suspends; the loop runs other work
    co_return base * 2;
}

task<void> use_computed_value() {
    int result = co_await compute_value(21);
    std::cout << "Result: " << result << '\n';   // prints "Result: 42"
}

int main() {
    qb::io::async::init();                        // no-op, kept for symmetry (see below)
    run_sync(use_computed_value());               // pump the loop until the task is done
    return 0;
}
```

`init()` is a **no-op** kept for symmetry — its whole body is a comment. `listener::current` is a `thread_local` that initializes itself on first access, so nothing needs readying; and `init()` deliberately does *not* clear existing state, because fixtures that share a thread's listener would have their already-registered watchers invalidated. For a genuinely clean loop, call `listener::current.clear()` (see [The async runtime](./async_system.md#one-loop-one-thread-no-lock)). `coro_scheduler()` returns the listener's scheduler so `spawn`, timers, and `run_ready()` all share one loop; `run_sync` spawns onto it for you, which is why the example above never names it.

Prefer `run_sync(awaitable)` over `run_for(duration)` for a root task. `run_sync` returns when the work is done — it pumps the loop until the awaitable completes, then yields its value or rethrows its exception. `run_for` returns when the *duration* is up, so it burns its whole budget even when the work finished early, and the duration is a correctness guess in the other direction too: pick it too small on a loaded machine and the coroutine is abandoned mid-flight, with no diagnostic and exit code 0. Reach for `run_for` only when pumping the loop for a fixed span is genuinely what you mean. Under `qb-core`, each `VirtualCore` owns its listener and pumps the loop for you — you call neither from inside an actor (see [Safe integration with `qb::Actor`](#safe-integration-with-qbactor)).
<!-- src: qb/src/qb/io/async/listener.h:1409 (init), qb/src/qb/io/async/coroutine/utils.h:212 (coro_scheduler), :227 (run_for), :285 (run_sync), examples/03-coroutines/01-first-coroutine.cpp:144-175 -->

## `task<T>` — the coroutine return type

`task<T>` (`coroutine/task.h`) is the primary return type. `T` is the value produced by `co_return`; use `task<void>` when there is none.

```cpp
// src: derived from qb/src/qb/io/async/coroutine/task.h
#include <qb/io/async/coroutine.h>
using namespace qb::io::async;
using namespace std::chrono_literals;

task<int> async_add(int a, int b) {
    co_await sleep(0ms);             // yield once, then resume on the same thread
    co_return a + b;
}

task<void> caller() {
    int v = co_await async_add(3, 4);   // suspends caller until async_add finishes
    // v == 7
}
```

| Property | Detail | Source |
|---|---|---|
| Return types | `task<void>` or `task<T>` for any move-constructible `T` | `task.h:436`, `:817` |
| Initial suspend | `std::suspend_always` — lazy until spawned or awaited | `task.h:508-509` |
| Move-only | yes; a moved-from task is empty and destroys nothing | `task.h:675-676`, `:697-698` |
| Exception propagation | stored in the promise, re-thrown at the awaiting `co_await` | `task.h:572`, `:756-757` |
| Symmetric transfer | `await_suspend` returns a `coroutine_handle<>` — flat stack in deep chains | `task.h:724-725` |
| Frame allocation | thread-local size-bucketed freelist (`detail::CoroutineFrameAllocator`) | `task.h:201` |

`await_resume()` always checks for a stored exception first and re-throws it; if the task is somehow not ready it throws `std::logic_error` rather than returning an uninitialized value. You generally never see these paths — you `co_await` the task and the result (or exception) is delivered.
<!-- src: qb/src/qb/io/async/coroutine/task.h:748-766 -->

> `task<T>` is move-only. Pass it to `spawn` (or any consumer) with `std::move`. `coro_scheduler().spawn(t)` is a compile error; write `coro_scheduler().spawn(std::move(t))`. See the [`spawn(Callable)` overload](#the-scheduler) for the case where you want to hand a lambda directly.
<!-- src: qb/src/qb/io/async/coroutine/task.h:697-698; scheduler.h:458 (Factbook) -->

### `shared_task<T>` — one computation, many awaiters

`shared_task<T>` (`coroutine/shared_task.h`) is a copyable handle to a single coroutine result. The first `co_await` runs the computation; later awaits — from any number of coroutines — observe the same result without re-running it.

```cpp
// src: derived from qb/src/qb/io/async/coroutine/shared_task.h
#include <qb/io/async/coroutine.h>
using namespace qb::io::async;
using namespace std::chrono_literals;

task<int> compute_value() { co_await sleep(50ms); co_return 42; }

task<void> fan_out() {
    shared_task<int> shared = make_shared_task(compute_value());
    int a = co_await shared;        // triggers execution
    int b = co_await shared;        // reuses the cached result, no extra work
    // a == b == 42
}
```

Awaiting a default-constructed `shared_task` throws `std::logic_error` — construct it through `make_shared_task`.
<!-- src: qb/src/qb/io/async/coroutine/shared_task.h:188, :372 -->

## The scheduler

The per-thread scheduler is reached through `coro_scheduler()` (`coroutine/utils.h`), which returns the listener's `CoroutineScheduler` (`coroutine/scheduler.h`).

```cpp
// Detached, fire-and-forget — the scheduler owns the frame to completion.
coro_scheduler().spawn(std::move(my_task));

// Closure-owning overload: pass the lambda itself (no trailing ()),
// so the closure is moved into an owning frame and cannot dangle.
coro_scheduler().spawn([captured]() -> task<void> {
    co_await do_work(captured);
});

// Introspection
std::size_t live    = coro_scheduler().active_count();   // ready + suspended
std::size_t pending = coro_scheduler().pending_count();  // ready queue only
bool        ready   = coro_scheduler().has_ready();
```
<!-- src: qb/src/qb/io/async/coroutine/scheduler.h:458 (spawn task), :601 (spawn Callable), :896 (active_count), :836 (pending_count), :802 (has_ready); utils.h:212 (coro_scheduler) -->

`spawn(task<void>&&)` takes ownership of the handle: the coroutine runs to completion even after the original `task` object is destroyed, and the scheduler frees the frame when it finishes. `spawn(Callable)` accepts a no-argument callable returning `task<void>` and moves the closure into an owning wrapper frame — the fix for the "dangling lambda" trap described in [Lifetime footguns](#lifetime-footguns). `schedule_resume()` does *not* take ownership; it is how awaiters wake a continuation whose frame belongs to a `task<T>` object elsewhere.
<!-- src: qb/src/qb/io/async/coroutine/scheduler.h:436-458 (spawn task), :573-604 (spawn Callable), :668 (schedule_resume, Factbook) -->

`active_count()` returns ready-queue frames plus suspended frames — the count of coroutines still in flight, which is what a drain or shutdown loop needs. Note what it does *not* count: a spawned coroutine parked on an inner `task` is tracked only in `owned_frames_`, because only the innermost I/O or timer awaiter registers as suspended (`src/qb/io/async/coroutine/scheduler.h:941-952`).

### Never pump the loop from inside a coroutine

Calling `run`, `run_once`, `run_until`, `run_for` or `run_sync` from **inside a coroutine body** throws `std::logic_error` (and asserts in debug). A coroutine body is resumed *by* `CoroutineScheduler::run_ready()`, so the `in_run_ready_` flag is set, and `ensure_not_inside_ready_drain()` sees it (`src/qb/io/async/coroutine/scheduler.h:827`; `src/qb/io/async/listener.h:1424`). A second, deeper guard inside `run_ready()` itself makes a nested drain a no-op: it resumes nothing and returns `0`, leaving the coroutines to the enclosing drain — which is what lets a library drain such as `Redis::await()` run `listener::current.run()` from a coroutine body. Until 3.3 that guard asserted in debug (Huly QB-253; `src/qb/io/async/coroutine/scheduler.h:738-740`).

**That guard does not fire in an actor event handler**, which is where the mistake is actually made — an actor handler runs *after* `listener::run()` has returned, so nothing is draining. The consequence is a silently frozen `VirtualCore`, and [the async runtime page owns the full rule](./async_system.md#run_sync-and-run_for-block-the-calling-thread). Inside an actor, `Actor::spawn` and `co_await` are the only correct spelling.
<!-- src: qb/src/qb/io/async/coroutine/scheduler.h:896 (active_count), :738-740 (re-entrancy guard), :827 (is_draining_ready); qb/src/qb/io/async/listener.h:1424 (ensure_not_inside_ready_drain) -->

## Awaiters

Awaiters bridge coroutines to libev events. The free functions in `coroutine/utils.h` cover the common cases; `coroutine/awaiter.h` defines the underlying types (`timer_awaiter`, `socket_awaiter`, `async_awaiter<T>`).

```cpp
// src: derived from qb/src/qb/io/async/coroutine/utils.h, awaiter.h
#include <qb/io/async/coroutine.h>
using namespace qb::io::async;
using namespace std::chrono_literals;

co_await sleep(500ms);              // suspend for a duration (qb::duration)

co_await wait_readable(fd);         // resume when fd is EV_READ ready
co_await wait_writable(fd);         // resume when fd is EV_WRITE ready
co_await wait_for_io(fd, EV_READ | EV_WRITE);
```

Note what `wait_readable` takes: a **raw descriptor**. It is the coroutine layer's only entry into the network stack, and it is one level below sessions and protocols — see [what has no coroutine form](./gaps.md) for the list of things that therefore have no awaiter.

### Bridging a callback API

`async_awaiter<T>` is the generic adapter, and it is the shape every callback-based library gets wrapped in — including the three qbm modules' own hand-rolled awaiters.

```cpp
// src: derived from qb/src/qb/io/async/coroutine/awaiter.h:608-618
// Bridge a callback-style API into a coroutine result.
int result = co_await async_awaiter<int>([](auto cb) {
    legacy_async_op([cb](int r) { cb(r); });   // cb fires from an event handler
});
```

The callback must fire **exactly once**: `await_resume` asserts on a resume with no result, because the only path that schedules the frame is the callback itself, which engages the result before waking it (`awaiter.h:693-697`). The awaiter holds a `shared_ptr<bool>` liveness flag that its destructor clears, so a callback that fires after the coroutine frame is gone is a safe no-op rather than a use-after-free (`awaiter.h:676-684`, `:701-706`).

What it does **not** give you is cancellation-awareness. A coroutine parked in an `async_awaiter` registers no `on_cancel` hook, so `cancel()` neither wakes it nor unwinds it; it stays parked until the operation completes naturally. To make one interruptible inside an actor, wrap it — `ctx.cancellable(...)`, `with_deadline(...)`, or a `when_any` against `check_cancelled(tok)`.

`sleep(qb::duration)` with a duration of zero or less is a **cooperative yield**, not a kernel timer: the coroutine is re-enqueued at the back of the ready queue and resumes on the next scheduler turn. A positive duration arms an `ev_timer`. There is no `sleep_until` in this layer; for an absolute deadline use [`with_deadline`](#cancellation).
<!-- src: qb/src/qb/io/async/coroutine/awaiter.h:305-314 (yield_only_ rationale), :332 (duration <= 0), :356-359 (re-enqueue, no timer), utils.h:101 (sleep); no sleep_until exists -->

> Awaiters must remain alive until `await_resume()`. Never create a temporary awaiter that goes out of scope before the coroutine resumes. The framework awaiters stop their libev watcher in `await_resume()` and in their destructor, so an early return or thrown exception cannot leave a live watcher pointing at a freed frame.
<!-- src: qb/src/qb/io/async/coroutine/awaiter.h:30-35, :381-395 (await_resume stops the watcher), :405-428 (destructor) -->

### Awaiting a TCP connect

The coroutine connect factory (`tcp/connector.h`) returns an awaiter that yields `std::optional<Socket_>` — empty on timeout or error.

```cpp
// src: derived from qb/src/qb/io/async/tcp/connector.h
#include <qb/io/async.h>            // pulls in tcp/connector.h
using namespace qb::io::async;
using namespace std::chrono_literals;

task<void> connect_to(qb::io::uri remote) {
    auto sock = co_await tcp::connect(remote, 5s);   // std::optional<...>
    if (!sock) {
        // timed out or failed to connect
        co_return;
    }
    // use *sock
}
```

`connect<Transport>(uri remote, qb::duration timeout = qb::duration::zero(), bool verify_peer = true)` defaults to `transport::tcp`. A zero timeout means no deadline.
<!-- src: qb/src/qb/io/async/tcp/connector.h:996-1000 (connect factory), :975-976 (await_resume std::optional<Socket_>) -->

**Every address, in order (3.3).** A hostname URI resolves to every address of its family -- IPv4 unless the URI says otherwise -- and the connector tries them in the resolver's order. One that refuses, fails or does not answer within its share of the deadline is closed and the next is tried; the deadline stays the whole connect's, and a TLS failure on an address that answered is final: the next would present the same server (`src/qb/io/async/tcp/connector.h:136-142`). The share is `tcp::connect_attempt_budget`: the time left over the addresses left, never under two seconds unless less is left, so a dead first address in a DNS round-robin no longer eats the deadline (`src/qb/io/tcp/socket.cpp:88-95`). `connect<Transport>(std::vector<qb::io::endpoint> endpoints, std::string host, timeout, verify_peer)` runs the same over addresses you resolved yourself -- on the [offload pool](#offloading-blocking-work), from a cache -- with `host` the name TLS verifies (`src/qb/io/async/tcp/connector.h:1021`).

## Offloading blocking work

A coroutine suspends only at `co_await`. A call that **blocks** — a read of a cold file, `getaddrinfo`, a key derivation sized for a login endpoint, the compression of a large payload — suspends nothing: it holds the loop thread, and every other coroutine, timer and socket of that thread waits with it ([what has no coroutine form](./gaps.md) names the usual ones). Since 3.3, `co_await offload(fn, args...)` moves such a call to a small pool and suspends the coroutine until it has returned:

```cpp
// src: derived from examples/03-coroutines/15-offloading-blocking-work.cpp
#include <qb/io/async.h>
#include <thread>
using namespace qb::io::async;
using namespace std::chrono_literals;

int blocking_call(int x) {                        // stands for any call that blocks its thread
    std::this_thread::sleep_for(300ms);
    return x * 2;
}

task<int> handler() {
    int r = co_await offload(blocking_call, 21);  // a pool thread blocks; this loop keeps turning
    co_return r;                                  // resumed on this loop, with 42
}
```

The example measures it beside a 5 ms heartbeat: called inline, the 300 ms call is the heartbeat's worst gap; offloaded, the worst gap stays one tick.

The contract of `offload` is four rules (`qb/src/qb/io/async/coroutine/offload.h:24-40`):

1. **Values in, values out.** The callable and its arguments are copied at the call, like `std::thread`'s; the pool invokes `std::invoke(std::move(fn), std::move(args)...)` and destroys them there, right after the call (`run()`, `qb/src/qb/io/async/coroutine/offload.h:202-213`). The result is a value — a callable returning a reference does not compile — handed back by `co_await`.
2. **The callable runs on another thread.** It must not touch an actor, a qb-io object, a coroutine or anything the awaiting loop owns. Inside it, `listener::current` is the *pool* thread's listener, not the awaiting loop's.
3. **The coroutine resumes on the thread that awaited**, in a turn of its loop, never on a pool thread. An exception the callable throws is rethrown by `co_await`.
4. **A running call cannot be interrupted.** If the waiting frame is destroyed while it runs — a `when_any` loser, a cancelled scope — the call goes to its end, its result is destroyed on the awaiting loop, `offload_stats::discarded` counts it, and nothing is resumed (`~offload_awaiter`, `qb/src/qb/io/async/coroutine/offload.h:274-280`).

The pool is one per process and starts with the first `offload` — no thread exists before it. It runs two threads unless `set_offload_threads(n)` said otherwise first (the call answers `false` once the pool has started), serves an unbounded FIFO queue, and is stopped at process exit: a queued call is dropped, a running one finishes, so an offload still running then delays the exit. `current_offload_stats()` reads its counters — `threads`, `submitted`, `completed`, `discarded`, `queued` — without starting it (`qb/src/qb/io/async/coroutine/offload.h:107-130`).

How the resume crosses threads, and what that costs a loop that never offloads: each thread that awaits an offload gets a completion port, its own `ev_async` watcher on that thread's loop, started while an offload of the thread is in flight and stopped once none is (`qb/src/qb/io/async/offload.cpp:47-49`). A pool thread hands the finished call back in `complete()`, under the port's mutex, so it never sends to a loop that is being destroyed (`qb/src/qb/io/async/offload.cpp:87-106`); the watcher's callback, `on_complete`, schedules the coroutine on the loop's own scheduler (`qb/src/qb/io/async/offload.cpp:289-308`). While started, the watcher is a referenced active watcher: the loop counts as busy (`has_work()`), a `VirtualCore` keeps pumping it and parks *inside* it, where the pool's send ends the park, and a blocking `async::run()` returns only once the offload has completed — as it would for a pending timer. A thread that never calls `offload` has no port, no watcher and no pipe, and no existing path of the loop changed to make room for one.

Inside an actor, write `ctx.offload(fn, args...)` (`ScopedCoroContext::offload`, `qb/src/qb/core/Actor.h:2301`): the same call, scoped to the actor, so a kill wakes the wait at once with `cancelled_error` rather than leaving the coroutine parked until the call returns — see [Safe integration with `qb::Actor`](#safe-integration-with-qbactor).
<!-- src: qb/src/qb/io/async/coroutine/offload.h:302 (offload), :124 (set_offload_threads), :130 (current_offload_stats); qb/src/qb/io/async/offload.cpp:73-76 (begin), :87-106 (complete), :289-308 (on_complete) -->

## Combinators

`coroutine/combinators.h` composes several tasks into one awaitable.

### `when_all` — wait for every task

```cpp
// Heterogeneous: returns std::tuple<A, B>
auto [a, b] = co_await when_all(fetch_int(), fetch_string());

// Homogeneous vector: returns std::vector<T>
std::vector<task<int>> work;
for (int i = 0; i < 8; ++i) work.push_back(compute(i));
std::vector<int> results = co_await when_all(std::move(work));
```
<!-- src: qb/src/qb/io/async/coroutine/combinators.h:225 (variadic), :338 (vector) -->

### `when_any` / `race` — first to finish wins

```cpp
// when_any returns when_any_result { size_t index; std::any value; std::exception_ptr exception; }
auto r = co_await when_any(fast(), slow(), backup());
// Structured bindings expose (index, value); the value is std::any:
auto [idx, value] = r;
int v = std::any_cast<int>(value);

// race(...) is a semantic alias for when_any(...).
co_await race(network_task(), local_cache_task());
```

`when_any_result::get<T>()` casts the winning value (and re-throws if the winner threw). On win, the losing branches are **reclaimed (cancelled)** — the scheduler tears down each loser, stopping its timers and dropping its result — so nothing lingers in the background.
<!-- src: qb/src/qb/io/async/coroutine/combinators.h:350 (when_any_result), :590 (when_any), :1094 (race) -->

### `coro_with_timeout` — deadline wrapper

```cpp
try {
    auto value = co_await coro_with_timeout(fetch_data(), 2s);   // returns T
    use(value);
} catch (const qb::io::async::timeout_error&) {
    // deadline elapsed before fetch_data() completed
}
```

`coro_with_timeout(task<T>&&, qb::duration)` returns `T` and **throws `timeout_error`** on timeout — it does not return an `std::optional`. On timeout the inner task keeps running in the background until it finishes naturally; its result is then dropped.

The awaiter arms a **raw self-stopping `ev_timer`** rather than spawning a `co_await sleep()` helper, deliberately: a spawned helper would leave one parked frame plus one armed watcher per in-flight call for the full timeout duration — the `ev_timer` lives in the awaiter's shared state instead (`combinators.h:735-767`). It is non-copyable and non-movable for the same reason — the watcher's `data` pointer refers to the state it owns.

Note the asymmetry between the timeout path and the teardown path, because it is easy to read as a contradiction. A **timeout** resolves the race in `resolve_timeout()` and leaves the inner task alone (`combinators.h:781-786`). A **destroyed awaiter** — this call was itself a `when_any` loser, or its scope was cancelled — tears the inner task down: it destroys the spawned runner, `forget`s the inner frame and drops it (`combinators.h:848-865`). For a genuinely interruptible deadline, use `with_deadline(op, tp, token)` instead.
<!-- src: qb/src/qb/io/async/coroutine/combinators.h:921 (coro_with_timeout returns T), :894 (throws timeout_error), :853-855 (inner task is not interrupted) -->

## Cancellation

`coroutine/cancellation.h` provides a cooperative `cancellation_token` and helpers that observe it.

```cpp
// src: derived from qb/src/qb/io/async/coroutine/cancellation.h
#include <qb/io/async/coroutine.h>
using namespace qb::io::async;
using namespace std::chrono_literals;

cancellation_token token;

// Sleep that wakes immediately with cancelled_error when the token fires.
task<void> worker(cancellation_token tok) {
    co_await cancellable_sleep(500ms, tok);
    // reached only if not cancelled before the timeout
}

// Run an operation against an absolute deadline.
task<int> bounded(task<int> op, cancellation_token tok) {
    auto deadline = std::chrono::steady_clock::now() + 200ms;
    co_return co_await with_deadline(std::move(op), deadline, tok);
}

token.on_cancel([] { release_resource(); });   // cleanup callback
token.cancel();                                 // same thread only
```

`cancellation_token` is copyable (copies share one intrusively refcounted `state`; the count is NOT atomic, because every copy lives on the owning thread — a copy is one register increment, which is what lets `qb::ask` take its context by value) and holds no mutex: `cancel()`, `on_cancel()` and `link()` must run on the token's own thread. `with_deadline(task<T>&& operation, std::chrono::steady_clock::time_point deadline, cancellation_token token = {})` takes ownership of the moved operation at the call, so the returned task can be stored or passed on before it is awaited. It checks the absolute deadline on resume, then throws `timeout_error` if already past or `cancelled_error` on cancellation; a winning operation result is authoritative and is never reclassified against wall-clock time. A wrapper coroutine of your own must also take a task operand by value, as `bounded` does above. `check_cancelled(token)` and `yield_or_cancel(token)` throw `cancelled_error` when the token is set; `make_cancellable(task, token)` wraps a task so it surfaces cancellation.
<!-- src: qb/src/qb/io/async/coroutine/cancellation.h:251 (cancel), :300 (on_cancel), :1119-1120 (with_deadline ownership), :1061 (owning frame), :1070-1072 (deadline already past), :463 (check_cancelled), :503 (yield_or_cancel), :808 (make_cancellable), :931 (cancellable_sleep) -->

> **Cross-thread cancellation.** A token has no lock. To cancel from another thread, send a `qb-core` actor event to the owning thread and call `token.cancel()` from that actor's synchronous handler, where it runs on the right thread.
<!-- src: qb/src/qb/io/async/coroutine/cancellation.h:142-143 -->

## Every awaitable, and what cancellation does to it

This is the table to read before you rely on cancellation for anything. **`cancel()` does not stop a coroutine.** It sets a flag and runs the callbacks registered against that token — and only five awaitables in the whole layer register one. A coroutine parked on anything else is listening to nothing: it stays parked until its own operation completes naturally, and then resumes into a world that may have moved on.

The distinction the vocabulary draws, and which the rest of this section depends on: **cancellation-aware** means the awaiter registers on the token — an `on_cancel` callback, or an embedded `cancel_hook` through `link()` (zero allocation, O(1) unlink; what `qb::ask` uses) — so `cancel()` wakes it. **Cancellable** means you can *wrap* it so that something else wakes on your behalf — which is what `make_cancellable`, `with_deadline` and `when_any` are for. Everything can be made cancellable; almost nothing is cancellation-aware.

### Cancellation-aware — `cancel()` wakes these

| Awaitable | Parks on | Hook | On cancel |
|---|---|---|---|
| `co_await cancellable_sleep(d, tok)` | a spawned timer task (`sleep(d)` inside it) | `on_cancel` — `cancellation.h:885` | wakes now, tears the spawned timer down through `cancel_spawned`, and `await_resume` throws `cancelled_error` |
| `co_await make_cancellable(std::move(t), tok, throw_on_cancel)` | a spawned `task_runner` driving the inner task | `on_cancel` — `cancellation.h:598` (`:739` for `void`) | destroys the runner frame **first**, then `forget`s and drops the inner task, then resumes the waiter; throws `cancelled_error` when `throw_on_cancel` |
| `co_await with_deadline(std::move(op), deadline, tok)` | `when_any(op, timeout_branch)`; the timeout branch owns the hook | `on_cancel` — `cancellation.h:996` | resolves the branch with `result == 1`, reclaims the deadline timer, and `with_deadline` throws `cancelled_error` |
| `co_await check_cancelled(tok)` | nothing but the token itself | `on_cancel` — `cancellation.h:441` | resumes and throws `cancelled_error` |
| `co_await sem.acquire(tok)` | the semaphore's `_waiters` deque | `on_cancel` — `sync.h:226` | marks the node cancelled, retracts it from the queue, resumes, and throws `cancelled_error` |

`yield_or_cancel(tok)` is a near miss worth naming: it re-enqueues the coroutine at the back of the ready queue and **checks** the token when it resumes — `yield_or_cancel` (`cancellation.h:474-505`), so it observes cancellation promptly in a loop — but it registers no hook, so it cannot be woken by `cancel()` from a longer sleep.

### Not cancellation-aware — `cancel()` does nothing to these

Everything else. Grouped by what they park on, because that determines what *does* eventually wake them.

| Awaitable | Parks on | Woken by |
|---|---|---|
| `co_await sleep(d)` | `timer_awaiter`, i.e. a `ev_timer` (`awaiter.h:292`); `d <= 0` is a bare re-enqueue with no timer at all | the timer |
| `co_await wait_readable(fd)` / `wait_writable(fd)` / `wait_for_io(fd, ev)` | `socket_awaiter`, i.e. a `ev_io` watcher (`awaiter.h:468`) | fd readiness |
| `co_await async_awaiter<T>(op)` | your callback (`awaiter.h:625`) | your callback |
| `co_await tcp::connect(uri, timeout)` | the callback connector (`async/tcp/connector.h:903`) | connect success, failure, or the connector's own deadline |
| `co_await offload(fn, args...)` | the thread's offload port: an `ev_async` the pool sends (`offload.h:49-59`) | the call returning on a pool thread — a running call is never interrupted |
| `co_await innerTask` | the inner coroutine, by **symmetric transfer** (`task.h:725`) | the inner coroutine finishing |
| `co_await sharedTask` | the shared state's waiter list (`shared_task.h:148`) | the one computation finishing |
| `co_await when_all(...)` / `when_any(...)` / `race(...)` | N spawned branch runners (`combinators.h:99`, `:438`) | the branches |
| `co_await coro_with_timeout(t, d)` | a spawned runner **and** a raw self-stopping `ev_timer` (`combinators.h:767`) | whichever comes first |
| `co_await ch.send(v)` / `ch.recv()` | the channel's own `_send_waiters` / `_recv_waiters` deque — `send_awaiter` (`channel.h:158`), `recv_awaiter` (`:247`) | a counterparty, or `close()` |
| `co_await ch.send_for(v, d)` / `ch.recv_for(d)` | the same deques plus a spawned `sleep` timer — `timed_recv_awaiter` (`channel.h:524`), `timed_send_awaiter` (`:633`) | a counterparty, `close()`, or the timer |
| `co_await select(a, b, ...)` | every channel's `_select_waiters`, through `channel_select_awaiter` (`channel.h:1091`) | the first channel with data or a close |
| `co_await sem.acquire()` (no token) / `mtx.lock()` / `rw.lock_read()` / `lock_write()` — only when the permit or lock is NOT free: a free one is taken without suspending | the primitive's own waiter deque — `acquire_awaiter` (`sync.h:102`), `lock_awaiter` (`:450`), `read_lock_awaiter` (`:685`), `write_lock_awaiter` (`:734`) | a `release()` / `unlock()`, which hands the permit or lock over (FIFO) before resuming the waiter |
| `co_await b.arrive_and_wait()` / `ev.wait()` / `latch.wait()` | the primitive's waiter list — `arrive_awaiter` (`sync.h:997`) and the two `wait_awaiter`s (`:1149`, `:1345`) | the final arrival / `set()` / the count reaching zero |
| `co_await gen.next()` (async generator) | the generator, by symmetric transfer (`generator.h:424`) | the generator's next `co_yield` |
| `co_await stream.collect()` and every other terminal | an ordinary `task` over the source | the source |
| `co_await with_retry(f, policy)` | `f()`, and `sleep(delay)` between attempts (`retry.h:273`, `:321`, `:379`) | `f()` or the backoff timer — **the backoff is a plain `sleep`, not `cancellable_sleep`** |

Two entries deserve a sentence of their own. **`coro_with_timeout` does not interrupt anything on timeout**: the timeout path sets the result and resumes the waiter, never touching the inner task, which runs to completion in the background and has its result dropped. And **`with_retry` parked in its backoff cannot be woken by a token** — `retry.h` does not include `cancellation.h` at all, so a 30-second `max_delay` is 30 seconds.

### The other way a parked coroutine ends: its frame is destroyed

Since almost nothing is cancellation-aware, the mechanism that actually reclaims a parked coroutine in this framework is **structural**: someone destroys the frame, and the awaiter's destructor cleans up on the way out. That is what `coroutine_scope`'s cancel policy and `when_any`'s loser reclaim do, and what `Actor::kill()` does through the context's cancellation-aware operations — `ctx.cancellable(t)` and `ctx.offload(...)` destroy the inner frame they wrapped. The kill destroys no frame by itself: a spawned coroutine parked on anything else is neither woken nor reclaimed by it (see [below](#safe-integration-with-qbactor)).

Every awaiter in the layer therefore carries a destructor that has to survive "destroyed while still parked", and they are worth knowing as a family because the pattern is the same each time:

- **Watcher-backed awaiters stop the watcher unconditionally, gated only on "was it armed", never on `ev_is_active`.** A one-shot `ev_timer` is auto-stopped by libev the instant it expires — *before* its callback runs — so between expiry and dispatch it is inactive yet still sitting in `pendings[]` with `w->data` pointing at the awaiter. An active-gated stop would skip it and leave a freed watcher queued for invocation (`awaiter.h:410-429`).
- **They scrub the scheduler's queues, not just the suspended set.** Once a watcher has fired, the frame has already moved out of `suspended_coroutines_` and *into* the ready queue and in-flight set. `unregister_suspended()` alone would leave a dangling handle for the next drain to resume; `unschedule()` → `CoroutineScheduler::forget()` clears all three (`awaiter.h:262-266`; `scheduler.h:561`).
- **Queue-backed awaiters retract their own entry** — and several also *repair* the object they were parked on. A destroyed `sem.acquire()` that had already been granted a permit calls `release()` so capacity does not erode by one permanently (`sync.h:125-127`); a destroyed `mtx.lock()` whose handle is no longer in the queue means `unlock()` already handed it ownership, so it unlocks rather than leaving the mutex locked with no holder (`sync.h:470-471`); an auto-reset `async_event` re-`set()`s a consumed-but-unclaimed signal (`sync.h:1169-1170`); a destroyed `ch.recv()` whose sender already wrote through its result slot re-buffers the value so the message is not lost (`channel.h:286-288`). The send side has nothing to repair: a parked `ch.send()` or `send_for()` is served by its wake, which hands the value over before resuming it, so a sender reclaimed after its wake has sent, exactly once (Huly QB-272).
- **Combinators tear down what they spawned, in a fixed order.** `when_any`'s loser reclaim destroys the branch's spawned runner **first** — so the inner task's `continuation_`, which points at that frame, can never be resumed — then `forget`s the inner frame, then destroys the inner `task`, whose destructor stops any watcher it was parked on (`combinators.h:473-480`). Getting that order wrong is a use-after-free, which is why the source spells it out.

`~task()` is the blunt instrument at the bottom of all this: for a frame still in flight it calls `forget_frame_if_current(handle_)` and then `handle_.destroy()` (`task.h:665-668`). It does not wait, does not resume, and does not cancel cooperatively — the frame is destroyed where it sits, running the destructors of every live local. Every property above is what makes that safe.

One consequence for your own code: **a coroutine's locals are destroyed at `co_return`, not when the frame is later freed.** Anything a deferred operation needs must be owned by the frame — a parameter or a capture — not borrowed from a caller's stack.

### One notable inconsistency

`when_any`'s two overloads deliver a winning branch's exception differently. The variadic form **carries** it in `when_any_result::exception` and rethrows only when you call `get<T>()` or `rethrow_if_exception()` (`combinators.h:353`, `:357`, `:376`). The `std::vector<task<T>>` overload **rethrows directly** from `await_resume` (`combinators.h:635-636`). Same situation, opposite delivery.

`with_retry_until` has a similar edge: unlike both `with_retry` overloads it has **no** `try`/`catch`, so a throwing factory propagates immediately with no retry at all, and its `retry_exhausted` carries a null `last_error` (`retry.h:348-382`).

## Synchronization primitives

`coroutine/sync.h` provides primitives that suspend the coroutine instead of blocking the OS thread. They rely on cooperative single-thread scheduling for mutual exclusion — no OS locks are involved.

```cpp
// src: derived from qb/src/qb/io/async/coroutine/sync.h
#include <qb/io/async/coroutine.h>
using namespace qb::io::async;

// ── Counting semaphore ────────────────────────────────────────────────
semaphore sem(3);                              // 3 permits
co_await sem.acquire();
sem.release();
auto guard = co_await sem.scoped_acquire();    // RAII: release on scope exit
co_await sem.acquire(token);                   // cancellation-AWARE overload:
                                               // cancel() wakes it, throws cancelled_error

// ── Async mutex ───────────────────────────────────────────────────────
async_mutex mtx;
co_await mtx.lock();
mtx.unlock();
auto lk = co_await mtx.scoped_lock();          // RAII unlock

// ── Read/write lock ───────────────────────────────────────────────────
async_rw_lock rw;
{ auto r = co_await rw.scoped_read_lock();  /* concurrent reads */ }
{ auto w = co_await rw.scoped_write_lock(); /* exclusive write  */ }

// ── Barrier (reusable rendezvous) ─────────────────────────────────────
barrier b(4);
co_await b.arrive_and_wait();                  // wait for all 4 arrivals

// ── Event (manual- or auto-reset) ─────────────────────────────────────
async_event ready;                             // manual-reset: set() wakes all
ready.set();
co_await ready.wait();
async_event gate(/*auto_reset=*/true);         // auto-reset: set() wakes one
gate.set();

// ── Latch (one-shot countdown) ────────────────────────────────────────
async_latch latch(3);
latch.count_down();                            // non-suspending
co_await latch.wait();                         // resume when count reaches 0

// ── RAII helpers (run a synchronous callable while holding the resource)
co_await with_semaphore(sem, [] { return do_sync_work(); });
co_await with_lock(mtx,     [] { return do_sync_work(); });
```

Notes grounded in the headers: the mutex methods are `lock()` / `unlock()` / `scoped_lock()`; the read/write lock exposes `lock_read()` / `lock_write()` / `unlock_read()` / `unlock_write()` plus the RAII `scoped_read_lock()` / `scoped_write_lock()`. `async_event(bool auto_reset = false, bool initially_set = false)`. `with_semaphore` and `with_lock` take a *synchronous* callable and return its result (they `co_return f()`), not a task factory. Over-releasing a semaphore is a no-op; unlocking an unheld mutex or rw-lock asserts in debug builds.
<!-- src: qb/src/qb/io/async/coroutine/sync.h:250 (acquire), :365 (scoped_acquire), :507 (lock), :530 (unlock), :597 (scoped_lock), :903/:909 (scoped_read/write_lock), :797/:811 (unlock_read/write), :1049 (arrive_and_wait), :1130 (async_event ctor), :1216 (set), :1325 (count_down), :1474/:1505 (with_semaphore/with_lock), :283/:531 (no-op / assert) -->

Two properties are worth spelling out because they are the reason these primitives exist at all rather than `std::mutex` and friends. **The rw-lock is writer-preferring**: a reader is admitted only when no writer holds *and* `_write_waiters` is empty (`sync.h:713`), so a steady stream of readers cannot starve a writer. And **`semaphore::acquire(cancellation_token)` is the one cancellation-aware primitive in the whole layer** (`sync.h:261`) — everything else here is woken only by a matching `release()`, `unlock()`, `set()` or arrival. `with_semaphore` uses the *non*-token overload (`sync.h:1476`).

**A free lock is taken without suspending** (since 3.3, Huly QB-287). `co_await mtx.lock()`, `rw.lock_read()` and `rw.lock_write()` on a lock that is free for the caller complete synchronously in `await_ready`, as `sem.acquire()` with a free permit always did (`sync.h:478-483`, `:712-717`, `:764-769`, `:138-146`): `co_await mtx.lock()` is not a yield point when nobody holds the mutex, and a loop that counted on it to let other coroutines run must `co_await sleep(0ms)`, the cooperative yield. Until 3.3 a free lock was granted, the coroutine queued and suspended, and a grant on that path could not be given back if the frame was reclaimed before it resumed (a `when_any` loser): the mutex stayed locked with no holder, a reader count leaked, the write lock stuck. FIFO is the other half of the contract: `unlock()` hands the lock to the first waiter without releasing it (`sync.h:534-538`), so a newcomer arriving before that waiter resumes queues behind it, and every hand-off of the rw-lock keeps it held the same way. Queued readers are never stranded either: when the last waiting writer is reclaimed, the readers that queued behind it are admitted at once (`sync.h:929-932`, Huly QB-288).

## Channels

`coroutine/channel.h` defines a single-thread `channel<T>` for handing values between coroutines. It is non-copyable and non-movable; the capacity defaults to `0` (rendezvous: a send and a recv meet directly with no buffering).

```cpp
// src: derived from qb/src/qb/io/async/coroutine/channel.h
#include <qb/io/async/coroutine.h>
using namespace qb::io::async;
using namespace std::chrono_literals;

channel<int> ch(/*capacity=*/16);

// Producer
co_await ch.send(42);                          // suspends if the buffer is full
bool sent = ch.try_send(42);                   // non-blocking

// Consumer
std::optional<int> v = co_await ch.recv();     // empty optional when closed
std::optional<int> w = ch.try_recv();          // non-blocking

// Timed member operations
std::optional<int> r = co_await ch.recv_for(200ms);   // empty on timeout
bool ok               = co_await ch.send_for(7, 200ms); // false on timeout

ch.close();                                    // wakes pending recvs with empty;
                                               // pending sends throw channel_closed

// Pipeline utilities (free functions / factories)
co_await transform(in_ch, out_ch, [](int x) { return x * 2; });
co_await filter   (in_ch, out_ch, [](int x) { return x % 2 == 0; });
std::vector<int> all = co_await collect(in_ch);

auto chan          = make_channel<int>(16);              // std::unique_ptr<channel<int>>
auto [in_p, out_p] = make_pipeline<int, int>(            // pair of unique_ptr channels
    [](int x) { return x + 1; });
```

`recv_for` / `send_for` are **member functions** (`ch.recv_for(timeout)` returns `task<std::optional<T>>`; `ch.send_for(value, timeout)` returns `task<bool>`). `send(value)`/`recv()` return awaiters; `recv()` yields `std::optional<T>` that is empty once the channel is closed, while `send(value)` throws `channel_closed` on a closed channel. `make_channel` and `make_pipeline` return `std::unique_ptr` so the caller owns the channel's lifetime.

What `close()` does to each parked party is worth a table of its own, because the three answers differ (`channel.h:397-418`):

| Parked on | After `close()` |
|---|---|
| `recv()` / `recv_for()` | resumes with `std::nullopt` — never an exception, and a value still in `_buffer` is drained first (`channel.h:331-335`) |
| `send()` | resumes and **throws `channel_closed`** — a parked sender is resumed without a hand-off only by `close()` (`channel.h:221-226`) |
| `send_for()` | resumes and returns **`false`** — no exception: it reports whether a wake handed its value over, and `close()` hands nothing (`channel.h:614`, `:668-675`) |
| `select()` | resumes with `closed == true` and an empty `value` (`channel.h:416`) |

`~channel()` clears its liveness flag **before** calling `close()`, and the order is load-bearing: `close()` only *schedules* the resumes, so by the time they run the channel is gone and every awaiter must be able to answer from its own state alone (`channel.h:137-143`). A parked `recv` then returns `nullopt`, a parked `send` throws, a parked `send_for` returns `false` — the same answers as a plain close, reached without touching the freed object.

**A parked sender is served by its wake** (since 3.3, Huly QB-272). When room appears for a parked `send()` or `send_for()` — a slot freed by a receive, a receiver or a `select()` / `recv_for()` that parks — the first parked sender's value is handed over on the spot, to that receiver, select waiter or slot, and only then is the sender resumed, already done (`channel.h:802-819`). Every send goes through the one hand-off, `deliver()`: a waiting receiver, then a live select waiter, then a free buffer slot (`channel.h:767-782`). The buffer therefore never holds more than `capacity()` values, a capacity-0 channel never buffers, and parked senders are served in FIFO order. Until 3.3 the sender was only woken and delivered when it resumed, so a `try_send` or a fast-path send running in between took the freed room, and the woken sender then buffered past the capacity. One consequence to know when a send races in a `when_any`: a sender reclaimed after its wake has sent — its value is in the channel exactly once, as a `send_for` woken before its timer always was.

`send_for` also has a **move-only caveat the header documents explicitly** above `send_for` (`channel.h:614`): its slow path stores the pending value in a `std::any`, so for a `T` that is not copy-constructible the timed path can only ever resolve as a timeout. Use a copyable payload, or `send()` with an outer `with_deadline`.
<!-- src: qb/src/qb/io/async/coroutine/channel.h:131 (capacity default 0), :237 (send), :347 (recv), :514 (recv_for), :614 (send_for), :357/:380 (try_send/try_recv), :397 (close), :846 (make_channel), :1023 (make_pipeline), :947/:969/:991 (transform/filter/collect) -->

### `select` — first ready channel wins

```cpp
auto res = co_await select(ch_int, ch_string);   // select_result
if (res.index == 0)      use(res.get<int>());
else if (!res.closed)    use(res.get<std::string>());
```

`select(...)` returns `select_result { size_t index; bool closed; std::any value; }`: `index` is the 0-based channel that won, `closed` is true when that channel was closed (the value is then empty), and `get<T>()` casts the received value. The immediate pass checks every channel for **data before checking any for closure**, deliberately, so a closed channel in the set cannot starve a live one (`channel.h:1100-1124`).

One asymmetry to know before you compose `select` with anything that can abandon it: a `select` that resolved *with a value* and is then destroyed before it resumes — a `when_any` loser, a cancelled scope — **drops that value** (`channel.h:1151-1163`). `recv()` and `recv_for()` re-buffer theirs instead (`channel.h:286-288`, `:559-561`); `select` cannot, because by then the channels it was watching may already be gone and the only thing it still holds is its own refcounted state.
<!-- src: qb/src/qb/io/async/coroutine/channel.h:1070 (select_result), :1201 (select variadic), :1280 (select vector) -->

> A `channel<T>` is single-thread only. Its destructor clears an internal liveness flag *before* closing, so a parked sender or receiver whose frame is torn down does not touch freed channel memory. `channel_range` (and `async_stream::from_channel`) drain non-blocking and stop at the first empty slot — use [`async_stream`](#async-streams) for true async iteration, and prefer `from_channel_shared` to avoid the borrowed-reference lifetime trap.
<!-- src: qb/src/qb/io/async/coroutine/channel.h:137-143 (dtor clears _alive before close), :854-857 (channel_range does not suspend, Factbook); stream.h:98/:110 -->

## Structured concurrency: `coroutine_scope`

`coroutine/scope.h` groups child coroutines and bounds their lifetime to the scope.

```cpp
// src: derived from qb/src/qb/io/async/coroutine/scope.h
#include <qb/io/async/coroutine.h>
using namespace qb::io::async;
using namespace std::chrono_literals;

task<void> structured_work() {
    coroutine_scope scope;                       // default policy: cancel_all

    scope.spawn(worker_a());
    scope.spawn(worker_b());
    scope.spawn([cap]() -> task<void> { co_await process(cap); });  // closure-owning

    co_await scope.join_all();                    // wait for all children
    // size_t winner = co_await scope.join_any();  // wait for the first
    // bool   all_ok = co_await scope.join_all_for(2s);  // bounded wait

    scope.cancel_all();                           // signal the scope's token
    scope.rethrow_if_error();                     // re-throw the first failure
}

// RAII wrapper
co_await with_scope([](coroutine_scope& s) -> task<void> {
    s.spawn(worker());
    co_await s.join_all();
});

// Bounded-concurrency map: f comes BEFORE max_concurrency
std::vector<Result> out = co_await parallel_map(
    items,
    [](Item it) -> task<Result> { co_return process(it); },
    /*max_concurrency=*/4);

// Loop while a synchronous predicate holds; factory returns a fresh task each turn
co_await repeat_while(
    []        { return make_iteration_task(); },   // factory → task<void>
    []         { return should_continue(); });      // predicate → bool
```

`join_any()` returns `task<size_t>` (the completed index, or `total_count()` at once when the scope holds no task — never spawned into, or emptied by `prune_completed()`); `join_all_for(qb::duration)` returns `task<bool>`. The cleanup policy on scope destruction is one of `cancel_all` (default — signals the scope token), `join_all` (best-effort; children keep running via the shared scope state if you forgot to `co_await join_all()`), or `detach`. `parallel_map(items, f, max_concurrency = 10)` takes the mapping function *before* the concurrency limit; `repeat_while(factory, should_continue, cancel_token = {})` calls `should_continue()` synchronously and `factory()` to build each iteration's task. The ready-made `joining_scope`, `cancelling_scope`, and `detaching_scope` subclasses fix the policy.
<!-- src: qb/src/qb/io/async/coroutine/scope.h:81 (cleanup_policy), :153 (default cancel_all), :228/:255 (spawn task/Callable), :366 (join_all), :428 (join_any), :500 (join_all_for), :631/:640/:649 (scope subclasses), :729 (with_scope), :746 (repeat_while), :792 (parallel_map) -->

## Generators

`coroutine/generator.h` provides pull-based sequences.

### Synchronous `generator<T>`

Uses `co_yield`; `co_await` is disallowed (`await_transform` is deleted). Supports range-for.

```cpp
// src: derived from qb/src/qb/io/async/coroutine/generator.h
#include <qb/io/async/coroutine.h>
using namespace qb::io::async;

generator<int> fibonacci(int n) {
    int a = 0, b = 1;
    for (int i = 0; i < n; ++i) {
        co_yield a;
        std::tie(a, b) = std::pair{b, a + b};
    }
}

for (int v : fibonacci(10)) { use(v); }

auto gen   = range(0, 100);        // generator over [0, 100)
auto inf   = iota(0);              // infinite: 0, 1, 2, …
auto fromv = from_range(my_vector);
auto fromi = from_iterator(my_vector.begin(), my_vector.end());
```

A `generator<T>` must outlive any iterator over it; a throwing generator body surfaces the exception to the consumer rather than appearing as normal exhaustion. `collect_to_vector(gen)` has two overloads: an lvalue-reference one that drains a named generator in place, and an rvalue one so a composed pipeline — `collect_to_vector(take(range(0, 100), 4))` — can be drained in a single expression, matching every other helper in the family (all of which take their generator by value).

#### The transforms, and which of them consume the source

Both generator kinds carry the same operation set under the same names — the argument type picks the
overload, which is why there is no prefix on either side. What a reader has to know is not the names but
which ones **consume**:

| | lazy — returns a generator | terminal — drains the source |
| --- | --- | --- |
| `generator<T>` | `take` · `skip` · `map` · `filter` · `concat` | `for_each` · `reduce` · `collect_to_vector` |
| `async_generator<T>` | `take` · `skip` · `map` · `filter` | `for_each` · `reduce` · `collect_to_vector` · `map_to_vector` · `filter_to_vector` |

```cpp
// Lazy composes, and the composition is the reason to prefer it: nothing is produced that nobody asked
// for. This pulls exactly three values out of an INFINITE source.
auto first3 = collect_to_vector(take(map(iota(1), [](int v) { return v * v; }), 3));

// Terminal, so the source is gone afterwards. The seed comes FIRST, as in std::ranges::fold_left.
auto total  = reduce(range(1, 11), 0, std::plus<int>{});
for_each(range(0, 3), [](int v) { use(v); });
```

`map_to_vector` and `filter_to_vector` exist only on the async side and are **eager**: they run the source
to exhaustion and hand back a `task<std::vector<…>>`. They are not spelled `map`/`filter` on purpose —
those are lazy here and on `async_stream`, and one word cannot mean "lazy" in one family and "eager, and
it consumed your generator" in another. If you want the eager result from a lazy chain, say so:
`co_await collect_to_vector(map(src, f))`.

One property is worth relying on: **`take(gen, n)` pulls exactly `min(n, size(gen))` values and never one
more.** The limit is tested before the source is resumed. Over `iota` an extra pull would cost nothing,
which is exactly why the opposite behaviour survived for so long; over a source whose body reads a row, a
token or a socket byte, that pull is a side effect nobody asked for.
<!-- src: qb/src/qb/io/async/coroutine/generator.h:77 (generator), :112 (await_transform deleted), :527 (collect_to_vector lvalue), :554 (collect_to_vector rvalue), :580 (from_range), :621 (from_iterator), :636 (iota), range/repeat (:653/:668) -->

### Asynchronous `async_generator<T>`

Combines `co_yield` with `co_await`, so each element can wait on the event loop.

```cpp
async_generator<std::string> read_lines(std::string path);   // co_await + co_yield inside

co_await for_each(read_lines("log.txt"), [](std::string line) {
    process(line);                                            // synchronous consumer
});
co_await for_each(read_lines("log.txt"), [](std::string line) -> task<void> {
    co_await store(line);                                     // async consumer
});

std::vector<std::string> all = co_await collect_to_vector(read_lines("log.txt"));
auto sizes = co_await map_to_vector   (read_lines("f"), [](auto& s) { return s.size(); });
auto kept  = co_await filter_to_vector(read_lines("f"), [](auto& s) { return !s.empty(); });
auto total = co_await reduce(read_lines("f"), std::size_t{0},
                                [](auto acc, auto& s) { return acc + s.size(); });
```

`reduce(gen, init, reducer)` takes the seed before the reducer.
<!-- src: qb/src/qb/io/async/coroutine/generator.h:289 (async_generator), :794 (take), :808 (skip), :833 (map, lazy), :844 (filter, lazy), :867 (for_each), :897 (reduce: init then reducer), :915 (collect_to_vector), :941 (map_to_vector, eager), :960 (filter_to_vector, eager) -->

## Async streams

`coroutine/stream.h` adds a lazy, composable `async_stream<T>` over asynchronous sources.

```cpp
// src: derived from qb/src/qb/io/async/coroutine/stream.h
#include <qb/io/async/coroutine.h>
using namespace qb::io::async;
using namespace std::chrono_literals;

// Sources
auto s1 = async_stream<int>::from_vector({1, 2, 3, 4, 5});
auto s2 = async_stream<int>::from_channel(ch);            // ch must outlive the stream
auto s3 = async_stream<int>::from_channel_shared(ch_ptr); // shared ownership, no UAF
auto s4 = range_stream(1, 101);                           // [1, 100]
auto s5 = interval(100ms);                                // async_stream<size_t>: 0,1,2,…

// Lazy transforms (chainable)
auto pipeline = async_stream<int>::from_vector({1,2,3,4,5,6})
    .map   ([](int v) { return v * v; })
    .filter([](int v) { return v % 2 == 0; })
    .take(5)
    .skip(1);

// Terminal consumers (each is co_await-ed)
co_await pipeline.for_each([](int v) { use(v); });        // sync sink
std::vector<int>   v   = co_await pipeline.collect();
std::optional<int> hd  = co_await pipeline.first();
size_t             n   = co_await pipeline.count();
int                sum = co_await pipeline.reduce(0, [](int a, int b) { return a + b; });
bool               any = co_await pipeline.any([](int v) { return v > 10; });
bool               all = co_await pipeline.all([](int v) { return v > 0; });
std::optional<int> hit = co_await pipeline.find([](int v) { return v == 36; });
co_await pipeline.drain_to(out_channel);

// Combining
auto merged = merge_streams(std::vector{stream_a, stream_b});      // async_stream<T>
auto zipped = zip(stream_of_ints, stream_of_strings);             // pairs
```

The numeric source is `range_stream(start, end)` (there is no `async_stream<T>::range`). `merge_streams` takes a `std::vector<async_stream<T>>`; `zip(a, b)` yields `async_stream<std::pair<T, U>>`; `reduce(initial, f)` takes the seed then the reducer, and its accumulator need not be the element type; `for_each` also accepts a callable returning `task<void>` for an async sink.
<!-- src: qb/src/qb/io/async/coroutine/stream.h:169/:181/:189 (from_channel/_shared/_vector), :973 (range_stream), :922 (interval), :781 (merge_streams), :834 (zip), :227/:244/:261/:277 (map/filter/take/skip), :447/:455/:463/:494/:502/:511/:520/:529/:539 (for_each/collect/first/reduce/count/any/all/find/drain_to) -->

`backpressure(max_buffer[, semaphore])` and `debounce(delay)` run their source in a producer coroutine of their own — eagerly for `backpressure`, from the first pull for `debounce`. That producer lives as long as its source or the stream, whichever ends first: the stream and every copy of it hold it, and when the last one is gone — an early terminal (`first`, `take`, `any`, `find`), a stream dropped unread — the producer is destroyed wherever it waits, pulls nothing more from the source, and a caller-supplied semaphore gets back every permit it held. Values already pulled and buffered go with the stream (Huly QB-289).
<!-- src: qb/src/qb/io/async/coroutine/stream.h:118-140 (producer_lease), :348 (debounce), :561 (backpressure), :720/:746 (the two producers) -->

## Safe integration with `qb::Actor`

Under `qb-core`, each `VirtualCore` runs one thread, one listener, and one coroutine scheduler. The actor dispatch model assumes a handler runs to completion with exclusive access to its actor's state — but a `co_await` *yields the thread*, letting other handlers run in between. A coroutine that touches actor members across a suspension point is therefore a single-thread data race, and the actor may even be destroyed while the coroutine is parked.

The supported integration points are `Actor::spawn` and `Actor::spawn_detached`. Both launch an *isolated* coroutine and may communicate back only through events. `spawn` is the recommended default: the coroutine is *scoped* to the actor (cancelled when the actor is killed) and receives a `qb::ScopedCoroContext` with cancellation-aware operations, usable with the free `qb::ask()` request/reply helper. `spawn_detached` is the explicit fire-and-forget variant: the coroutine outlives the actor and receives a plain `qb::CoroContext` (an `ActorId`-by-value handle). The example below uses `spawn`.

```cpp
// src: derived from examples/03-coroutines/02-actor-coroutines.cpp
#include <qb/actor.h>
#include <qb/io/async/coroutine.h>
#include <memory>
#include <string>

// Event payloads must be trivially RELOCATABLE. The runtime moves an event with raw
// memcpy — the source pipe relocates what it already holds when it grows, reply/forward
// byte-recycle it, and a cross-core hop copies it twice — and a short std::string points
// into its own inline buffer on libstdc++, so a by-value one is invalid on every path.
// Box dynamic text so only the heap pointer travels (or use qb::string<N> when the
// length has a known bound). Note `request_id`, not `id`: qb::Event already has an
// `id` member (the event type id), and shadowing it stops the event from compiling.
struct StartProcessing : qb::Event {
    int                                request_id;
    std::shared_ptr<const std::string> data;
    StartProcessing(int rid, std::string d)
        : request_id(rid)
        , data(std::make_shared<const std::string>(std::move(d))) {}
};
struct ProcessingComplete : qb::Event {
    int                                request_id;
    std::shared_ptr<const std::string> result;
    uint64_t                           ns;
    ProcessingComplete(int rid, std::string r, uint64_t n)
        : request_id(rid)
        , result(std::make_shared<const std::string>(std::move(r)))
        , ns(n) {}
};

class CoroWorker : public qb::Actor {
    int processed_ = 0;
public:
    qb::io::async::task<bool> onInit() override {
        registerEvent<StartProcessing>(*this);
        registerEvent<ProcessingComplete>(*this);
        co_return true;
    }

    // Synchronous handler — runs with exclusive access to actor state.
    void on(StartProcessing& req) {
        int      rid   = req.request_id;    // capture by VALUE only
        auto     data  = req.data;          // shared_ptr copy — the bytes are not copied
        uint64_t start = time();

        spawn([rid, data, start](auto ctx) -> qb::io::async::task<void> {
            // Isolated context: NO access to actor members here.
            std::string result = co_await AsyncService::process_data(*data);
            uint64_t    ns     = ctx.time() - start;
            // `template` is required: ctx's type is dependent inside a generic lambda.
            ctx.template push<ProcessingComplete>(rid, std::move(result), ns);
        });
    }

    // Synchronous handler — exclusive access again.
    void on(ProcessingComplete& ev) {
        ++processed_;                       // safe: no suspension here
    }
};
```

`CoroContext` exposes exactly five members: `push<Event>(args…)` (send an event to the spawning actor — i.e. to `self`), `push_to<Event>(dest, args…)` (send to a specific `ActorId`), `broadcast<Event>(args…)` (fan out to every actor on all cores, mirroring `Actor::broadcast` — this is how `qb::require` sends its discovery ping), `id()`, and `time()`. Events sent to a now-dead actor are ignored, so the context is safe to use after any suspension. A `spawn` coroutine instead receives a `qb::ScopedCoroContext`, which derives from `CoroContext` and adds cancellation-aware operations (`sleep`, `until_cancelled`, `cancellation_point`, `cancellable`). For request/reply, use the free helper `qb::ask(ctx, target, Event{...}, timeout)` (declared in `qb/core/patterns/request.h`): it sends `Event` to `target` and `co_return`s the same `Event` filled in by the responder's `reply()` — e.g. `auto r = co_await qb::ask(ctx, target, PriceQuery{"BTC"}, 500ms);`. `has_active_coroutines()` reports whether the actor still has spawned coroutines in flight.
<!-- src: qb/src/qb/core/Actor.h:1657 (class CoroContext), :1677 (push), :1689 (push_to), :1698 (broadcast), :1713 (time), :2192 (ScopedCoroContext), :1483 (spawn), :1446 (spawn_detached); qb/src/qb/core/patterns/request.h:276 (ask free helper); qb/src/qb/core/Actor.cpp:513,534 (__resolve_coro_scheduler__ debug-asserts a TLS scheduler) -->

| Rule | Reason | Source |
|---|---|---|
| Event handlers stay `void on(Event&)` | `registerEvent` requires a `void` handler; a `task<void> on(Event&)` breaks actor dispatch | `Actor.h:1010` |
| Use `spawn()` (or `spawn_detached()`) for coroutine work | isolates the coroutine from live actor state | `Actor.h:1483`, `:1446` |
| Capture by **value** inside the lambda | a reference (or `this`) dangles after the first `co_await` | `Actor.h:1405-1407`, `:1463-1464`; examples/03-coroutines/02-actor-coroutines.cpp:138 |
| Communicate via `ctx.push` / `ctx.push_to` | preserves message-passing semantics; an event addressed to an actor that is already gone finds no subscribed handler, so it is reported as a dead letter and disposed instead of delivered | `Actor.h:1676-1677` (`push`), `:1688-1689` (`push_to`); `qb/src/qb/system/event/router.h:524-541` (missing destination → report and dispose), `qb/src/qb/core/VirtualCore.cpp:1542-1546` (dead letter); when the event type itself is unregistered, `qb/src/qb/system/event/router.h:1130-1165` |
| Process results in a synchronous handler | guarantees exclusive access to actor state | `Actor.h:1401-1403` |

`spawn()` and `spawn_detached()` must be called on the actor's own `VirtualCore` thread (each debug-asserts that a thread-local scheduler exists). They are the only supported way to use coroutines inside an actor — `run`, `run_for` and `run_sync` block that thread, and [the framework's guard does not fire from a handler](./async_system.md#the-guard-and-what-it-actually-checks).

One corollary of [the cancellation table](#every-awaitable-and-what-cancellation-does-to-it) applies specifically here, and it is the sharpest thing on this page. `kill()` cancels the actor's coroutine scope, which **signals the token** — by itself that stops nothing.

A coroutine parked on a cancellation-aware operation unwinds promptly, because that awaiter registered a hook. All five of the context's own operations qualify: `ctx.sleep(d)` is `cancellable_sleep` (`src/qb/core/Actor.h:2237`), `ctx.until_cancelled()` is `check_cancelled` (`src/qb/core/Actor.h:2258`), `ctx.cancellable(t)` is `make_cancellable` (`src/qb/core/Actor.h:2270`), `ctx.offload(fn, args...)` is `make_cancellable` over an [`offload`](#offloading-blocking-work) — the kill ends the wait, the call runs on to its end on the pool and its result is discarded on the core (`src/qb/core/Actor.h:2301-2304`), and `qb::ask` links an embedded `cancel_hook` on the same token — no `std::function`, nothing allocated, unlinked in O(1) when the reply lands (`src/qb/core/Actor.h:1961`). `ctx.cancellation_point()` is a near relative rather than a member of that set: it returns a `yield_or_cancel` that hands the loop a turn and throws if the token fired while it was away (`src/qb/core/Actor.h:2248`), so it is prompt inside a loop but cannot be woken out of a long wait.

A coroutine parked on **anything else** is listening to nothing. It is neither woken nor unwound; it resumes when its own operation finishes, into a world where its actor is gone. The `CoroContext` makes that safe rather than fatal — an event addressed to a dead actor finds no handler and is disposed — but the work is not cancelled, and whatever it holds is not released until it completes. **To be interruptible, an unwrapped await must be wrapped**: `ctx.cancellable(op)`, `with_deadline(op, deadline, ctx.token())`, or a `when_any` against `ctx.until_cancelled()`.
<!-- src: qb/src/qb/core/Actor.cpp:532; qb/src/qb/io/async/listener.h:1424 (ensure_not_inside_ready_drain) -->

## Lifetime footguns

The most common coroutine bug in this layer is a dangling capture: a *temporary* lambda is destroyed as soon as its call expression finishes, but the coroutine frame may outlive it and reference its captures after the first suspension.

```cpp
// src: derived from qb/src/qb/io/async/coroutine.h, qb/src/qb/io/async/coroutine/task.h
using namespace qb::io::async;
using namespace std::chrono_literals;

// WRONG: temporary lambda is gone before the coroutine resumes
auto t = [&data]() -> task<void> {
    co_await sleep(100ms);
    use(data);                       // dangling reference
}();

// OK: store the lambda so it outlives its invocation
auto fn = [&data]() -> task<void> {
    co_await sleep(100ms);
    use(data);
};
coro_scheduler().spawn(fn());        // fn is alive throughout

// BEST: hand the lambda itself to spawn(); the closure is moved into an owning frame
coro_scheduler().spawn([data_copy = data]() -> task<void> {
    co_await sleep(100ms);
    use(data_copy);                  // copy lives in the coroutine frame
});

// WRONG: loop variable captured by reference
for (int i = 0; i < 5; ++i) {
    tasks.push_back([&i]() -> task<int> {
        co_await sleep(10ms);
        co_return i * 10;            // i is out of scope / wrong value
    }());
}

// OK: pass the loop variable as a by-value parameter (copied into the frame)
auto worker = [](int id) -> task<int> {
    co_await sleep(10ms);
    co_return id * 10;
};
for (int i = 0; i < 5; ++i) {
    tasks.push_back(worker(i));      // i is copied into the argument
}
```

Three rules cover every case: function parameters are copied into the coroutine frame, so passing data as an argument is always safe; the `spawn(Callable)` and `coroutine_scope::spawn(Callable)` overloads move the closure into an owning frame for you; and a coroutine local lives until `co_return`, not until the frame is destroyed. Note that a coroutine's locals are destroyed at `co_return` — not when the spawned frame is later freed — so anything a deferred operation needs must be owned by the frame (a parameter or a capture), not borrowed from a caller stack.
<!-- src: qb/src/qb/io/async/coroutine/scheduler.h:573-604 (spawn Callable), :1036 (invoke_owned_), task.h:697-698; coroutine.h (capture-safety guidance); io_invariants Factbook scheduler.h:601 -->

> **Scheduler teardown.** `~CoroutineScheduler` destroys only ready-queue frames it owns plus deferred completed frames; *suspended* frames are intentionally left alone because their libev watchers still reference them. Stop the event loop before destroying the scheduler. The listener does this on its own destruction (`reset_coro_scheduler()` runs `destroy_all_suspended()` first, so a frame parked on the listener's scheduler is destroyed, not abandoned); a scheduler owned directly and destroyed with frames still suspended abandons them, and says so in every build: one WARNING line on `qb::io::cerr` and the count added to `qb::io::async::abandoned_coroutine_frames_total()`, the process-wide tally (Huly QB-84).
<!-- src: qb/src/qb/io/async/coroutine/scheduler.h:364-394 (rationale), :395-434 (teardown, the abandoned-frame report at :424-425), task.h:150-164 (report_abandoned_coroutine_frames / abandoned_coroutine_frames_total) -->

## Compiler note — by-value parameters under clang older than 22

A parameter taken by value is copied into the coroutine frame, which is what makes it safe (above). Clang before 22 lays that copy out wrong in one precise case. On an ABI that passes a large trivially copyable argument `byval` — x86-64 System V, so Linux and Intel macOS — when the body never *writes* the parameter, the optimiser folds the copy back into the caller's argument slot, and the coroutine pass then spills that slot into the frame at the natural alignment of the LLVM struct type (8 bytes) while every read keeps the C++ alignment (64 for a `qb::Event`). The first aligned vector copy out of the slot faults whenever the slot's frame offset is not a multiple of the vector width: a crash that appears or vanishes with the frame layout, at `-O2` and above only, and that `-O0`, ASan and UBSan do not show (LLVM issue 159571, fixed by pull request 159765 in LLVM 22, backported to no earlier branch). GCC and MSVC build parameter copies as frame members with their own alignment; under clang-cl the Windows ABI passes such arguments by reference, so the defect cannot trigger there.

`qb::io::async::pin_frame_copy(param)` is the shield: a memory-operand asm barrier that makes the copy a written, escaped object, so the optimiser keeps it and the frame lays it out at its declared alignment. The asm emits no instruction; the copy costs what the spill cost, plus one 64-byte stack copy the optimiser used to forward when a pattern hands the request to an inner coroutine — and the call is a no-op everywhere but clang on a `byval` ABI. Every `qb::ask*` pattern coroutine opens with it on its request (`qb::ask` itself is an awaitable, not a coroutine, since 3.2); a coroutine of yours that takes an event by value and does not assign to it before its first `co_await` should do the same while it has to build with an older clang.

```cpp
// src: derived from qb/src/qb/io/async/coroutine/utils.h:378 (pin_frame_copy), qb/src/qb/core/patterns/resilience.h:559-560 (ask_guarded)
struct Order  : qb::Event { int amount{0}; };   // every qb::Event is 64-byte aligned
struct Placed : qb::Event { int amount{0}; };

qb::io::async::task<void> place(qb::ScopedCoroContext ctx, qb::ActorId book, Order order) {
    qb::io::async::pin_frame_copy(order);          // first statement: the copy stays a frame member at alignof(Order)
    co_await ctx.sleep(std::chrono::milliseconds{10});
    ctx.push_to<Placed>(book, order.amount);       // read after the suspension: the copy the frame holds
}
```
<!-- src: qb/src/qb/io/async/coroutine/utils.h:378 (pin_frame_copy); qb/src/qb/core/patterns/request.h:231 (pin_frame_copy), resilience.h:468 (pin_frame_copy), :560 (pin_frame_copy), scatter.h:60 (pin_frame_copy), streaming.h:314 (pin_frame_copy) -->

## Seeing what a coroutine waits on

A coroutine that never resumes says nothing: it holds its frame and waits, and nothing in the program fails.
`CoroutineScheduler::dump()` (3.3) lists the coroutines parked on the calling thread — the roots the scheduler
owns, by the name each was spawned with, and, with suspension tracking on, what each one waits on, for how long,
and the coroutine it awaits — the longest waits first.

```cpp
auto &sched = qb::io::async::coro_scheduler();
sched.set_suspension_tracking(true);          // this thread only; off by default
sched.spawn("refresh-cache", refresh_cache()); // a name the dump reports while the frame lives
// ... later, on the same thread
sched.dump(std::cerr);
```

```text
coroutines parked: 2 (tracking on)
  "refresh-cache" 0x5813a7ef78c0 task 12.4 s -> 0x5813a7ef5380 pgsql 12.4 s
```

The written form puts each root on one line, followed by the chain of coroutines it awaits down to the one that
is actually parked; `dump()` returns the same as `std::vector<parked_coroutine>` — `frame`, `name`, `kind`, `age`,
`waits_on`, `root`. Inside an actor, name the coroutine at spawn — `spawn("name", fn)`, `spawn_detached("name", fn)`
— and dump the core's scheduler, `qb::io::async::listener::current.coro_scheduler()`, from a handler.

| | Tracking off (the default) | Tracking on |
|---|---|---|
| A suspension costs | one load of a process-wide count and one branch while no thread tracks — names or not — no thread-local access, no clock, nothing in the frame | a record: kind, the CPU counter, the awaited frame; a task's or a generator's hand-over takes one more hop, through the thread's recorder coroutine |
| A frame's destruction costs | the same while no thread tracks or names; one thread-local read while one does | the erasure of its record (a hash lookup) |
| The dump lists | the roots, by name, and the coroutines parked on a loop watcher (`"watcher"`) | every coroutine parked on a labelled awaiter, with its age, chained through the tasks and generators it awaits |

Every awaiter of qb and of the modules labels its suspension in `await_suspend` — first thing, or, for the two that
hand the thread straight to another coroutine (a task's, a generator's), last, just before they hand it over:

| Kind | Waits on | Header |
|---|---|---|
| `task` | the coroutine it `co_await`s (`waits_on`) | `task.h` |
| `sleep`, `io`, `async`, `offload` | a timer, a socket, a callback bridge, a pool thread | `awaiter.h`, `offload.h` |
| `semaphore`, `mutex`, `read lock`, `write lock`, `barrier`, `event`, `latch` | a synchronisation primitive | `sync.h` |
| `channel send`, `channel recv`, `channel select` | a channel | `channel.h` |
| `when_all`, `when_any`, `timeout`, `scope join`, `shared task` | its children, a deadline, a shared result | `combinators.h`, `scope.h`, `shared_task.h` |
| `cancellation`, `yield`, `cancellable`, `sleep`, `deadline` | the cancellation primitives | `cancellation.h` |
| `generator next`, `generator yield` | the producer it pulls from (`waits_on`), the consumer's next pull | `generator.h` |
| `connect` | a TCP connect | `tcp/connector.h` |
| `ask`, `ask quorum`, `ask stream`, `require`, `actor ready` | an actor's answer, a discovery, an activation | qb-core |
| `http`, `pgsql`, `pgsql connect`, `redis`, `redis connect` | a request, a query, a command, a connect | qbm-http, qbm-pgsql, qbm-redis |

What to know before reading a dump:

- **It is per thread.** Tracking, records and the dump belong to the thread that runs the coroutines; call them
  there. Turning tracking on records the suspensions from then on, and turning it off drops every record.
- **A record is the coroutine's LAST labelled suspension.** A coroutine already in the ready queue still shows the
  wait it is leaving. An awaitable of your own labels itself with `qb::io::async::track_suspension(h, "kind")` as
  the first statement of its `await_suspend` — `kind` must outlive the wait; a literal — or the coroutine parked on
  it keeps the record of its previous wait, ageing. `scripts/check-awaiter-tracking.py` holds every awaiter of qb
  and of the modules to that rule.
- **A record goes with its frame.** A cancelled, completed or destroyed coroutine leaves the dump, and so does its
  name; a completed root waiting for the end-of-pass drain is not listed.
- **A name costs while it lives.** Names do not need tracking, but while a named coroutine lives on a thread, every
  coroutine frame destroyed there is looked up in the name table (a hash lookup), and every frame destroyed on
  another thread reads a thread-local flag. Suspensions pay nothing for names, and a program that names nothing
  pays nothing for them: the test is the one the promise destructor already makes for the records.
- `dump()` allocates and sorts the parked coroutines: a diagnostic, not a hot-path call.

<!-- src: qb/src/qb/io/async/coroutine/scheduler.h:337 (parked_coroutine), :611/:618 (spawn with a name), :633/:637 (set_suspension_tracking/suspension_tracking), :647/:651 (dump/dump to a stream); qb/src/qb/io/async/coroutine/tracking.h:125 (the branch), :164 (track_suspension for your own awaitable); qb/src/qb/io/async/coroutine/tracking.cpp:342 (longest waits first); qb/src/qb/core/Actor.h:1493/:1497 (spawn/spawn_detached with a name) -->

## Debug tracing

Each macro is a compile-time flag (`-DQB_DEBUG_CORO_LIFECYCLE=1`); when set it emits trace output to `stderr`.

| Macro | What it traces | Source |
|---|---|---|
| `QB_DEBUG_COROUTINES` | `task<T>` promise lifecycle, awaiter `on_event_ready`, timer fire | `task.h:94`, `awaiter.h:208` |
| `QB_DEBUG_SCOPE` | `coroutine_scope` spawn / join / completion | `scope.h:42` |
| `QB_DEBUG_CORO_LIFECYCLE` | `CoroutineScheduler` teardown, suspended-count traces | `scheduler.h:56-57` |
| `QB_DEBUG_AGEN` | `async_generator` yield / next / suspend flow | `generator.h:35-37` |

`QB_DEBUG_SCOPE` and `QB_DEBUG_CORO_LIFECYCLE` share the scheduler trace channel.
<!-- src: qb/src/qb/io/async/coroutine/{task.h:94, awaiter.h:208, scope.h:42, scheduler.h:56-57, generator.h:35-37} -->

## Header reference

| Header | Key exports |
|---|---|
| `coroutine/task.h` | `task<T>` — primary coroutine return type |
| `coroutine/shared_task.h` | `shared_task<T>`, `make_shared_task` — multi-consumer result |
| `coroutine/scheduler.h` | `CoroutineScheduler`, `schedule_via_current`, `parked_coroutine` |
| `coroutine/tracking.h` | `track_suspension` — label an awaitable of your own for `CoroutineScheduler::dump()` |
| `coroutine/awaiter.h` | `awaiter_base`, `timer_awaiter`, `socket_awaiter`, `async_awaiter<T>` |
| `coroutine/utils.h` | `sleep`, `wait_readable` / `wait_writable` / `wait_for_io`, `coro_scheduler`, `run_for`, `run_sync` |
| `coroutine/offload.h` | `offload`, `offload_awaiter<R>`, `set_offload_threads`, `current_offload_stats`, `offload_stats` |
| `coroutine/combinators.h` | `when_all`, `when_any`, `race`, `coro_with_timeout`, `timeout_error`, `when_any_result` |
| `coroutine/cancellation.h` | `cancellation_token`, `cancelled_error`, `check_cancelled`, `yield_or_cancel`, `make_cancellable`, `cancellable_sleep`, `with_deadline` |
| `coroutine/sync.h` | `semaphore`, `async_mutex`, `async_rw_lock`, `barrier`, `async_event`, `async_latch`, `with_semaphore`, `with_lock` |
| `coroutine/channel.h` | `channel<T>`, `channel_closed`, `select`, `make_channel`, `make_pipeline`, `transform`, `filter`, `collect` |
| `coroutine/scope.h` | `coroutine_scope`, `joining_scope` / `cancelling_scope` / `detaching_scope`, `with_scope`, `parallel`, `parallel_map`, `repeat_while` |
| `coroutine/generator.h` | `generator<T>`, `async_generator<T>`, `range`, `iota`, `from_range`, `repeat` / `repeat_n`, `concat`; lazy `take` / `skip` / `map` / `filter`; terminal `for_each` / `reduce` / `collect_to_vector`; eager `map_to_vector` / `filter_to_vector` |
| `coroutine/stream.h` | `async_stream<T>`, `range_stream`, `interval`, `merge_streams`, `zip`, `timer`, `repeat_value`, `from_generator` |
| `coroutine/retry.h` | `retry_policy`, `backoff_strategy`, `retry_exhausted`, `with_retry`, `with_retry_until`, `make_retryable`, predefined policies |
| `coroutine.h` | umbrella include for everything above |
<!-- src: qb/src/qb/io/async/coroutine.h (include list) and each cited header -->

## Retry policies

`coroutine/retry.h` wraps a task factory with backoff-driven retries.

```cpp
// src: derived from qb/src/qb/io/async/coroutine/retry.h
#include <qb/io/async/coroutine.h>
using namespace qb::io::async;
using namespace std::chrono_literals;

retry_policy policy {
    .max_attempts = 5,
    .base_delay   = 100ms,
    .max_delay    = 30s,
    .strategy     = backoff_strategy::exponential_jitter,
    .is_retryable = [](const std::exception& e) { return is_transient(e); },
    .on_retry     = [](std::size_t attempt, const std::exception& e) { log_retry(attempt, e); },
};

// Retry until the task succeeds (or attempts are exhausted → retry_exhausted)
int result = co_await with_retry(
    []() -> task<int> { co_return co_await fetch(); }, policy);

// Retry until a success predicate over the result holds
std::string status = co_await with_retry_until(
    []() -> task<std::string> { co_return co_await get_status(); },
    [](const std::string& s)  { return s == "ready"; },
    policy);

// Bind a factory + policy into a reusable retrying callable
auto retryable = make_retryable([]() -> task<int> { co_return co_await fetch(); }, policy);
int again = co_await retryable();
```

`retry_policy` defaults: `max_attempts = 3`, `base_delay = 100ms`, `max_delay = 30000ms`, `strategy = backoff_strategy::exponential`. `on_retry` has signature `void(std::size_t attempt, const std::exception&)`. When all attempts fail, `with_retry` throws `retry_exhausted`. The predefined policies are: `transient_network_policy()` — 5 attempts, exponential-jitter backoff, retryable on common transient-error substrings; `idempotent_policy()` — 10 attempts, exponential-jitter, always retryable; `aggressive_retry_policy()` — 20 attempts, linear backoff, always retryable.
<!-- src: qb/src/qb/io/async/coroutine/retry.h:85-97 (retry_policy defaults, on_retry signature), :219 (with_retry), :350 (with_retry_until), :418 (make_retryable), :45 (retry_exhausted), :428/:446/:460 (predefined policies) -->

## See also

- [The async runtime](./async_system.md) — the loop turn this layer is drained by, the bounded coroutine drain, and the `run_sync` rule.
- [What has no coroutine form](./gaps.md) — accept, QUIC, signals, file I/O and DNS, with the structural reason for each.
- [Transports](./transports.md) — the callback stack `wait_readable` sits underneath.
- [Protocols](./protocols.md) — the framing contract that turns bytes into the messages a session handler receives.
- [Async, lifecycle, and allocation invariants](../7_reference/io_invariants.md) — the scheduler and awaiter guarantees in reference form.
