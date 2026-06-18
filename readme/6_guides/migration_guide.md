# Migration guide

> **Audience:** Adopter · **Status:** stable · **Verified-against:** qb 2.0.0 (C++20 default, C++23 supported)

Move an existing codebase to qb: from hand-rolled `std::thread` + locked queues to actors, and from the pre-2.0 `qb::Timestamp`/`qb::Duration` time types to the `qb::duration`/`qb::mono_time`/`qb::wall_time` chrono model.

**Prerequisites:** [Getting started](./getting_started.md) — **See also:** [The actor model](../2_core_concepts/actor_model.md), [Inter-actor messaging](../4_qb_core/messaging.md), [The threading model](../2_core_concepts/threading_model.md), [Time vocabulary](../3_qb_io/utilities.md#time-vocabulary-qbduration-qbmono_time-qbwall_time)

## Summary

This guide covers two migrations that adopters hit most often.

1. **Threads to actors.** You have shared state guarded by a mutex, one or more worker threads, and a `std::queue` (plus condition variable) carrying work between them. The actor model replaces all three: state is owned by one actor, work is an event, and the queue and locks disappear. This section maps each primitive to its actor-model equivalent and walks one worker-pool example through the rewrite.

2. **The pre-2.0 time types to the canonical chrono model.** qb 2.0 retired `qb::Timestamp`, `qb::Duration`, and `qb::TimePoint` in favor of three `std::chrono` aliases: `qb::duration`, `qb::mono_time`, and `qb::wall_time`. This section gives a concrete old-to-new mapping so call sites compile against the current API.

Neither migration is all-or-nothing. An actor can call into existing synchronous code, and the time aliases are plain `std::chrono` types, so a partial port still compiles and runs.

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
| Polling loop / `sleep_for` between iterations | `qb::ICallback::onCallback()` or `qb::io::async::callback` | A per-loop tick or a one-shot non-blocking timer on the same core. |
| Manual `errno` / return-code propagation across threads | An error event, or an unhandled exception caught by the core | See [Error handling](./error_handling.md). |

`push` and `send` differ: `push<_Event>(dest, …)` returns a reference to the queued event and guarantees ordered delivery from one source to one destination; `send<_Event>(dest, …)` is unordered, fire-and-forget, and restricted to trivially-destructible events. Prefer `push`. _(`qb/include/qb/core/Actor.h:729,752`.)_

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
    bool onInit() final {
        registerEvent<JobEvent>(*this);   // subscribe; KillEvent is automatic
        return true;
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

    bool onInit() final {
        for (int i = 0; i < 3; ++i)
            push<JobEvent>(_worker, i, (i + 1) * 10);   // enqueue, ordered
        push<qb::KillEvent>(_worker);                   // graceful stop
        kill();                                         // stop self
        return true;
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
- `stop()` and the `_stop` flag became `push<qb::KillEvent>(worker)`. Every actor is auto-subscribed to `qb::KillEvent` at construction, and the default handler calls `kill()`. _(`qb/source/core/src/Actor.cpp:38,70`; [Getting started](./getting_started.md).)_
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

- **Do not block inside a handler.** A handler holds its core's event-loop thread until it returns; blocking stalls every actor on that core. Wrap blocking or long-running work (synchronous file I/O, a slow library call) in `qb::io::async::callback` so it does not freeze the loop, or restructure it as events. See [Asynchronous operations inside actors](../5_core_io_integration/async_in_actors.md). _(`qb/include/qb/core/ICallback.h:16`.)_
- **Do not share an actor's state with other threads.** The no-lock guarantee holds only because one core touches the state. Reintroducing a raw pointer, a `std::shared_ptr` to mutable data, or a global shared with non-actor code reintroduces the data race. Communicate by sending events.
- **`addActor` can fail — and *how* depends on the path.** The pre-start `Main::addActor<T>(coreId, …)` overload does **not** run `onInit()`; it returns `qb::ActorId::NotFound` (the default-constructed, invalid ID) only when the per-core ID pool is exhausted or a duplicate service `Tag` is registered. An `onInit()` that returns `false` for a pre-start actor is detected at startup: the engine flags that core `VirtualCore::Error::BadActorInit`, the core fails to start, and you observe it via `hasError()` after the run — not through the returned ID. The runtime `addRefActor` / `addActor(…)` path is the one that *does* run `onInit()` at add time and returns an invalid ID when it returns `false`. Guard every returned ID with `is_valid()`, and gate startup with `hasError()`. _(`qb/include/qb/core/Main.tpp:37-65`; `qb/source/core/src/VirtualCore.cpp:466-475`; `qb/source/core/src/Main.cpp:204-215`; `qb/include/qb/core/ActorId.h:403,444`; [Error handling](./error_handling.md).)_
- **Add every actor before `start()`.** Actors are constructed on their worker thread when the engine starts; `addActor` must be called beforehand.
- **A periodic task is not a `sleep` loop.** Replace a polling thread with `qb::ICallback` (`onCallback()` runs once per loop iteration) or a one-shot `qb::io::async::callback`, both non-blocking. _(`qb/include/qb/core/ICallback.h:16,122`.)_

## Part 2 — From the pre-2.0 time types to the chrono model

### Concepts

qb 2.0 replaced the framework-specific time classes with three `std::chrono` aliases. They are the single source of truth across qb and every module; the [time vocabulary](../3_qb_io/utilities.md#time-vocabulary-qbduration-qbmono_time-qbwall_time) section owns their full definition. In brief:

| Alias | Underlying type | Use for |
|---|---|---|
| `qb::duration` | `std::chrono::nanoseconds` | Every timeout, delay, TTL, interval, and latency value in public APIs. |
| `qb::mono_time` | `std::chrono::steady_clock::time_point` | Deadlines, timers, the event-loop "now", latency, RTT — immune to NTP/DST. |
| `qb::wall_time` | `std::chrono::system_clock::time_point` | Dates, expiry, JWT `exp`/`nbf`, TLS validity, logs, wire formats. |

_(`qb/include/qb/system/timestamp.h:82,85,88`.)_

Two design points drive the migration:

- **A `qb::duration` rejects a bare integer.** It accepts any finer-or-equal `std::chrono` literal implicitly (`30s`, `100ms`, `5us`) but a raw `int` does not convert, so a seconds-versus-milliseconds unit confusion cannot compile. Wherever old code passed a number, pass a chrono literal. _(`qb/include/qb/system/timestamp.h:8-11`.)_
- **`mono_time` and `wall_time` are distinct types.** Subtracting a wall instant from a monotonic one does not compile. Pick `mono_time` for anything you measure or schedule against (deadlines, elapsed time, RTT) and `wall_time` for anything tied to the calendar (expiry, logs, wire timestamps). _(`qb/include/qb/system/timestamp.h:18-20`.)_

The literal operators (`30s`, `100ms`, `5us`, …) are pulled into `qb` through `inline namespace qb::time_literals`, so call sites can write them after `#include <qb/system/timestamp.h>` with no extra `using`. _(`qb/include/qb/system/timestamp.h:104-106`.)_

> **Retired tokens — never reintroduce them.** `qb::Timestamp`, `qb::Duration`, and `qb::TimePoint` no longer exist anywhere in the framework; the canonical replacements are defined in `<qb/system/timestamp.h>`. Any reference to them is pre-2.0 code that must be ported. The documentation anti-drift guard rejects these tokens everywhere except the migration, contributing, and changelog surfaces. _(`qb/include/qb/system/timestamp.h:79-88`; `qb/scripts/doc-lint.sh:44-51`.)_

### Old-to-new mapping table

| Pre-2.0 construct | qb 2.0 replacement | Header / note |
|---|---|---|
| `qb::Duration` (a span) | `qb::duration` (`std::chrono::nanoseconds`) | `<qb/system/timestamp.h>` |
| `qb::Timestamp` / `qb::TimePoint` used as a deadline, timer base, or elapsed-time anchor | `qb::mono_time` (`steady_clock::time_point`) | Monotonic; immune to clock steps. |
| `qb::Timestamp` / `qb::TimePoint` used as a date, expiry, or wire timestamp | `qb::wall_time` (`system_clock::time_point`) | Calendar time. |
| `Timestamp::now()` for a monotonic anchor | `qb::mono_now()` | Returns `qb::mono_time`. |
| `Timestamp::now()` for a wall-clock instant | `qb::wall_now()` | Returns `qb::wall_time`. |
| `Duration::seconds(n)` / `fromSeconds(n)` | `std::chrono::seconds(n)` (or the literal `n` s, e.g. `30s`) | Implicitly converts to `qb::duration`. |
| `Duration::milliseconds(n)` | `std::chrono::milliseconds(n)` / `100ms` | Implicitly converts to `qb::duration`. |
| `Duration::microseconds(n)` | `std::chrono::microseconds(n)` / `5us` | Implicitly converts to `qb::duration`. |
| `someDuration.seconds()` / `.toSeconds()` | `std::chrono::duration_cast<std::chrono::seconds>(d).count()` | Explicit cast; `count()` yields the integer. |
| `someDuration.milliseconds()` | `std::chrono::duration_cast<std::chrono::milliseconds>(d).count()` | |
| `Timestamp::epochSeconds()` on a wall instant | `qb::unix_seconds(tp)` | `int64_t` seconds since the Unix epoch. _(`timestamp.h:113`.)_ |
| `Timestamp::epochMillis()` | `qb::unix_millis(tp)` (also `unix_micros`, `unix_nanos`) | `int64_t` since the Unix epoch. _(`timestamp.h:119`.)_ |
| `Timestamp::fromEpochSeconds(s)` | `qb::wall_from_unix_seconds(s)` | Returns `qb::wall_time`. _(`timestamp.h:137`.)_ |
| `Timestamp::fromEpochMillis(ms)` | `qb::wall_from_unix_millis(ms)` | Returns `qb::wall_time`. _(`timestamp.h:143`.)_ |
| `timestamp.toString(fmt)` / custom date formatting | `qb::format_utc(tp, fmt)` — strftime-compatible, UTC | Empty string on failure. _(`timestamp.h:155`.)_ |
| ISO-8601 string from a timestamp | `qb::to_iso8601(tp)`, returning `"YYYY-MM-DDTHH:MM:SSZ"` | _(`timestamp.h:173`.)_ |
| Parsing a date string | `qb::parse_utc(str, fmt)` / `qb::from_iso8601(str)` | Returns `std::optional<qb::wall_time>` (`nullopt` on error), UTC only. _(`timestamp.h:181,205`.)_ |
| `Duration::zero()` / a "no timeout" sentinel | `qb::duration::zero()` | Inherited from `std::chrono::nanoseconds`. |
| A hand-rolled scope timer | `qb::ScopedTimer` / `qb::LogTimer` | Monotonic; callback receives a `qb::duration`. _(`timestamp.h:254,305`.)_ |
| A raw RDTSC / CPU-counter read | `qb::tsc_ticks()` | Per-core, uncalibrated — micro-benchmark deltas only, **not a clock**. _(`timestamp.h:214-216`.)_ |

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
// src: qb/include/qb/system/timestamp.h
#include <qb/system/timestamp.h>   // qb::duration, qb::mono_time, qb::mono_now, literals

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
// src: qb/include/qb/system/timestamp.h
#include <qb/system/timestamp.h>

qb::wall_time expiry = qb::wall_now() + 24h;          // 24h from now, wall clock
std::string   iso    = qb::to_iso8601(expiry);        // "YYYY-MM-DDTHH:MM:SSZ"
std::int64_t  exp_s  = qb::unix_seconds(expiry);      // for a JWT exp claim, say
```

### Where the types show up in qb APIs

The migration matters because the framework's own surfaces take these types. A few you will meet immediately:

- `CoreInitializer::setLatency(qb::duration latency = qb::duration::zero())` — the maximum the event loop waits when idle; the default `zero()` is the busy-spin, lowest-latency mode (100% CPU on the core), and a positive value lets the idle core park to trade latency for CPU. _(`qb/include/qb/core/Main.h:242-254`.)_
- Socket and async timeouts take `qb::duration` (for example `tcp::socket::connect(qb::io::endpoint const &ep, qb::duration wtimeout)`); a non-positive value is clamped to a single poll, not "wait forever." _(`qb/include/qb/io/tcp/socket.h:155`; `qb/source/io/src/system/sys__socket.cpp:698-699`.)_
- `qb::SpinLock::trylock_for(qb::duration)` and `trylock_until(qb::mono_time)`. _(`qb/include/qb/system/lockfree/spinlock.h:123,147`.)_

### Pitfalls

- **Do not pass a bare integer to a `qb::duration` parameter.** `setLatency(100)` does not compile by design; write `setLatency(100us)` (or `std::chrono::microseconds(100)`). This is the unit-confusion guard, not a defect. _(`qb/include/qb/system/timestamp.h:8-11`.)_
- **Do not mix the two instant clocks.** You cannot subtract a `qb::wall_time` from a `qb::mono_time`; the compiler rejects it. Measure and schedule with `mono_time`; record dates and expiry with `wall_time`. Converting between them means going through a Unix-epoch scalar (`unix_seconds` / `wall_from_unix_seconds`) and accepting that the wall clock can step. _(`qb/include/qb/system/timestamp.h:18-20`.)_
- **`tsc_ticks()` is not a clock.** It is monotonic per core but uncalibrated and not comparable across cores or to either clock. Use it only for single-thread micro-benchmark deltas. _(`qb/include/qb/system/timestamp.h:214-216`.)_
- **`format_utc`/`parse_utc` are UTC-only.** There is no time-zone database on this toolchain; formatting uses `strftime` and parsing uses `std::get_time` + `timegm`, both in UTC. `format_utc` returns an empty string on failure; `parse_utc` and `from_iso8601` return `std::nullopt`. _(`qb/include/qb/system/timestamp.h:27-30,181-208`.)_
- **`Actor::time()` returns a raw `uint64_t`, not a chrono type.** It is the core's cached epoch-nanosecond count (sourced from `qb::wall_now()`), constant within one handler or `onCallback()` invocation; it is not a `qb::mono_time` or `qb::wall_time`. For a fresh high-precision wall instant use `qb::unix_nanos(qb::wall_now())`. _(`qb/include/qb/core/Actor.h:513-528`.)_

## See also

- [Getting started](./getting_started.md) — install qb and build your first actor program.
- [The actor model](../2_core_concepts/actor_model.md) — isolation, the event loop, and one-at-a-time handler execution.
- [Inter-actor messaging](../4_qb_core/messaging.md) — `push`, `send`, `to`, `reply`, `forward`, and `broadcast`.
- [The threading model](../2_core_concepts/threading_model.md) — how `CoreId`s map to worker threads and how cross-core delivery works.
- [Asynchronous operations inside actors](../5_core_io_integration/async_in_actors.md) — offloading blocking work without stalling the loop.
- [Time vocabulary](../3_qb_io/utilities.md#time-vocabulary-qbduration-qbmono_time-qbwall_time) — the canonical definition of `qb::duration`, `qb::mono_time`, and `qb::wall_time`.
- [Error handling](./error_handling.md) — supervision and failure propagation across actors.
