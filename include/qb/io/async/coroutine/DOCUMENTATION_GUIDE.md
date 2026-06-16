# QB Coroutine Documentation Guide

**Last Updated**: 2026-03-16  
**Status**: ✅ All 164 tests passing across 9 suites  
**Model**: Single-thread cooperative — one `VirtualCore` per thread, one `CoroutineScheduler` per thread

---

## Architecture Overview

```
qb::io::async::listener  (one per thread, owns the libev loop)
    └── CoroutineScheduler  (thread_local, lock-free MPSC ready queue)
            ├── spawn(task<void>&&)          — owned, fire-and-forget
            ├── spawn(Callable)              — owned, lambda-safe overload
            ├── schedule_resume(handle)      — continuation wakeup
            └── run_ready()                  — called by listener after ev loop tick
```

Because all coroutines on a thread share one scheduler and one event loop, **they are never concurrent**. The only interleaving point is `co_await`. This means:

- No `std::mutex` needed inside coroutine primitives
- No `std::atomic` needed for shared coroutine state
- Natural mutual exclusion between suspension points

---

## File Map

| Header | Exports |
|--------|---------|
| `task.h` | `task<T>` — primary coroutine return type |
| `scheduler.h` | `CoroutineScheduler`, `schedule_via_current()` |
| `awaiter.h` | `timer_awaiter`, `socket_awaiter`, `async_awaiter<T>` |
| `utils.h` | `sleep()`, `wait_readable()`, `wait_writable()`, `coro_scheduler()`, `run_for()` |
| `combinators.h` | `when_all()`, `when_any()`, `race()`, `coro_with_timeout()` |
| `cancellation.h` | `cancellation_token`, `cancellable_operation`, `cancellable_sleep`, `with_deadline` |
| `sync.h` | `semaphore`, `async_mutex`, `async_rw_lock`, `barrier`, `async_event`, `async_latch` |
| `channel.h` | `channel<T>`, `select()`, `recv_for()`, `send_for()`, `transform()`, `filter()` |
| `scope.h` | `coroutine_scope`, `with_scope()`, `parallel_map()`, `repeat_while()` |
| `generator.h` | `generator<T>`, `async_generator<T>`, `ag_for_each()`, `ag_collect()`, `ag_map()`, `ag_filter()`, `ag_reduce()` |
| `stream.h` | `async_stream<T>` — functional stream pipeline |
| `retry.h` | `retry_policy`, `with_retry()`, `with_retry_until()`, `make_retryable()` |
| `mixin.h` | `coro_mixin<Derived>` — CRTP helper for actors |
| `coroutine.h` | Umbrella include for all of the above |

---

## Critical Safety Rules

### Rule 1 — Lambda / Callable Spawning (THE most common error)

**Root cause**: when you write `spawn(f())`, the closure of `f` is destroyed at the end of the expression, leaving the coroutine frame with a dangling `this` pointer to the closure.

#### Old workaround (still valid, but verbose)
```cpp
// Store the lambda so it outlives the coroutine invocation:
auto fn = [captured]() -> task<void> { co_await ...; };
coro_scheduler().spawn(fn());   // fn is alive, no dangle
```

#### New preferred pattern — `spawn(Callable)` overload
Both `CoroutineScheduler` and `coroutine_scope` expose a callable overload that wraps the lambda in an owning `invoke_owned_` / `owned_invoke_` coroutine (value parameter ⟹ always alive):

```cpp
// ✅ BEST — pass the lambda WITHOUT trailing ()
coro_scheduler().spawn([captured]() -> task<void> {
    co_await do_work(captured);
});

scope.spawn([captured, i]() -> task<void> {
    co_await process(captured, i);
});
```

> **Constraint**: the callable must take no arguments and return `task<void>`.

#### Terminal methods on `async_stream<T>` — same pattern applied internally
All terminal consumers (`for_each`, `collect`, `first`, `reduce`, `count`, `any`, `all`, `find`, `drain_to`) are non-coroutine shims that move `*this` into a private static helper:

```cpp
// The stream is moved into the coroutine frame — no dangling this
task<void> consume(async_stream<int> stream) {
    co_await stream.for_each([](int v) { process(v); });
    // Equivalently: co_await stream.for_each([](int v) -> task<void> { co_await log(v); });
}
```

---

### Rule 2 — `task<T>` is move-only

```cpp
auto t = my_coroutine();
coro_scheduler().spawn(std::move(t));   // ✅ — t is emptied
// coro_scheduler().spawn(t);           // ❌ compile error
```

---

### Rule 3 — Never hold a lock across `co_await`

```cpp
// ❌ WRONG — lock held while event loop runs other coroutines
task<void> bad() {
    std::lock_guard lk(some_mutex);
    co_await sleep(10ms);           // DEADLOCK potential
}

// ✅ CORRECT
task<void> good() {
    { std::lock_guard lk(some_mutex); /* quick sync work */ }
    co_await sleep(10ms);
}

// ✅ BEST — use async_mutex which suspends instead of blocking
task<void> best(async_mutex& mtx) {
    auto guard = co_await mtx.scoped_lock();
    co_await sleep(10ms);           // fine — no OS thread blocked
}
```

---

### Rule 4 — cross-thread cancellation

`cancellation_token` has **no mutex**. Cross-thread cancellation must be routed through the qb actor event system:

```cpp
// Thread B — send a Cancel event to Thread A
actor_on_B.send<CancelEvent>(actor_on_A.id());

// Thread A — event handler calls cancel on its own thread
void on(CancelEvent) { my_token.cancel(); }
```

---

## Complete Feature Reference

### `task<T>` — Core type (`task.h`)

```cpp
task<int> fetch() {
    co_await sleep(100ms);
    co_return 42;
}

task<void> caller() {
    int v = co_await fetch();   // symmetric transfer, no stack growth
}
```

Key points:
- `initial_suspend = suspend_always` — task is lazy (doesn't run until spawned/awaited)
- `final_suspend` performs symmetric transfer to `continuation_`
- `promise_type::result_` variant is **explicitly initialised** to `std::monostate` (index 0) to prevent premature ready state

---

### `CoroutineScheduler` — Execution engine (`scheduler.h`)

```cpp
// Spawn fire-and-forget
coro_scheduler().spawn(std::move(task));     // task<void>
coro_scheduler().spawn([]() -> task<void> { co_await ...; }); // callable

// State inspection
coro_scheduler().active_count();   // in-flight + ready queue
coro_scheduler().pending_count();  // ready queue size only
coro_scheduler().has_ready();

// Manual step (usually called by listener)
coro_scheduler().run_ready();
```

---

### Awaiters (`awaiter.h` / `utils.h`)

```cpp
co_await sleep(100ms);                    // timer
co_await wait_readable(fd);              // EV_READ
co_await wait_writable(fd);             // EV_WRITE
co_await wait_for_io(fd, EV_READ|EV_WRITE);

// Bridge callback API → coroutine
int result = co_await async_awaiter<int>([](auto cb) {
    legacy_async_op([cb](int r) { cb(r); });
});
```

---

### Combinators (`combinators.h`)

```cpp
// Wait for ALL — returns std::tuple or std::vector
auto [a, b] = co_await when_all(task_a(), task_b());
std::vector<int> results = co_await when_all(std::move(vec_of_tasks));

// Wait for FIRST — returns when_any_result { .index, .value }
auto [idx, val] = co_await when_any(fast_task(), slow_task());

// Wait for first, no result needed
co_await race(timer_task(), data_task());

// With timeout
auto result = co_await coro_with_timeout(fetch(), 500ms);
// result is std::optional<T>; nullopt = timed out
```

---

### Cancellation (`cancellation.h`)

```cpp
cancellation_token token;

// Cancellable worker
task<void> worker(cancellation_token tok) {
    co_await cancellable_sleep(500ms, tok);   // throws cancelled_error on cancel
}

// Deadline wrapper
task<void> bounded(cancellation_token tok) {
    co_await with_deadline(tok, 200ms, inner_task());
}

// Register callback
token.on_cancel([]() { cleanup(); });

// Cancel (same thread only!)
token.cancel();
```

---

### Synchronisation primitives (`sync.h`)

```cpp
// Semaphore
semaphore sem(3);
co_await sem.acquire();
auto guard = co_await sem.scoped_acquire();  // RAII

// Async mutex (no OS blocking)
async_mutex mtx;
co_await mtx.lock();
auto lk = co_await mtx.scoped_lock();       // RAII

// Read-write lock
async_rw_lock rw;
auto r = co_await rw.read_lock();
auto w = co_await rw.write_lock();

// Barrier (reusable rendezvous)
barrier b(3);
co_await b.arrive_and_wait();               // all 3 must arrive

// Async event (broadcast / auto-reset)
async_event ready;
ready.set();                                // manual-reset: wakes all
co_await ready.wait();

async_event gate(/*auto_reset=*/true);      // wakes exactly one
gate.set();

// Latch (one-shot countdown)
async_latch latch(3);
latch.count_down();                         // decrement
co_await latch.wait();                      // wait for count == 0

// RAII helpers
co_await with_semaphore(sem, []() -> task<void> { co_await work(); });
co_await with_lock(mtx, []() -> task<void>      { co_await work(); });
```

---

### Channel — MPSC communication (`channel.h`)

```cpp
channel<int> ch(/* capacity */ 16);

// Producer
co_await ch.send(42);
bool ok = ch.try_send(42);               // non-blocking

// Consumer
auto opt = co_await ch.recv();           // std::optional<int>; nullopt = closed
auto opt = ch.try_recv();               // non-blocking

// Timed operations
auto opt = co_await recv_for(ch, 200ms);
bool ok  = co_await send_for(ch, val, 200ms);

// Close
ch.close();

// Select — wait on multiple channels
auto [winner, value] = co_await select(ch_a, ch_b, ch_c);
// winner: channel index; value: std::any

// Pipelines
co_await transform(in_ch, out_ch, [](int x) { return x * 2; });
co_await filter(in_ch, out_ch, [](int x) { return x % 2 == 0; });
auto [in, out] = make_pipeline<int, int>([](int x) { return x + 1; });
```

---

### Structured concurrency (`scope.h`)

```cpp
task<void> structured() {
    coroutine_scope scope;

    scope.spawn(worker_a());
    scope.spawn(worker_b());
    scope.spawn([capture]() -> task<void> { co_await work(capture); }); // lambda-safe

    co_await scope.join_all();              // wait all
    co_await scope.join_any();              // wait first
    co_await scope.join_all_for(500ms);     // wait all or timeout
    scope.cancel_all();                     // signal cancellation
    scope.rethrow_if_error();               // propagate first exception
}

// Structured scope via RAII helper
co_await with_scope([](coroutine_scope& s) -> task<void> {
    s.spawn(worker());
    co_await s.join_all();
});

// Parallel map
auto results = co_await parallel_map(items, max_concurrency,
    [](Item item) -> task<Result> { co_return process(item); });

// Repeat with condition
co_await repeat_while(
    []() -> task<bool> { co_return should_continue(); },
    []() -> task<void> { co_await do_one_iteration(); }
);
```

Cleanup policies: `cancel_all` (default), `join_all`, `detach`.

---

### Generators (`generator.h`)

```cpp
// Sync generator (range-for, no co_await inside)
generator<int> fibonacci(int n) {
    int a = 0, b = 1;
    for (int i = 0; i < n; ++i) {
        co_yield a;
        std::tie(a, b) = std::make_pair(b, a + b);
    }
}
for (int v : fibonacci(10)) { use(v); }

// Async generator (co_yield + co_await allowed)
async_generator<int> async_range(int n) {
    for (int i = 0; i < n; ++i) {
        co_await sleep(1ms);
        co_yield i;
    }
}

// Consumers
co_await ag_for_each(async_range(5), [](int v) { use(v); });
co_await ag_for_each(async_range(5), [](int v) -> task<void> { co_await log(v); });
auto vec  = co_await ag_collect(async_range(5));
auto mapped  = co_await ag_map(async_range(5), [](int v) { return v * 2; });
auto filtered = co_await ag_filter(async_range(10), [](int v) { return v % 2 == 0; });
auto sum  = co_await ag_reduce(async_range(5), 0, [](int acc, int v) { return acc + v; });

// Sync helpers
auto vec = collect_to_vector(fibonacci(10));
auto gen = from_range(my_vector);
auto gen = range(0, 100);
auto gen = iota(0);  // infinite
```

---

### Async stream — functional pipeline (`stream.h`)

```cpp
// Sources
auto s = async_stream<int>::from_vector({1,2,3});
auto s = async_stream<int>::from_channel(ch);              // ch must outlive s
auto s = async_stream<int>::from_channel_shared(ch_ptr);   // lifetime-safe
auto s = async_stream<int>::range(0, 100);
auto s = interval(100ms);                                   // stream<size_t> ticks

// Transforms (lazy, chainable)
auto s2 = s.map([](int v)    { return v * 2; })
           .filter([](int v) { return v > 5; })
           .take(10)
           .skip(2);

// Terminal consumers — safe on temporaries (stream owned in coroutine frame)
co_await stream.for_each([](int v) { use(v); });           // sync callback
co_await stream.for_each([](int v) -> task<void> { co_await log(v); }); // async
auto vec  = co_await stream.collect();
auto opt  = co_await stream.first();
auto n    = co_await stream.count();
auto sum  = co_await stream.reduce([](int a, int b){ return a+b; }, 0);
bool any  = co_await stream.any([](int v)  { return v > 5; });
bool all  = co_await stream.all([](int v)  { return v > 0; });
auto opt  = co_await stream.find([](int v) { return v == 42; });
co_await stream.drain_to(output_channel);

// Merging
auto merged = merge_streams({s1, s2, s3});
auto zipped = zip(s_a, s_b);               // stream<pair<A,B>>
```

---

### Retry (`retry.h`)

```cpp
retry_policy policy{
    .max_attempts = 5,
    .base_delay   = 100ms,
    .max_delay    = 30s,
    .strategy     = backoff_strategy::exponential_jitter,
    .is_retryable = [](const std::exception& e) { return is_transient(e); },
    .on_retry     = [](size_t n, const auto& e) { log_retry(n, e); }
};

// One-shot retry
auto result = co_await with_retry([]() -> task<int> { co_return co_await fetch(); }, policy);

// With success predicate
auto result = co_await with_retry_until(
    []() -> task<std::string> { co_return co_await get_status(); },
    [](const std::string& s)  { return s == "ready"; },
    policy);

// Wrap a callable to be auto-retried whenever called
auto retryable = make_retryable([]() -> task<int> { co_return co_await fetch(); }, policy);
auto result = co_await retryable();

// Predefined policies
auto p = transient_network_policy();
auto p = idempotent_policy();
```

---

## Debug Macros

| Macro | Effect |
|-------|--------|
| `QB_DEBUG_COROUTINES` | Trace `initial_suspend`, `final_suspend`, `promise_destroyed` in `task<T>` |
| `QB_DEBUG_SCOPE=1` | Trace `coroutine_scope` spawn, join, task completion |
| `QB_DEBUG_CORO_LIFECYCLE=1` | Trace `CoroutineScheduler` spawn, resume, destroy |
| `QB_DEBUG_AGEN=1` | Trace `async_generator` yield/next/suspend flow |

---

## Test Suite Summary

| Suite | Tests | File |
|-------|-------|------|
| coroutine-stream | 23 | `test-coroutine-stream.cpp` |
| coroutine-retry | 9 | `test-coroutine-retry.cpp` |
| coroutine-generator | 23 | `test-coroutine-generator.cpp` |
| coroutine-scope | 20 | `test-coroutine-scope.cpp` |
| coroutine-channel | 21 | `test-coroutine-channel.cpp` |
| coroutine-shared-task | 10 | `test-coroutine-shared-task.cpp` |
| coroutine-sync | 21 | `test-coroutine-sync.cpp` |
| coroutine-combinators | 20 | `test-coroutine-combinators.cpp` |
| coroutine-cancellation | 17 | `test-coroutine-cancellation.cpp` |
| **Total** | **164** | |

---

## Maintenance Checklist

When adding new primitives:

- [ ] All coroutine state in `shared_ptr<state_t>` if multiple coroutines reference it
- [ ] No `std::mutex` / `std::atomic` for within-scheduler state
- [ ] Callable parameters taken **by value** across suspension points
- [ ] Terminal coroutine methods delegate to static helpers taking args by value
- [ ] Symmetric transfer in `await_suspend` returning `coroutine_handle<>` (not `void`)
- [ ] `await_resume()` called correctly after destruction chain (no UAF)
- [ ] Tests cover: normal path, exception, early cancel, temporary lambda
