# Migration guide

> **Audience:** Adopter · **Status:** stable · **Verified-against:** qb 3.0.0 (C++20 default, C++23 supported)

Move an existing codebase to qb: from hand-rolled `std::thread` + locked queues to actors, and from the pre-2.0 `qb::Timestamp`/`qb::Duration` time types to the `qb::duration`/`qb::mono_time`/`qb::wall_time` chrono model.

**Prerequisites:** [Getting started](./getting_started.md) — **See also:** [The actor model](../2_core_concepts/actor_model.md), [Inter-actor messaging](../4_qb_core/messaging.md), [The threading model](../2_core_concepts/threading_model.md), [Time vocabulary](../0_foundations/time.md)

## Summary

This guide covers the migrations that adopters hit most often.

1. **Threads to actors.** You have shared state guarded by a mutex, one or more worker threads, and a `std::queue` (plus condition variable) carrying work between them. The actor model replaces all three: state is owned by one actor, work is an event, and the queue and locks disappear. This section maps each primitive to its actor-model equivalent and walks one worker-pool example through the rewrite.

2. **The pre-2.0 time types to the canonical chrono model.** qb 2.0 retired `qb::Timestamp`, `qb::Duration`, and `qb::TimePoint` in favor of three `std::chrono` aliases: `qb::duration`, `qb::mono_time`, and `qb::wall_time`. This section gives a concrete old-to-new mapping so call sites compile against the current API.

3. **The synchronous `onInit()` to the async-init APIs.** `onInit()` is a coroutine, and `addRefActor<T>()` returns a phase-aware `ActorHandle<T>` instead of a raw pointer.

4. **`Event::id_type` in 3.0.** It stopped depending on `NDEBUG`, so a Debug consumer and a Release `libqb-core` finally agree on the event header. Two breaks, with different blast radii: the `id_type` return type changes **in Debug only** (a Release-only CI stays green), while `qb::type_id<T>()` now requires a complete type **in every build mode**, which breaks the common `ServiceActor<struct MyTag>` spelling everywhere. Read this one even if you never named `id_type`.

5. **The remaining 3.0 source breaks.** Nine mechanical ones, each a compile or configure error with a fixed edit: the qbm include prefix, nlohmann no longer being bundled, the retired `.tpp`/`.inl` headers, `<cube.h>`, the vendored libev's rename to `qev`, `std::to_string(uuids::uuid)`, `SO_NOSIGPIPE` on Linux, the unprefixed socket macros, and qbm-redis `client_kill()`. Part 5 lists them in descending order of how many call sites each touches.

The first three are not all-or-nothing. An actor can call into existing synchronous code, and the time aliases are plain `std::chrono` types, so a partial port still compiles and runs. The fourth and fifth are hard 3.0 breaks; they are small, and every one of them is a compile or configure error rather than a silent one.

## Part 1 — From threads and locked queues to actors

### Concepts

The actor model removes the three building blocks of a manual threading design and replaces each with one framework concept.

- A **shared object behind a mutex** becomes an **actor** that owns the state outright. Because exactly one `VirtualCore` thread runs a given actor's handlers, and it runs them one at a time, there is no concurrent access to guard. The mutex has no counterpart — there is nothing to lock.
- A **`std::queue` plus condition variable** becomes the actor's **mailbox**. You do not manage it. `push<Event>(dest, …)` enqueues a message; the engine delivers it to the destination's `on(const Event&)` handler. Cross-core delivery rides a lock-free MPSC queue with no code change.
- A **worker thread** becomes a **`VirtualCore`**. You do not spawn it. `qb::Main` starts one worker thread per core you placed actors on; `addActor<T>(coreId, …)` decides which thread runs an actor.

The behavioral contract that makes this safe: an actor handler runs to completion on its core's event-loop thread before the next event is delivered, so a handler must never block. Long or blocking work is offloaded — see the pitfalls below.

For the full model see [The actor model](../2_core_concepts/actor_model.md); for delivery semantics see [Inter-actor messaging](../4_qb_core/messaging.md); for how cores map to threads see [The threading model](../2_core_concepts/threading_model.md).

### Mapping table

| Manual threading construct | qb actor-model equivalent | Notes |
|---|---|---|
| `std::mutex` / `std::lock_guard` guarding an object | An actor owning that object | One core runs an actor's handlers serially; there is no concurrent access, so no lock. |
| `std::queue<Work>` + `std::condition_variable` | The actor mailbox + `push<Event>` | The engine owns the queue and wakes the consumer; you send events, not work items. |
| `std::thread` / a thread pool | `qb::VirtualCore` worker threads | `qb::Main` spawns one thread per used core; `addActor<T>(coreId, …)` places actors. |
| `worker.join()` | `engine.join()` | Blocks the calling thread until every actor has terminated. |
| `std::atomic<bool> running` flag, then `notify` to stop | `push<qb::KillEvent>(id)` / `kill()` | The default `KillEvent` handler terminates the actor gracefully after the current handler. |
| `std::future` / `std::promise` for a result | A reply event back to the requester | The handler answers with `push<ResultEvent>(requester, …)` or `reply(event)`. |
| `std::thread::hardware_concurrency()` fan-out | Actors placed across `CoreId`s | Distribute by passing different `CoreId` values to `addActor`. |
| Polling loop / `sleep_for` between iterations | `qb::ICallback::on(qb::LoopEvent const&)` or `qb::io::async::callback` | A per-loop tick or a one-shot non-blocking timer on the same core. |
| Manual `errno` / return-code propagation across threads | An error event, or an unhandled exception caught by the core | See [Error handling](./error_handling.md). |

`push` and `send` differ: `push<_Event>(dest, …)` returns a reference to the queued event and guarantees ordered delivery from one source to one destination; `send<_Event>(dest, …)` is unordered and fire-and-forget, and is conventionally reserved for trivially-destructible events — a rule the compiler holds you to only for `qb::EventQOS0`-derived ones. Prefer `push`. _(`push` `qb/src/qb/core/Actor.h:826-831,877`; `send` `:886-890`, `:900`.)_

### Before: a hand-rolled worker

A common pattern: a producer thread feeds work items through a locked queue to a worker thread, which mutates shared state.

```cpp
// before.cpp — manual threads, queue, mutex, condition variable
#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>

struct Job {
    int id;
    int amount;
};

class Worker {
    std::queue<Job>         _jobs;
    std::mutex              _m;
    std::condition_variable _cv;
    bool                    _stop = false;
    long                    _total = 0;   // shared state, guarded by _m

public:
    void submit(Job j) {
        {
            std::lock_guard<std::mutex> lock(_m);
            _jobs.push(j);
        }
        _cv.notify_one();
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(_m);
            _stop = true;
        }
        _cv.notify_one();
    }

    void run() {
        for (;;) {
            std::unique_lock<std::mutex> lock(_m);
            _cv.wait(lock, [&] { return _stop || !_jobs.empty(); });
            if (_stop && _jobs.empty())
                return;
            Job j = _jobs.front();
            _jobs.pop();
            lock.unlock();           // do the work outside the lock
            _total += j.amount;      // ... but _total still needs the lock
        }
    }
};
```

### After: the same logic as actors

The queue, the mutex, the condition variable, and the explicit thread all disappear. A `Job` becomes an event; `_total` is private actor state touched only by the worker's own handler.

```cpp
// after.cpp — actors
// src: examples/01-actors/01-hello-actor.cpp (structure),
//      qb/readme/6_guides/getting_started.md (idioms)
#include <qb/actor.h>   // qb::Actor, qb::ActorId
#include <qb/event.h>   // qb::Event
#include <qb/io.h>      // qb::io::cout
#include <qb/main.h>    // qb::Main

// A job is an event.
struct JobEvent : qb::Event {
    int id;
    int amount;
    JobEvent(int i, int a) : id(i), amount(a) {}
};

// The worker owns the state that used to sit behind the mutex.
class WorkerActor : public qb::Actor {
    long _total = 0;   // no lock: only this actor's handlers touch it

public:
    qb::io::async::task<bool> onInit() final {
        registerEvent<JobEvent>(*this);   // subscribe; KillEvent is automatic
        co_return true;
    }

    void on(const JobEvent &job) {
        _total += job.amount;             // runs serially on this core
        qb::io::cout() << "job " << job.id << " -> total " << _total << '\n';
    }
};

// The producer sends jobs, then asks the worker to stop.
class ProducerActor : public qb::Actor {
    qb::ActorId _worker;

public:
    explicit ProducerActor(qb::ActorId worker) : _worker(worker) {}

    qb::io::async::task<bool> onInit() final {
        for (int i = 0; i < 3; ++i)
            push<JobEvent>(_worker, i, (i + 1) * 10);   // enqueue, ordered
        push<qb::KillEvent>(_worker);                   // graceful stop
        kill();                                         // stop self
        co_return true;
    }
};

int main() {
    qb::Main engine;

    qb::ActorId worker = engine.addActor<WorkerActor>(0);
    if (!worker.is_valid())
        return 1;
    engine.addActor<ProducerActor>(0, worker);

    engine.start();                 // spawns one thread per used core
    engine.join();                  // blocks until both actors terminate
    return engine.hasError() ? 1 : 0;
}
```

Notes on the translation:

- `submit(Job)` became `push<JobEvent>(worker, …)`. The engine owns the queue; there is no `notify`.
- `_total` lost its mutex. Only `WorkerActor::on(const JobEvent&)` reads or writes it, and that handler runs serially on one core, so the data race the lock prevented cannot occur.
- `stop()` and the `_stop` flag became `push<qb::KillEvent>(worker)`. Every actor is auto-subscribed to `qb::KillEvent` at construction, and the default handler calls `kill()`. _(`qb/src/qb/core/Actor.cpp:120,170`; [Getting started](./getting_started.md).)_
- `run()`, the loop, and `_cv.wait` are gone. The event loop inside the `VirtualCore` is the loop.
- `std::thread` and `join()` became `engine.start()` and `engine.join()`.

### Scaling out

To run the worker on a different core than the producer, pass a different `CoreId` to `addActor`:

```cpp
qb::ActorId worker = engine.addActor<WorkerActor>(1);     // core 1
engine.addActor<ProducerActor>(0, worker);                // core 0
```

Cross-core `push` is delivered over a lock-free queue; the code above is unchanged. See [The threading model](../2_core_concepts/threading_model.md).

### Pitfalls

- **Do not block inside a handler.** A handler holds its core's event-loop thread until it returns; blocking stalls every actor on that core. Wrap blocking or long-running work (synchronous file I/O, a slow library call) in `qb::io::async::callback` so it does not freeze the loop, or restructure it as events. See [Asynchronous operations inside actors](../5_core_io_integration/async_in_actors.md). _(`qb/src/qb/core/ICallback.h:16`.)_
- **Do not share an actor's state with other threads.** The no-lock guarantee holds only because one core touches the state. Reintroducing a raw pointer, a `std::shared_ptr` to mutable data, or a global shared with non-actor code reintroduces the data race. Communicate by sending events.
- **`addActor` can fail — and *how* depends on the path.** The pre-start `Main::addActor<T>(coreId, …)` overload does **not** run `onInit()`; it returns `qb::ActorId::NotFound` (the default-constructed, invalid ID) only when the per-core ID pool is exhausted or a duplicate service `Tag` is registered. An `onInit()` that returns `false` for a pre-start actor is detected at startup: the engine flags that core `VirtualCore::Error::BadActorInit`, the core fails to start, and you observe it via `hasError()` after the run — not through the returned ID. The runtime `addRefActor` / `addActor(…)` path is the one that *does* run `onInit()` at add time and returns an invalid ID when it returns `false`. Guard every returned ID with `is_valid()`, and gate startup with `hasError()`. _(`qb/src/qb/core/Main.h:737-763`, `qb/src/qb/core/Main.h:776-780`; `qb/src/qb/core/Main.cpp:344-350`; `qb/src/qb/core/VirtualCore.h:129`; `qb/src/qb/core/VirtualCore.cpp:862-877`; `qb/src/qb/core/VirtualCore.h:947-951`; `qb/src/qb/core/ActorId.h:401,442`; [Error handling](./error_handling.md).)_
- **Add every actor before `start()`.** Actors are constructed on their worker thread when the engine starts; `addActor` must be called beforehand.
- **A periodic task is not a `sleep` loop.** Replace a polling thread with `qb::ICallback` (`on(qb::LoopEvent const&)` runs once per loop iteration) or a one-shot `qb::io::async::callback`, both non-blocking. _(`qb/src/qb/core/ICallback.h:169` is the `on(qb::LoopEvent const&)` hook, `:93` says it runs on every loop iteration, `:16-19` is the non-blocking contract; `class ICallback` is at `:146`.)_

## Part 2 — From the pre-2.0 time types to the chrono model

### Concepts

qb 2.0 replaced the framework-specific time classes with three `std::chrono` aliases. They are the single source of truth across qb and every module; the [time vocabulary](../0_foundations/time.md) page owns their full definition. In brief:

| Alias | Underlying type | Use for |
|---|---|---|
| `qb::duration` | `std::chrono::nanoseconds` | Every timeout, delay, TTL, interval, and latency value in public APIs. |
| `qb::mono_time` | `std::chrono::steady_clock::time_point` | Deadlines, timers, the event-loop "now", latency, RTT — immune to NTP/DST. |
| `qb::wall_time` | `std::chrono::system_clock::time_point` | Dates, expiry, JWT `exp`/`nbf`, TLS validity, logs, wire formats. |

_(`qb/src/qb/system/time.h:90,93,96`.)_

Two design points drive the migration:

- **A `qb::duration` rejects a bare integer.** It accepts any finer-or-equal `std::chrono` literal implicitly (`30s`, `100ms`, `5us`) but a raw `int` does not convert, so a seconds-versus-milliseconds unit confusion cannot compile. Wherever old code passed a number, pass a chrono literal. _(`qb/src/qb/system/time.h:8-11`.)_
- **`mono_time` and `wall_time` are distinct types.** Subtracting a wall instant from a monotonic one does not compile. Pick `mono_time` for anything you measure or schedule against (deadlines, elapsed time, RTT) and `wall_time` for anything tied to the calendar (expiry, logs, wire timestamps). _(`qb/src/qb/system/time.h:18-20`.)_

The literal operators (`30s`, `100ms`, `5us`, …) are pulled into `qb` through `inline namespace qb::time_literals`, so call sites can write them after `#include <qb/system/time.h>` with no extra `using`. _(`qb/src/qb/system/time.h:110-114`.)_

> **Retired tokens — never reintroduce them.** `qb::Timestamp`, `qb::Duration`, and `qb::TimePoint` no longer exist anywhere in the framework; the canonical replacements are defined in `<qb/system/time.h>`. Any reference to them is pre-2.0 code that must be ported. The documentation anti-drift guard rejects these tokens everywhere except the migration, contributing, and changelog surfaces. _(`qb/src/qb/system/time.h:87-96`; `qb/scripts/doc-lint.sh:74` (the token list), `qb/scripts/doc-lint.sh:76-81` (the three allowed surfaces).)_

### Old-to-new mapping table

| Pre-2.0 construct | qb 2.0 replacement | Header / note |
|---|---|---|
| `qb::Duration` (a span) | `qb::duration` (`std::chrono::nanoseconds`) | `<qb/system/time.h>` |
| `qb::Timestamp` / `qb::TimePoint` used as a deadline, timer base, or elapsed-time anchor | `qb::mono_time` (`steady_clock::time_point`) | Monotonic; immune to clock steps. |
| `qb::Timestamp` / `qb::TimePoint` used as a date, expiry, or wire timestamp | `qb::wall_time` (`system_clock::time_point`) | Calendar time. |
| `Timestamp::now()` for a monotonic anchor | `qb::mono_now()` | Returns `qb::mono_time`. |
| `Timestamp::now()` for a wall-clock instant | `qb::wall_now()` | Returns `qb::wall_time`. |
| `Duration::seconds(n)` / `fromSeconds(n)` | `std::chrono::seconds(n)` (or the literal `n` s, e.g. `30s`) | Implicitly converts to `qb::duration`. |
| `Duration::milliseconds(n)` | `std::chrono::milliseconds(n)` / `100ms` | Implicitly converts to `qb::duration`. |
| `Duration::microseconds(n)` | `std::chrono::microseconds(n)` / `5us` | Implicitly converts to `qb::duration`. |
| `someDuration.seconds()` / `.toSeconds()` | `std::chrono::duration_cast<std::chrono::seconds>(d).count()` | Explicit cast; `count()` yields the integer. |
| `someDuration.milliseconds()` | `std::chrono::duration_cast<std::chrono::milliseconds>(d).count()` | |
| `Timestamp::epochSeconds()` on a wall instant | `qb::unix_seconds(tp)` | `int64_t` seconds since the Unix epoch. _(`time.h:122`.)_ |
| `Timestamp::epochMillis()` | `qb::unix_millis(tp)` (also `unix_micros`, `unix_nanos`) | `int64_t` since the Unix epoch. _(`time.h:128`.)_ |
| `Timestamp::fromEpochSeconds(s)` | `qb::wall_from_unix_seconds(s)` | Returns `qb::wall_time`. _(`time.h:146`.)_ |
| `Timestamp::fromEpochMillis(ms)` | `qb::wall_from_unix_millis(ms)` | Returns `qb::wall_time`. _(`time.h:152`.)_ |
| `timestamp.toString(fmt)` / custom date formatting | `qb::format_utc(tp, fmt)` — strftime-compatible, UTC | Empty string on failure. _(`time.h:306`.)_ |
| ISO-8601 string from a timestamp | `qb::to_iso8601(tp)`, returning `"YYYY-MM-DDTHH:MM:SSZ"` | _(`time.h:319`.)_ |
| Parsing a date string | `qb::parse_utc(str, fmt)` / `qb::from_iso8601(str)` | Returns `std::optional<qb::wall_time>` (`nullopt` on error), UTC only. _(`time.h:327,346`.)_ |
| `Duration::zero()` / a "no timeout" sentinel | `qb::duration::zero()` | Inherited from `std::chrono::nanoseconds`. |
| A hand-rolled scope timer | `qb::ScopedTimer` / `qb::LogTimer` | Monotonic; callback receives a `qb::duration`. _(`time.h:718,769`.)_ |
| A raw RDTSC / CPU-counter read | `qb::tsc_ticks()` | Per-core, uncalibrated — micro-benchmark deltas only, **not a clock**. _(`time.h:684`.)_ |

### Before and after

```cpp
// before.cpp — pre-2.0 time types (illustrative; these types no longer exist)
qb::Duration  timeout  = qb::Duration::milliseconds(500);
qb::Timestamp start    = qb::Timestamp::now();
// ... work ...
qb::Duration  elapsed  = qb::Timestamp::now() - start;
long          secs     = elapsed.seconds();
qb::Timestamp deadline = start + qb::Duration::seconds(30);
```

```cpp
// after.cpp — canonical chrono model
// src: qb/src/qb/system/time.h
#include <qb/system/time.h>   // qb::duration, qb::mono_time, qb::mono_now

// REQUIRED for the `500ms` / `30s` suffixes below. The header declares the inline
// namespace qb::time_literals (time.h:112-114), but a chrono literal is only found by
// unqualified lookup once it is brought into scope — without this line the block fails
// with: no matching literal operator for call to 'operator""ms'.
using namespace qb::time_literals;

qb::duration  timeout  = 500ms;                  // bare-int rejected; literal accepted
qb::mono_time start    = qb::mono_now();         // monotonic anchor for elapsed time
// ... work ...
qb::duration  elapsed  = qb::mono_now() - start; // mono - mono -> qb::duration
auto          secs     =
    std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
qb::mono_time deadline = start + 30s;            // schedule against monotonic time
```

A wall-clock example — formatting an expiry for a log line or a wire field:

```cpp
// src: qb/src/qb/system/time.h
#include <qb/system/time.h>
using namespace qb::time_literals;                    // required for the `24h` suffix

qb::wall_time expiry = qb::wall_now() + 24h;          // 24h from now, wall clock
std::string   iso    = qb::to_iso8601(expiry);        // "YYYY-MM-DDTHH:MM:SSZ"
std::int64_t  exp_s  = qb::unix_seconds(expiry);      // for a JWT exp claim, say
```

### Where the types show up in qb APIs

The migration matters because the framework's own surfaces take these types. A few you will meet immediately:

- `CoreInitializer::setLatency(qb::duration latency = qb::duration::zero())` — the maximum the event loop waits when idle; the default `zero()` is the busy-spin, lowest-latency mode (100% CPU on the core), and a positive value lets the idle core park to trade latency for CPU. _(`qb/src/qb/core/Main.h:271-284`.)_
- Socket and async timeouts take `qb::duration` (for example `tcp::socket::connect(qb::io::endpoint const &ep, qb::duration wtimeout)`); a non-positive value is clamped to a single poll, not "wait forever." _(`qb/src/qb/io/tcp/socket.h:155-158`; the clamp is `qb/src/qb/io/system/sys__socket.cpp:752`.)_
- `qb::SpinLock::trylock_for(qb::duration)` and `trylock_until(qb::mono_time)`. _(`qb/src/qb/system/lockfree/spinlock.h:135-136,171-172`.)_

### Pitfalls

- **Do not pass a bare integer to a `qb::duration` parameter.** `setLatency(100)` does not compile by design; write `setLatency(100us)` (or `std::chrono::microseconds(100)`). This is the unit-confusion guard, not a defect. _(`qb/src/qb/system/time.h:8-11`.)_
- **Do not mix the two instant clocks.** You cannot subtract a `qb::wall_time` from a `qb::mono_time`; the compiler rejects it. Measure and schedule with `mono_time`; record dates and expiry with `wall_time`. Converting between them means going through a Unix-epoch scalar (`unix_seconds` / `wall_from_unix_seconds`) and accepting that the wall clock can step. _(`qb/src/qb/system/time.h:18-20`.)_
- **`tsc_ticks()` is not a clock.** It is monotonic per core but uncalibrated and not comparable across cores or to either clock. Use it only for single-thread micro-benchmark deltas. _(`qb/src/qb/system/time.h:680-684`.)_
- **`format_utc`/`parse_utc` are UTC-only.** There is no time-zone database on this toolchain; formatting uses `strftime` and parsing uses `std::get_time` + `timegm`, both in UTC. `format_utc` returns an empty string on failure; `parse_utc` and `from_iso8601` return `std::nullopt`. _(`qb/src/qb/system/time.h:27-30,305-342`.)_
- **`Actor::time()` returns a raw `uint64_t`, not a chrono type.** It is the core's cached epoch-nanosecond count (sourced from `qb::wall_now()`), constant within one handler or `on(qb::LoopEvent const&)` invocation; it is not a `qb::mono_time` or `qb::wall_time`. For a fresh high-precision wall instant use `qb::unix_nanos(qb::wall_now())`. _(`qb/src/qb/core/Actor.h:566-583`.)_

## Part 3 — From the synchronous `onInit()` to the async-init APIs

3.0 makes two **source-incompatible** changes to the actor API — on `main` as well as `develop`,
which have carried the same 3.0.0 since 2026-08-11. Both are mechanical to adopt.

### `onInit()` is now a coroutine

`Actor::onInit()` changed from `bool onInit()` to `qb::io::async::task<bool> onInit()`. Replace the
return type and turn each `return` into `co_return`:

```cpp
// Before (qb 2.0)
bool onInit() override {
    registerEvent<MyEvent>(*this);
    if (!acquire()) return false;
    return true;
}

// After
qb::io::async::task<bool> onInit() override {
    registerEvent<MyEvent>(*this);
    if (!acquire()) co_return false;
    co_return true;
}
```

A purely synchronous init needs nothing more than the signature + `co_return` swap — it still
completes in one step and pays none of the suspended-init machinery. The new power is that `onInit()`
**may now `co_await`** — sleep, `qb::ask` a peer for its configuration, or run a pattern — without
blocking the core:

```cpp
qb::io::async::task<bool> onInit() override {
    registerEvent<Reply>(*this);
    auto cfg = co_await qb::ask(context(), config_service, ConfigReq{}, 2s);
    _setting = cfg.response;
    co_return true;
}
```

While a suspended `onInit()` is in flight the actor is **Activating**: inbound unicast business
events are stashed and replayed FIFO once it activates (broadcasts and `KillEvent` still pass
through), bounded by `qb::VirtualCore::activation_deadline_ns` (default 5 s). Use `context()` so a
kill during init unwinds the coroutine cleanly. `co_return false` or an uncaught exception still
fails init and removes the actor. Query `is_active()` (alive **and** activated) where you previously
relied only on `is_alive()`.

### `addRefActor<T>()` returns `ActorHandle<T>`, not `T*`

`addRefActor<T>()` (and the alias `addRefHandle<T>()`) now return a phase-aware
`qb::ActorHandle<T>` (alias `RefActorHandle<T>`) instead of a raw `T*`:

```cpp
// Before
ChildHelper *child = addRefActor<ChildHelper>(args);   // raw, could dangle after kill()
child->doWork();

// After
auto child = addRefActor<ChildHelper>(args);           // qb::ActorHandle<ChildHelper>
push<Task>(child.id(), ...);                            // always safe (stashed if Activating)
if (child.ready()) child->doWork();                    // direct call only when active
// async-init child: if (co_await child.ready_async(context())) child->doWork();
```

The handle never dangles: `get()` / `operator->` resolve the live actor on demand and return
`nullptr` while it is Activating, after a failed init, or once it died. Code that already used
`addRefHandle<T>()` + `RefActorHandle<T>` keeps compiling unchanged.

## Part 4 — `Event::id_type` is now one type in every build mode

**Who is affected:** two disjoint groups, and they break differently — measured, not assumed:

| what you wrote | 2.6 Release | 2.6 Debug | **3.0, both modes** |
|---|---|---|---|
| `Event::id_type` / `type_to_id<T>()` used as a `const char *` | already broken | compiled | **compile error** |
| `qb::type_id<Tag>()`, `ServiceActor<Tag>`, `getServiceId<Tag>()` with an **incomplete** `Tag` | compiled | compiled | **compile error** |

The first row is **Debug-only, and a Release-only CI stays green** — that is why it is written down
here rather than left to a build to find. The second row breaks in *every* mode, so any build will
catch it.

### What changed

`qb::Event::id_type` used to be selected by `NDEBUG`:

| | 2.6 Release (`NDEBUG`) | 2.6 Debug | **3.0, both modes** |
|---|---|---|---|
| `Event::id_type` | `qb::EventId` (`uint16_t`) | `const char *` | **`qb::EventId`** |
| `Event::type_to_id<T>()` returns | `EventId` | `const char *` (`typeid(T).name()`) | **`EventId`** |
| `id` at byte | 6 | 8 | **6** |
| `dest` / `source` at bytes | 8 / 12 | 16 / 20 | **8 / 12** |
| `sizeof(qb::Event)` | 64 | 64 | 64 |

`sizeof(qb::Event)` never moved — `Event` is cache-line aligned — but the *header* did, and events
are relocated across cores with `memcpy`. A consumer whose `NDEBUG` disagreed with the installed
`libqb-core` therefore read `dest` out of the payload and routed to a garbage `ActorId`, with no
diagnostic: it compiled, linked, agreed on `sizeof`, did not crash, and delivered nothing.
`CMAKE_BUILD_TYPE` unset — CMake's default — is one of the configurations that produced this.

### Fixing your code

**1. An id held or printed as a string.**

```cpp
// Before — compiles in Debug only, where id_type was const char *
const char *id = event.getID();
LOG_INFO("event " << id);

// After — the id is a 16-bit integer in every mode; ask for the name separately
const qb::Event::id_type id = event.getID();
LOG_INFO("event " << qb::event_type_name(id) << '#' << id);
```

`qb::event_type_name(id)` reverse-resolves a runtime id, and `qb::Event::type_to_name<T>()` gives
the same string from the type. Both work in every build mode and return the Itanium-mangled
`typeid(T).name()` — exactly what Debug printed before. An id no type owns yields
`"<unregistered>"`. Neither is on a routing path; they are for log lines and assertions.

**2. Your own `#ifdef NDEBUG` mirroring the old split.** Delete it. There is one representation
now, so a branch on `NDEBUG` can only reintroduce the defect:

```cpp
// Before
struct MyKey {
#ifdef NDEBUG
    using id_type = qb::EventId;
#else
    using id_type = const char *;
#endif
};

// After
struct MyKey { using id_type = qb::EventId; };
```

**3. A forward-declared service tag.** `qb::type_id<T>()` now reaches `typeid(T)` in every mode, so
`T` must be **complete**. This one compiled in *both* modes before 3.0 — the Release *and* the Debug
path of the tag lookup went through the counter and never touched `typeid` — so it is a new error
everywhere, not a Debug surprise. The commonly-copied one-liner declares the tag without defining
it and stops compiling with `'typeid' of incomplete type`:

```cpp
// Before — `struct MyTag` here DECLARES MyTag; it is never defined
class Registry : public qb::ServiceActor<struct MyTag> { … };

// After
struct MyTag {};
class Registry : public qb::ServiceActor<MyTag> { … };
```

The same applies to `Actor::getServiceId<Tag>()`, `Actor::registerIndex<Tag>()`,
`Actor::require<T>()`, `Actor::is<T>()`, `qb::require<T>()`, `ActorProxy::getType<T>()` and
`router::*::unsubscribe<T>()` — the paths that only ever *name* a type without constructing or
sizing it. Everything that already constructed `T` (`push`, `send`, `broadcast`, `registerEvent`)
required a complete type before and is unchanged.

### What you get back

Debug builds get *faster*, not slower: the router key stops being a pointer, so
`std::hash<const char *>` — a real call chain at `-O0` — becomes identity hashing. Measured on the
dispatch lookup itself over a 211-type table, the 16-bit key wins 9 rounds out of 9 (median −51 %);
at engine level that was −11 % ns/event on `messaging-api-oneway`. A Debug event also regains the
48 bytes of first-bucket payload capacity that Release always had (it was 40), and Release log
lines gain an event-type name they never had.

## Part 5 — The mechanical 3.0 breaks

The four parts above are ports: you rewrite code to a different shape. This part is the rest of the
3.0 break list — nine items that are pure edits. Every one is a compile error or a configure error,
none is silent, and each has exactly one correct fix. They are ordered by how many call sites they
touch, not by how interesting they are.

| # | What breaks | Who it touches | Fix |
|---|---|---|---|
| 5.1 | `#include <http/…>` → `<qbm/http/…>` (same for `pgsql`, `redis`) | every qbm consumer, every include line | mechanical prefix |
| 5.2 | nlohmann/json is no longer bundled | anyone who used `qb::json` without a system nlohmann | install the package |
| 5.3 | the `.tpp` / `.inl` headers are gone | anyone who included one directly | include the `.h` |
| 5.4 | `<cube.h>` is gone | anyone who used the umbrella | name the entry points |
| 5.5 | the vendored libev is `qev` | anyone who reached the C API or `<ev/…>` | rename the prefix |
| 5.6 | `std::to_string(uuids::uuid)` is gone | anyone who stringified a `qb::uuid` that way | `uuids::to_string` |
| 5.7 | `SO_NOSIGPIPE` is not defined on Linux | Linux code that named the macro | use `MSG_NOSIGNAL` |
| 5.8 | the unprefixed socket macros are off | Winsock-style code (`closesocket`, `SD_SEND`, …) | one option, or the `QB_` names |
| 5.9 | qbm-redis `client_kill()` returns a count | qbm-redis callers of that one command | change the `Reply<>` type |

### 5.1 The qbm include prefix

Every qbm header is reached as `<qbm/<module>/…>` in 3.0. In 2.6.0 the include root was the module's
*parent* directory — which only exists inside the development superproject — so the shipped spelling
was `<http/http.h>`. Module sources now live at `<module>/src/qbm/<module>/`, and `src/` **is** the
include root, so the same string works in the source tree and in an installed prefix
— `qb_package_include_root` (`qb/cmake/qbPackage.cmake:51-55`) decides both, and the module
registration (`qb/cmake/qbFunctions.cmake:957-964`) refuses to configure a module laid out any other
way.

```
t_http.cpp:1:10: fatal error: 'http/http.h' file not found
```

```cpp
// Before                              // After
#include <http/http.h>                 #include <qbm/http/http.h>
#include <http/middleware/cors.h>      #include <qbm/http/middleware/cors.h>
#include <redis/redis.h>               #include <qbm/redis/redis.h>
#include <pgsql/pgsql.h>               #include <qbm/pgsql/pgsql.h>
```

The CMake side does not move: the targets are still `qbm::http`, `qbm::pgsql`, `qbm::redis`, and an
installed module is still found with `find_package(qbm-http CONFIG REQUIRED)`. Installed headers land
under `<prefix>/include/qbm/<module>/`, and `<prefix>/include` now holds exactly `qb` and `qbm`.

### 5.2 nlohmann/json is no longer bundled

qb used to install its own copy of `nlohmann/json.hpp` whenever the build host had none. That copy
declared 3.12.0 while diverging from the tag, so it shared the `json_abi_v3_12_0` inline namespace
with a genuine 3.12.0 over different definitions. 3.0 deletes it: nlohmann is a real dependency,
resolved by `find_package(nlohmann_json 3.11)` with a pinned `FetchContent` fallback.

Nothing changes in your source. `qb::json` is still `nlohmann::json` — qb re-exports the namespace
(`qb/src/qb/json.h:282-283`), so there is no alias to update. What changes is provisioning:

- **A plain build or test run does not care.** With no system copy it fetches
  `QB_NLOHMANN_GIT_TAG` (`qb/cmake/qbConfig.cmake:107`, default `v3.12.0`).
- **An installable build needs a *real* system nlohmann.** A fetched target belongs to no export set,
  so `QB_INSTALL=ON` without one is a deliberate configure-time error
  (`qb/cmake/qbDependencies.cmake:464-472`) that names every way out:

  ```
  CMake Error at cmake/qbConfig.cmake:493 (message):
    [qb] nlohmann_json was not found on the system, so it would be fetched
    (v3.12.0, via QB_USE_SYSTEM_NLOHMANN=AUTO), but QB_INSTALL is ON.
  ```

  Fix it with `brew install nlohmann-json` / `apt install nlohmann-json3-dev`, or point
  `CMAKE_PREFIX_PATH` at a prefix that has one, or build with `QB_INSTALL=OFF`.
- **Consuming an installed qb needs the package too.** The generated `qbConfig.cmake` calls
  `find_dependency(nlohmann_json 3.11)` unconditionally — the call is written into its template,
  `qb/cmake/qbConfig.cmake.in`.

`QB_USE_SYSTEM_NLOHMANN` (`qb/cmake/qbConfig.cmake:173`) is the lever: `AUTO` (default) takes a system
copy when there is one, `ON` requires it, `OFF` always fetches.

### 5.3 The `.tpp` and `.inl` headers are gone

`.h` is now the only header extension in the framework and in every module, and
`qb/scripts/check-header-extensions.py` fails the build if one comes back — it rejects both the files
and any `#include` naming one. Nine files went: qb's `qb/core/Actor.tpp`, `Main.tpp`, `Pipe.tpp`,
`VirtualCore.tpp` and `qb/io/system/sys__inet_compat.inl`; qbm-http's `routing/router.tpp`; qbm-pgsql's
`resultset.inl`, `transaction.inl`, `transaction_coro.inl`.

The four qb `.tpp` files were **installed public headers** in 2.6.0, so this is the one item in Part 5
that a consumer could hit without ever having looked inside qb.

```
t_tpp.cpp:1:10: fatal error: 'qb/core/Actor.tpp' file not found
```

Template bodies moved to the tail of the `.h` that declares them — except `qb::Actor`'s, which moved
to the tail of **`VirtualCore.h`** (`qb/src/qb/core/VirtualCore.h:915-920`). Most of those bodies name
`VirtualCore::` in a nested-name-specifier and need a complete `qb::VirtualCore`, and `VirtualCore.h`
already includes `Actor.h` — so `Actor.h` can never host them. The body has to go where the include
cycle *closes*, which is a position, not a file you get to choose.

```cpp
// Before                                    // After
#include <qb/core/Actor.tpp>                 #include <qb/actor.h>   // or <qb/core/VirtualCore.h>
#include <qb/core/Main.tpp>                  #include <qb/main.h>    // or <qb/core/Main.h>
#include <qb/core/Pipe.tpp>                  #include <qb/core/Pipe.h>
#include <qb/core/VirtualCore.tpp>           #include <qb/core/VirtualCore.h>
#include <qbm/http/routing/router.tpp>       #include <qbm/http/routing/router.h>
#include <qbm/pgsql/resultset.inl>          #include <qbm/pgsql/resultset.h>
```

If you include the umbrella headers (`<qb/actor.h>`, `<qb/main.h>`, `<qb/io.h>`) there is nothing to
edit — they already pull the right files.

### 5.4 `<cube.h>` is gone

`cube.h` sat at the top of qb's installed include root — `<prefix>/include/cube.h`, the last generic
name in there — and did nothing but include `<qb/actor.h>`, `<qb/io.h>` and `<qb/main.h>`. 3.0 removes
it and ships no replacement umbrella.

```
t_cube.cpp:1:10: fatal error: 'cube.h' file not found
```

```cpp
// Before                    // After
#include <cube.h>            #include <qb/actor.h>   // qb-core
                             #include <qb/main.h>
                             #include <qb/io.h>      // qb-io
```

### 5.5 The vendored libev is `qev`

qb's libev fork was re-prefixed so its 58 exported C symbols can no longer collide at link time with a
system libev, or with another library that vendored its own. `ev_*` became `qev_*`, `struct ev_loop`
became `struct qev_loop`, the headers became `qev.h` / `qev++.h`, the CMake target `ev` became `qev`
(`qb::ev` → `qb::qev`), and the archive `libev.a` became `libqev.a`.

The 2.6.0 spelling of the header was `<ev/ev++.h>`, installed from qb's own `modules/ev/`. It is now
`<qb/vendor/qev/qev++.h>`.

**Most consumers need no edit at all.** The C++ namespace is still `ev`
(`qb/src/qb/vendor/qev/qev++.h:33`), qb's watcher types still derive from `ev::io` / `ev::sig` /
`ev::timer`, and `EV_READ` / `EV_WRITE` / `EV_MULTIPLICITY` were deliberately left alone. Two
populations are affected: anyone who included `<ev/ev++.h>` directly, and anyone who called the libev
**C** API through qb's copy.

```
t_ev.cpp:1:10: fatal error: 'ev/ev++.h' file not found

t_qevsym.cpp:2:20: error: use of undeclared identifier 'ev_run'; did you mean 'qev_run'?
```

The compiler's fix-it is right: prefix the C calls with `q` (`ev_run` → `qev_run`,
`qb/src/qb/vendor/qev/qev.h:696`), change the include to `<qb/vendor/qev/qev++.h>`, and change a CMake
`qb::ev` dependency to `qb::qev`. Leave `ev::`, `EV_*` and the libevent-compat `event_*` names as they
are.

### 5.6 `std::to_string(uuids::uuid)` is gone

2.6.0 added a `to_string` overload *inside `namespace std`*, next to the legitimate
`std::hash<uuids::uuid>` specialisation. Adding a declaration to `std` is undefined behavior, so 3.0
removed it. Nothing replaced it in `std`; the library's own function was always there.

```
t_uuid.cpp:5:21: error: no matching function for call to 'to_string'
    5 |     std::string s = std::to_string(id);
```

```cpp
// Before                              // After
std::string s = std::to_string(id);    std::string s = uuids::to_string(id);
                                       // or, unqualified — ADL finds it:
                                       std::string s = to_string(id);
```

`uuids::to_string` is at `qb/src/qb/vendor/uuid/include/uuid.h:547-549`, and `qb::uuid` is
`::uuids::uuid` (`qb/src/qb/uuid.h:45`), so the unqualified call resolves by argument-dependent lookup.
`std::hash<uuids::uuid>` is untouched — a `qb::uuid` still works as a map key.

### 5.7 `SO_NOSIGPIPE` is no longer defined on Linux

`qb/io/config.h` used to do `#define SO_NOSIGPIPE MSG_NOSIGNAL` inside its `__linux__` branch: a socket
*option* name bound to a message *flag* value, in a header that ships in the install tree. Linux has no
such option — `setsockopt(SOL_SOCKET, MSG_NOSIGNAL, …)` returns `-1`/`ENOPROTOOPT`. The define is gone
(`qb/src/qb/io/config.h:363`).

Behavior changes on no platform: both of qb's own call sites already guarded with `!defined(__linux__)`,
and qb applies the option at descriptor acquisition where it is real
(`qb/src/qb/io/system/sys__socket.cpp:111-115`). Only code that *named* the macro on Linux breaks:

```
error: 'SO_NOSIGPIPE' undeclared
```

Use `MSG_NOSIGNAL` per send call, which is what every `qb::io::socket` send/recv entry point already
passes. There is nothing to change if you only use qb's socket API.

### 5.8 The unprefixed socket-portability macros are off by default

`qb/io/config.h` no longer defines the bare Winsock-style spellings — `closesocket`, `ioctlsocket`,
`SD_RECEIVE`, `SD_SEND`, `SD_BOTH`, `FD_TO_SOCKET`, `OPEN_FD_FROM_SOCKET` — because a header in an
install tree must not claim names that generic. The `QB_`-prefixed equivalents are always defined.

```cpp
// Before                    // After
closesocket(fd);             QB_CLOSESOCKET(fd);
ioctlsocket(fd, c, &v);      QB_IOCTLSOCKET(fd, c, &v);
shutdown(fd, SD_SEND);       shutdown(fd, QB_SD_SEND);
```

Better still, do not touch a raw descriptor: a `qb::io::socket` closes itself, and
`socket::close(int shut_how = QB_SD_BOTH)` is the member that does it explicitly.

If you need the old spellings back while you port, configure with `-DQB_LEGACY_SOCKET_MACROS`
(`qb/src/qb/io/config.h:560`) — it restores every one of them, and it is meant as a bridge, not a
setting to keep.

### 5.9 qbm-redis: `client_kill()` returns `Reply<long long>`

`client_kill()` was declared `Reply<status>`. Its `skipme` parameter defaults to `true`, so the wrapper
*always* emitted the filter form of `CLIENT KILL`, and that form replies with an integer count —
which a `Reply<status>` cannot decode. Every call, on every argument combination, failed with
`"STRING or ERROR required for status"`. 3.0 changes the return type to `Reply<long long>`, which is
what the command actually returns.

The SFINAE guard on the callback overload moved with the return type, so an existing caller gets a hard
compile error rather than a silent conversion:

```
t_redis.cpp:4:7: error: no matching member function for call to 'client_kill'
    note: candidate template ignored: requirement
    'std::is_invocable_v<(lambda …), qb::redis::Reply<long long> &&>' was not satisfied
```

```cpp
// Before
Reply<status> r = co_await c.client_kill(addr);
if (r.ok()) { /* killed — how many? unknown */ }
c.client_kill([](Reply<status> &&r) { … }, addr);

// After
Reply<long long> r = co_await c.client_kill(addr);
if (r.ok()) { long long killed = r.result(); }
c.client_kill([](Reply<long long> &&r) { … }, addr);
```

The parameter list is unchanged (`addr`, `id`, `type`, `skipme`). See qbm-redis's own CHANGELOG for the
module's full 3.0 list.

### One rename that is *not* a break

qb 3.0 renamed `LOG_DEBUG` / `LOG_VERB` / `LOG_INFO` / `LOG_WARN` / `LOG_CRIT` to `QB_LOG_*`, because
three of the five collide with POSIX `<syslog.h>`. The unprefixed names are still defined, as
`#ifndef`-guarded aliases, so existing code compiles unchanged. The one behavioral difference: if your
translation unit defines `LOG_INFO` before including qb, yours now wins where qb's used to. Prefer the
`QB_LOG_*` spellings in new code.

## See also

- [Getting started](./getting_started.md) — install qb and build your first actor program.
- [The actor model](../2_core_concepts/actor_model.md) — isolation, the event loop, and one-at-a-time handler execution.
- [Inter-actor messaging](../4_qb_core/messaging.md) — `push`, `send`, `to`, `reply`, `forward`, and `broadcast`.
- [The threading model](../2_core_concepts/threading_model.md) — how `CoreId`s map to worker threads and how cross-core delivery works.
- [Asynchronous operations inside actors](../5_core_io_integration/async_in_actors.md) — offloading blocking work without stalling the loop.
- [Time vocabulary](../0_foundations/time.md) — the canonical definition of `qb::duration`, `qb::mono_time`, and `qb::wall_time`.
- [Error handling](./error_handling.md) — supervision and failure propagation across actors.
