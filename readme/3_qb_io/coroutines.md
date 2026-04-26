@page qb_io_coroutines_md QB-IO: C++23 Coroutines — Structured Async Programming
@brief A complete guide to `qb-io`'s C++23 coroutine support: tasks, awaiters, combinators, channels, structured concurrency, generators, async streams, and safe actor integration.

# QB-IO: C++23 Coroutines — Structured Async Programming

`qb-io` provides first-class support for **C++23 coroutines**, enabling you to write
asynchronous code that reads like straight-line sequential code — no callbacks, no state
machines, no pyramid of doom.  The coroutine layer sits directly on top of the
`listener`/libev event loop and inherits all of its single-threaded safety properties.

> **Requires** a compiler with `__cpp_impl_coroutine` support (GCC ≥ 12, Clang ≥ 14, MSVC 2022).
> All coroutine headers are automatically included via `#include <qb/io/async/coroutine.h>`
> or the umbrella `#include <qb/io/async.h>`.

---

## 1. Architecture Overview

```
qb::io::async::listener   (one per thread — owns the libev loop)
    └── CoroutineScheduler  (thread_local, intrusive ready-queue)
            ├── spawn(task<void>&&)      — fire-and-forget
            ├── spawn(Callable)          — lambda-safe overload
            ├── schedule_resume(handle)  — continuation wakeup after co_await
            └── run_ready()              — drains the queue after each ev_run tick
```

**Key properties of this model:**

| Property | Value |
|---|---|
| Scheduler per thread | 1 (thread_local) |
| Concurrency model | Cooperative (single-threaded) |
| Interleaving point | `co_await` only |
| Mutexes / atomics needed | ❌ None (within one thread) |
| Cross-thread communication | Via qb actor events |

Because coroutines on a thread share a single scheduler and a single event loop,
they are **never concurrent**.  Mutual exclusion between two coroutines is a natural
consequence of the model: only one runs at a time, between `co_await` points.

---

## 2. Quick Start

```cpp
#include <qb/io/async.h>          // includes coroutine.h transitively
#include <iostream>

using namespace qb::io::async;
using namespace std::chrono_literals;

task<int> fetch_value() {
    co_await sleep(100ms);        // suspends — event loop runs freely
    co_return 42;
}

task<void> root() {
    int result = co_await fetch_value();
    std::cout << "Got: " << result << "\n";    // prints "Got: 42"
}

int main() {
    qb::io::async::init();                     // standalone: init the listener
    coro_scheduler().spawn(root());
    run();                                     // drives listener + scheduler
}
```

---

## 3. Core Types

### 3.1 `task<T>` — The Primary Return Type

(`qb/io/async/coroutine/task.h`)

```cpp
task<int>  async_add(int a, int b) {
    co_await sleep(0ms);     // yield once, still same thread
    co_return a + b;
}
```

| Property | Detail |
|---|---|
| Return types | `task<void>`, `task<T>` for any move-constructible `T` |
| Initial suspend | **`suspend_always`** — task is lazy until spawned or awaited |
| Move-only | Yes — `task<T>` cannot be copied |
| Exception propagation | Stored in promise, re-thrown at the awaiting `co_await` site |
| Symmetric transfer | `await_suspend` returns `coroutine_handle<>` — O(1) stack depth |

**Awaiting a task from another coroutine:**

```cpp
task<void> caller() {
    int v = co_await async_add(3, 4);   // suspends caller until async_add finishes
    std::cout << v << "\n";             // 7
}
```

### 3.2 `shared_task<T>` — Multi-Consumer Result

(`qb/io/async/coroutine/shared_task.h`)

`shared_task<T>` allows **multiple coroutines to `co_await` the same result**:

```cpp
shared_task<std::string> fetch_config() {
    co_await sleep(50ms);
    co_return "config_value";
}

task<void> consumer_a(shared_task<std::string> t) {
    auto val = co_await t;   // waits for the same computation
}

task<void> consumer_b(shared_task<std::string> t) {
    auto val = co_await t;   // same shared result, no duplicate work
}
```

---

## 4. The Scheduler: `CoroutineScheduler`

(`qb/io/async/coroutine/scheduler.h`)

The scheduler is accessible via the free function `coro_scheduler()`:

```cpp
// Fire-and-forget — caller does not wait for the task
coro_scheduler().spawn(std::move(my_task));

// Lambda-safe overload (preferred for temporary lambdas)
coro_scheduler().spawn([capture]() -> task<void> {
    co_await do_work(capture);
});

// Introspection
std::size_t n   = coro_scheduler().active_count();  // in-flight + ready queue
std::size_t q   = coro_scheduler().pending_count(); // ready queue only
bool        has = coro_scheduler().has_ready();
```

> **Never pass a `task<T>` by copy** — `task<T>` is move-only.
> `coro_scheduler().spawn(t)` is a compile error; use `spawn(std::move(t))`.

---

## 5. Awaiters

(`qb/io/async/coroutine/awaiter.h` and `utils.h`)

Awaiters are the bridge between coroutines and libev events:

```cpp
// ── Timers ──────────────────────────────────────────────────────────────
co_await sleep(500ms);                       // suspend for a duration
co_await sleep_until(deadline_time_point);   // suspend until absolute time

// ── I/O readiness ───────────────────────────────────────────────────────
co_await wait_readable(fd);                  // EV_READ
co_await wait_writable(fd);                  // EV_WRITE
co_await wait_for_io(fd, EV_READ | EV_WRITE);

// ── TCP async connect ────────────────────────────────────────────────────
// (requires qb/io/async/tcp/connector.h, included via async.h)
auto sock = co_await qb::io::async::tcp::connect(
    qb::io::uri{"tcp://api.example.com:443"}, 5s);
if (!sock) { /* timeout or error */ }

// ── Bridge callback-style APIs ───────────────────────────────────────────
int result = co_await async_awaiter<int>([](auto cb) {
    legacy_async_op([cb](int r) { cb(r); });  // invokes cb from event handler
});
```

---

## 6. Controlling the Event Loop

```cpp
// Standalone usage
qb::io::async::init();          // install listener on this thread

qb::io::async::run();           // blocking: runs until break_parent() or no watchers
qb::io::async::run_once();      // process one batch, then return
qb::io::async::run_until(flag); // run while flag == true (EVRUN_NOWAIT loop)
qb::io::async::run_for(500ms);  // run for a fixed wall-clock duration
qb::io::async::break_parent();  // request the active run() to exit
```

When using `qb-core`, `qb::Main` drives the loop for each `VirtualCore` automatically.

---

## 7. Combinators

(`qb/io/async/coroutine/combinators.h`)

### `when_all` — Scatter-Gather

```cpp
// Heterogeneous pair — returns std::tuple<A, B>
auto [a, b] = co_await when_all(fetch_int(), fetch_string());

// Homogeneous vector — returns std::vector<T>
std::vector<task<int>> work;
for (int i = 0; i < 8; ++i) work.push_back(compute(i));
std::vector<int> results = co_await when_all(std::move(work));
```

### `when_any` — First Wins

```cpp
// Returns when_any_result { .index, .value }
auto [idx, val] = co_await when_any(fast(), slow(), backup());
std::cout << "Winner: task #" << idx << " with value " << val << "\n";
```

### `race` — First Wins (no value)

```cpp
// Returns as soon as the first task completes; others are abandoned
co_await race(network_task(), local_cache_task());
```

### `coro_with_timeout` — Deadline Wrapper

```cpp
// Returns std::optional<T>; nullopt if the operation timed out
auto result = co_await coro_with_timeout(fetch_data(), 2s);
if (!result) {
    std::cerr << "Timed out!\n";
}
```

---

## 8. Cancellation

(`qb/io/async/coroutine/cancellation.h`)

```cpp
cancellation_token token;

// Cancellable sleep — throws cancelled_error on cancel()
task<void> worker(cancellation_token tok) {
    co_await cancellable_sleep(500ms, tok);
    // continues here only if not cancelled before timeout
}

// Deadline — auto-cancels after duration
task<void> bounded(cancellation_token tok) {
    co_await with_deadline(tok, 200ms, inner_task());
}

// Register a cleanup callback
token.on_cancel([]() { release_resource(); });

// Trigger cancellation (same thread only!)
token.cancel();
```

> ⚠️ **Cross-thread cancellation**: `cancellation_token` has no mutex.  
> Route cancellation requests through the qb actor event system:
> the receiving actor's sync event handler calls `token.cancel()` on its own thread.

---

## 9. Synchronisation Primitives

(`qb/io/async/coroutine/sync.h`)

All primitives suspend the coroutine instead of blocking the OS thread:

```cpp
// ── Semaphore ─────────────────────────────────────────────────────────
semaphore sem(3);                           // permits: 3
co_await sem.acquire();
auto guard = co_await sem.scoped_acquire(); // RAII release on scope exit

// ── Async Mutex ───────────────────────────────────────────────────────
async_mutex mtx;
co_await mtx.lock();
mtx.unlock();
auto lk = co_await mtx.scoped_lock();      // RAII

// ── Read-Write Lock ───────────────────────────────────────────────────
async_rw_lock rw;
{   auto r = co_await rw.read_lock();   /* concurrent reads OK */ }
{   auto w = co_await rw.write_lock();  /* exclusive write */     }

// ── Barrier (reusable rendezvous) ────────────────────────────────────
barrier b(4);
co_await b.arrive_and_wait();              // wait for all 4 coroutines

// ── Async Event (manual-reset broadcast) ─────────────────────────────
async_event ready;
ready.set();                               // wakes all waiters
co_await ready.wait();

async_event gate(/*auto_reset=*/true);     // wakes exactly one waiter
gate.set();

// ── Latch (one-shot countdown) ────────────────────────────────────────
async_latch latch(3);
latch.count_down();                        // decrement (non-suspending)
co_await latch.wait();                     // wait until count == 0

// ── RAII helpers ──────────────────────────────────────────────────────
co_await with_semaphore(sem, []() -> task<void> { co_await work(); });
co_await with_lock(mtx,     []() -> task<void> { co_await work(); });
```

---

## 10. Channels — Coroutine Communication

(`qb/io/async/coroutine/channel.h`)

```cpp
channel<int> ch(/*capacity=*/16);

// Producer
co_await ch.send(42);                         // suspend if full
bool ok = ch.try_send(42);                    // non-blocking

// Consumer
std::optional<int> val = co_await ch.recv();  // nullopt when closed
std::optional<int> val = ch.try_recv();       // non-blocking

// Timed operations
auto opt = co_await recv_for(ch, 200ms);      // timeout → nullopt
bool ok  = co_await send_for(ch, 42, 200ms);  // timeout → false

// Close — wakes all pending recv()s with nullopt
ch.close();

// Select — first ready channel wins
auto [winner_idx, value] = co_await select(ch_a, ch_b, ch_c);
// winner_idx: 0-based index; value: std::any

// Pipeline utilities
co_await transform(input_ch, output_ch, [](int x) { return x * 2; });
co_await filter (input_ch, output_ch, [](int x) { return x % 2 == 0; });
auto [in, out] = make_pipeline<int, int>([](int x) { return x + 1; });
```

---

## 11. Structured Concurrency: `coroutine_scope`

(`qb/io/async/coroutine/scope.h`)

`coroutine_scope` groups coroutines together and manages their lifetime:

```cpp
task<void> structured_work() {
    coroutine_scope scope;

    scope.spawn(worker_a());
    scope.spawn(worker_b());
    // Lambda-safe overload:
    scope.spawn([cap]() -> task<void> { co_await process(cap); });

    co_await scope.join_all();              // wait for all
    co_await scope.join_any();              // wait for first
    co_await scope.join_all_for(2s);        // wait with timeout

    scope.cancel_all();                     // signal cancellation tokens
    scope.rethrow_if_error();               // propagate first exception
}

// RAII helper
co_await with_scope([](coroutine_scope& s) -> task<void> {
    s.spawn(worker());
    co_await s.join_all();
});

// Parallel map (bounded concurrency)
auto results = co_await parallel_map(items, /*max_concurrent=*/4,
    [](Item item) -> task<Result> { co_return process(item); });

// Repeat with a condition
co_await repeat_while(
    []() -> task<bool> { co_return should_continue(); },
    []() -> task<void> { co_await do_iteration(); }
);
```

**Cleanup policies** when the scope object is destroyed: `cancel_all` (default),
`join_all`, `detach`.

---

## 12. Generators

(`qb/io/async/coroutine/generator.h`)

### Synchronous Generator (`generator<T>`)

Uses `co_yield`; no `co_await` inside.  Supports range-for:

```cpp
generator<int> fibonacci(int n) {
    int a = 0, b = 1;
    for (int i = 0; i < n; ++i) {
        co_yield a;
        std::tie(a, b) = std::pair{b, a + b};
    }
}

for (int v : fibonacci(10)) {
    std::cout << v << ' ';
}

// Helpers
auto vec = collect_to_vector(fibonacci(10));
auto gen = from_range(my_std_vector);
auto gen = range(0, 100);               // [0, 100)
auto gen = iota(0);                     // infinite: 0, 1, 2, …
```

### Asynchronous Generator (`async_generator<T>`)

Combines `co_yield` with `co_await`:

```cpp
async_generator<std::string> read_lines(std::string path) {
    /* open file … */
    while (has_more_data()) {
        co_await sleep(0ms);         // yield to event loop
        co_yield next_line();
    }
}

// Consumers
co_await ag_for_each(read_lines("log.txt"), [](std::string line) {
    process(line);
});
co_await ag_for_each(read_lines("log.txt"), [](std::string line) -> task<void> {
    co_await store(line);            // async callback allowed
});
auto all = co_await ag_collect(read_lines("log.txt"));   // std::vector<std::string>
auto mapped   = co_await ag_map(read_lines("f"), [](auto s){ return s.size(); });
auto filtered = co_await ag_filter(read_lines("f"), [](auto& s){ return !s.empty(); });
auto total    = co_await ag_reduce(read_lines("f"), 0UZ, [](auto acc, auto& s){ return acc + s.size(); });
```

---

## 13. Async Streams — Functional Pipelines

(`qb/io/async/coroutine/stream.h`)

`async_stream<T>` brings lazy, composable, functional-style transformations to async data:

```cpp
// ── Sources ──────────────────────────────────────────────────────────
auto s = async_stream<int>::from_vector({1, 2, 3, 4, 5});
auto s = async_stream<int>::from_channel(ch);           // ch must outlive stream
auto s = async_stream<int>::from_channel_shared(ch_ptr);// safe shared ownership
auto s = async_stream<int>::range(1, 101);              // [1, 100]
auto s = interval(100ms);                               // stream<size_t>: 0,1,2,…

// ── Lazy transforms (chainable) ───────────────────────────────────────
auto pipeline = s
    .map([](int v)    { return v * v; })
    .filter([](int v) { return v % 2 == 0; })
    .take(5)
    .skip(1);

// ── Terminal consumers (co_await required) ────────────────────────────
co_await pipeline.for_each([](int v) { use(v); });               // sync
co_await pipeline.for_each([](int v) -> task<void> {             // async
    co_await log(v);
});
auto vec  = co_await pipeline.collect();
auto opt  = co_await pipeline.first();
auto n    = co_await pipeline.count();
auto sum  = co_await pipeline.reduce([](int a, int b){ return a+b; }, 0);
bool any  = co_await pipeline.any([](int v){ return v > 10; });
bool all  = co_await pipeline.all([](int v){ return v > 0; });
auto opt2 = co_await pipeline.find([](int v){ return v == 36; });
co_await pipeline.drain_to(output_channel);

// ── Merging ──────────────────────────────────────────────────────────
auto merged = merge_streams({stream_a, stream_b, stream_c});
auto zipped = zip(stream_of_ints, stream_of_strings); // stream<pair<int,string>>
```

---

## 14. Retry Policies

(`qb/io/async/coroutine/retry.h`)

```cpp
retry_policy policy {
    .max_attempts = 5,
    .base_delay   = 100ms,
    .max_delay    = 30s,
    .strategy     = backoff_strategy::exponential_jitter,
    .is_retryable = [](const std::exception& e) { return is_transient(e); },
    .on_retry     = [](std::size_t n, const auto& e) { log_retry(n, e); }
};

// One-shot with retry
auto result = co_await with_retry(
    []() -> task<int> { co_return co_await fetch(); }, policy);

// Retry until a success predicate is satisfied
auto status = co_await with_retry_until(
    []() -> task<std::string>   { co_return co_await get_status(); },
    [](const std::string& s)    { return s == "ready"; },
    policy);

// Wrap a callable for transparent auto-retry
auto retryable = make_retryable([]() -> task<int> { co_return co_await fetch(); }, policy);
auto result    = co_await retryable();

// Pre-defined policies
auto p = transient_network_policy();  // 3 attempts, exponential back-off
auto p = idempotent_policy();         // 5 attempts, linear back-off
```

---

## 15. Safe Integration with `qb::Actor`

### The Problem: Coroutines Break Actor Safety

```cpp
// ❌ WRONG — handler suspends and other actors can modify _buffer meanwhile
class BadActor : public qb::Actor {
    std::vector<Data> _buffer;

    task<void> on(Request& req) {      // async handler = DANGER
        auto data = co_await fetch(req.key);   // suspension point
        _buffer.push_back(data);               // _buffer may have been modified!
    }
};
```

### The Solution: Isolated Coroutines via `spawn_async`

```cpp
// ✅ CORRECT — sync handler spawns an isolated coroutine
class GoodActor : public qb::Actor {
    std::vector<Data> _buffer;

    void on(Request& req) {              // synchronous handler = SAFE
        auto key    = req.key;           // capture by VALUE
        auto sender = req.sender;

        spawn_async([this, key, sender](auto ctx) -> task<void> {
            auto data = co_await fetch(key);
            ctx.push<Result>(ctx.id(), sender, data);  // communicate via events
        });
    }

    void on(Result& ev) {                // synchronous handler = SAFE again
        _buffer.push_back(ev.data);      // exclusive access guaranteed
    }
};
```

**Rules for Actor + Coroutine integration:**

| Rule | Reason |
|---|---|
| Event handlers are `void on(Event&)` | A `task<void> on(Event&)` handler breaks the actor dispatch model |
| Use `spawn_async()` for coroutine work | Isolates coroutine from actor state |
| Capture by **value** inside lambdas | References become dangling after the first `co_await` |
| Communicate via events (`ctx.push<>`) | Maintains Actor Model message-passing semantics |
| Process results in sync handlers | Ensures exclusive access to actor state |

---

## 16. Lambda Capture Safety (Most Common Pitfall)

```cpp
// ❌ WRONG — temporary lambda destroyed before coroutine resumes
auto t = [&data]() -> task<void> {
    co_await sleep(100ms);
    use(data);        // DANGLING REFERENCE — lambda is gone
}();

// ✅ CORRECT — store lambda in a variable so it outlives the invocation
auto fn = [&data]() -> task<void> {
    co_await sleep(100ms);
    use(data);
};
coro_scheduler().spawn(fn());    // fn is alive throughout

// ✅ BEST — pass the lambda directly to spawn() (new overload owns it)
coro_scheduler().spawn([data_copy = data]() -> task<void> {
    co_await sleep(100ms);
    use(data_copy);              // copy owned by the coroutine frame
});

// ❌ WRONG — loop variable captured by reference
for (int i = 0; i < 5; ++i) {
    tasks.push_back([&i]() -> task<int> {
        co_await sleep(10ms);
        co_return i * 10;        // UNDEFINED: i out of scope
    }());
}

// ✅ CORRECT — pass loop variable as parameter (copied into frame)
auto worker = [](int id) -> task<int> {
    co_await sleep(10ms);
    co_return id * 10;
};
for (int i = 0; i < 5; ++i) {
    tasks.push_back(worker(i));  // i is copied into the argument
}
```

---

## 17. Debug Macros

| Macro | What it traces |
|---|---|
| `QB_DEBUG_COROUTINES=1` | `initial_suspend`, `final_suspend`, `promise_destroyed` in `task<T>` |
| `QB_DEBUG_SCOPE=1` | `coroutine_scope` spawn / join / task completion |
| `QB_DEBUG_CORO_LIFECYCLE=1` | `CoroutineScheduler` spawn, resume, destroy |
| `QB_DEBUG_AGEN=1` | `async_generator` yield / next / suspend flow |

Define any of these as a compiler flag (`-DQB_DEBUG_CORO_LIFECYCLE=1`) to enable the
corresponding trace output to `stderr`.

---

## 18. Complete File Reference

| Header | Key Exports |
|---|---|
| `coroutine/task.h` | `task<T>` — primary coroutine return type |
| `coroutine/shared_task.h` | `shared_task<T>` — multi-consumer shared result |
| `coroutine/scheduler.h` | `CoroutineScheduler`, `coro_scheduler()`, `schedule_via_current()` |
| `coroutine/awaiter.h` | `timer_awaiter`, `socket_awaiter`, `async_awaiter<T>` |
| `coroutine/utils.h` | `sleep()`, `wait_readable()`, `wait_writable()`, `run_for()` |
| `coroutine/combinators.h` | `when_all()`, `when_any()`, `race()`, `coro_with_timeout()` |
| `coroutine/cancellation.h` | `cancellation_token`, `cancellable_sleep()`, `with_deadline()` |
| `coroutine/sync.h` | `semaphore`, `async_mutex`, `async_rw_lock`, `barrier`, `async_event`, `async_latch` |
| `coroutine/channel.h` | `channel<T>`, `select()`, `recv_for()`, `send_for()`, `transform()`, `filter()` |
| `coroutine/scope.h` | `coroutine_scope`, `with_scope()`, `parallel_map()`, `repeat_while()` |
| `coroutine/generator.h` | `generator<T>`, `async_generator<T>`, `ag_for_each()`, `ag_collect()`, … |
| `coroutine/stream.h` | `async_stream<T>`, `interval()`, `merge_streams()`, `zip()` |
| `coroutine/retry.h` | `retry_policy`, `with_retry()`, `with_retry_until()`, `make_retryable()` |
| `coroutine/mixin.h` | `coro_mixin<Derived>` — CRTP helper for actor classes |
| `coroutine.h` | Umbrella include for all of the above |

---

**(Next:** [QB-IO: Transports](./transports.md) | [QB-IO: Protocols](./protocols.md) | [Reference: Async Invariants](../7_reference/io_invariants.md))**
