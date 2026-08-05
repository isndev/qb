# Migration guide

> **Audience:** Adopter · **Status:** stable · **Verified-against:** qb 3.0.0 (C++20 default, C++23 supported)

Move an existing codebase to qb: from hand-rolled `std::thread` + locked queues to actors, and from the pre-2.0 `qb::Timestamp`/`qb::Duration` time types to the `qb::duration`/`qb::mono_time`/`qb::wall_time` chrono model.

**Prerequisites:** [Getting started](./getting_started.md) — **See also:** [The actor model](../2_core_concepts/actor_model.md), [Inter-actor messaging](../4_qb_core/messaging.md), [The threading model](../2_core_concepts/threading_model.md), [Time vocabulary](../3_qb_io/utilities.md#time-vocabulary-qbduration-qbmono_time-qbwall_time)

## Summary

This guide covers the migrations that adopters hit most often.

1. **Threads to actors.** You have shared state guarded by a mutex, one or more worker threads, and a `std::queue` (plus condition variable) carrying work between them. The actor model replaces all three: state is owned by one actor, work is an event, and the queue and locks disappear. This section maps each primitive to its actor-model equivalent and walks one worker-pool example through the rewrite.

2. **The pre-2.0 time types to the canonical chrono model.** qb 2.0 retired `qb::Timestamp`, `qb::Duration`, and `qb::TimePoint` in favor of three `std::chrono` aliases: `qb::duration`, `qb::mono_time`, and `qb::wall_time`. This section gives a concrete old-to-new mapping so call sites compile against the current API.

3. **The synchronous `onInit()` to the async-init APIs.** `onInit()` is a coroutine, and `addRefActor<T>()` returns a phase-aware `ActorHandle<T>` instead of a raw pointer.

4. **`Event::id_type` in 3.0.** It stopped depending on `NDEBUG`, so a Debug consumer and a Release `libqb-core` finally agree on the event header. Two breaks, with different blast radii: the `id_type` return type changes **in Debug only** (a Release-only CI stays green), while `qb::type_id<T>()` now requires a complete type **in every build mode**, which breaks the common `ServiceActor<struct MyTag>` spelling everywhere. Read this one even if you never named `id_type`.

The first three are not all-or-nothing. An actor can call into existing synchronous code, and the time aliases are plain `std::chrono` types, so a partial port still compiles and runs. The fourth is a hard 3.0 break; it is small, and it is a compile error rather than a silent one.

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

`push` and `send` differ: `push<_Event>(dest, …)` returns a reference to the queued event and guarantees ordered delivery from one source to one destination; `send<_Event>(dest, …)` is unordered, fire-and-forget, and restricted to trivially-destructible events. Prefer `push`. _(`qb/src/qb/core/Actor.h:798,821`.)_

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
// src: examples/core/example1_simple_actor.cpp (structure),
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
- **`addActor` can fail — and *how* depends on the path.** The pre-start `Main::addActor<T>(coreId, …)` overload does **not** run `onInit()`; it returns `qb::ActorId::NotFound` (the default-constructed, invalid ID) only when the per-core ID pool is exhausted or a duplicate service `Tag` is registered. An `onInit()` that returns `false` for a pre-start actor is detected at startup: the engine flags that core `VirtualCore::Error::BadActorInit`, the core fails to start, and you observe it via `hasError()` after the run — not through the returned ID. The runtime `addRefActor` / `addActor(…)` path is the one that *does* run `onInit()` at add time and returns an invalid ID when it returns `false`. Guard every returned ID with `is_valid()`, and gate startup with `hasError()`. _(`qb/src/qb/core/Main.h:705-733`; `qb/src/qb/core/Main.cpp:226-232`; `qb/src/qb/core/VirtualCore.h:125`; `qb/src/qb/core/VirtualCore.cpp:735-753`; `qb/src/qb/core/ActorId.h:401,442`; [Error handling](./error_handling.md).)_
- **Add every actor before `start()`.** Actors are constructed on their worker thread when the engine starts; `addActor` must be called beforehand.
- **A periodic task is not a `sleep` loop.** Replace a polling thread with `qb::ICallback` (`on(qb::LoopEvent const&)` runs once per loop iteration) or a one-shot `qb::io::async::callback`, both non-blocking. _(`qb/src/qb/core/ICallback.h:16,122`.)_

## Part 2 — From the pre-2.0 time types to the chrono model

### Concepts

qb 2.0 replaced the framework-specific time classes with three `std::chrono` aliases. They are the single source of truth across qb and every module; the [time vocabulary](../3_qb_io/utilities.md#time-vocabulary-qbduration-qbmono_time-qbwall_time) section owns their full definition. In brief:

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

> **Retired tokens — never reintroduce them.** `qb::Timestamp`, `qb::Duration`, and `qb::TimePoint` no longer exist anywhere in the framework; the canonical replacements are defined in `<qb/system/time.h>`. Any reference to them is pre-2.0 code that must be ported. The documentation anti-drift guard rejects these tokens everywhere except the migration, contributing, and changelog surfaces. _(`qb/src/qb/system/time.h:79-88`; `qb/scripts/doc-lint.sh:44-51`.)_

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
#include <qb/system/time.h>   // qb::duration, qb::mono_time, qb::mono_now, literals

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

qb::wall_time expiry = qb::wall_now() + 24h;          // 24h from now, wall clock
std::string   iso    = qb::to_iso8601(expiry);        // "YYYY-MM-DDTHH:MM:SSZ"
std::int64_t  exp_s  = qb::unix_seconds(expiry);      // for a JWT exp claim, say
```

### Where the types show up in qb APIs

The migration matters because the framework's own surfaces take these types. A few you will meet immediately:

- `CoreInitializer::setLatency(qb::duration latency = qb::duration::zero())` — the maximum the event loop waits when idle; the default `zero()` is the busy-spin, lowest-latency mode (100% CPU on the core), and a positive value lets the idle core park to trade latency for CPU. _(`qb/src/qb/core/Main.h:242-254`.)_
- Socket and async timeouts take `qb::duration` (for example `tcp::socket::connect(qb::io::endpoint const &ep, qb::duration wtimeout)`); a non-positive value is clamped to a single poll, not "wait forever." _(`qb/src/qb/io/tcp/socket.h:155`; `qb/src/qb/io/system/sys__socket.cpp:724`.)_
- `qb::SpinLock::trylock_for(qb::duration)` and `trylock_until(qb::mono_time)`. _(`qb/src/qb/system/lockfree/spinlock.h:135-136,147`.)_

### Pitfalls

- **Do not pass a bare integer to a `qb::duration` parameter.** `setLatency(100)` does not compile by design; write `setLatency(100us)` (or `std::chrono::microseconds(100)`). This is the unit-confusion guard, not a defect. _(`qb/src/qb/system/time.h:8-11`.)_
- **Do not mix the two instant clocks.** You cannot subtract a `qb::wall_time` from a `qb::mono_time`; the compiler rejects it. Measure and schedule with `mono_time`; record dates and expiry with `wall_time`. Converting between them means going through a Unix-epoch scalar (`unix_seconds` / `wall_from_unix_seconds`) and accepting that the wall clock can step. _(`qb/src/qb/system/time.h:18-20`.)_
- **`tsc_ticks()` is not a clock.** It is monotonic per core but uncalibrated and not comparable across cores or to either clock. Use it only for single-thread micro-benchmark deltas. _(`qb/src/qb/system/time.h:680-684`.)_
- **`format_utc`/`parse_utc` are UTC-only.** There is no time-zone database on this toolchain; formatting uses `strftime` and parsing uses `std::get_time` + `timegm`, both in UTC. `format_utc` returns an empty string on failure; `parse_utc` and `from_iso8601` return `std::nullopt`. _(`qb/src/qb/system/time.h:27-30,305-342`.)_
- **`Actor::time()` returns a raw `uint64_t`, not a chrono type.** It is the core's cached epoch-nanosecond count (sourced from `qb::wall_now()`), constant within one handler or `on(qb::LoopEvent const&)` invocation; it is not a `qb::mono_time` or `qb::wall_time`. For a fresh high-precision wall instant use `qb::unix_nanos(qb::wall_now())`. _(`qb/src/qb/core/Actor.h:560-577`.)_

## Part 3 — From the synchronous `onInit()` to the async-init APIs

The development branch makes two **source-incompatible** changes to the actor API. Both are
mechanical to adopt.

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

## See also

- [Getting started](./getting_started.md) — install qb and build your first actor program.
- [The actor model](../2_core_concepts/actor_model.md) — isolation, the event loop, and one-at-a-time handler execution.
- [Inter-actor messaging](../4_qb_core/messaging.md) — `push`, `send`, `to`, `reply`, `forward`, and `broadcast`.
- [The threading model](../2_core_concepts/threading_model.md) — how `CoreId`s map to worker threads and how cross-core delivery works.
- [Asynchronous operations inside actors](../5_core_io_integration/async_in_actors.md) — offloading blocking work without stalling the loop.
- [Time vocabulary](../3_qb_io/utilities.md#time-vocabulary-qbduration-qbmono_time-qbwall_time) — the canonical definition of `qb::duration`, `qb::mono_time`, and `qb::wall_time`.
- [Error handling](./error_handling.md) — supervision and failure propagation across actors.
