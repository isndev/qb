# Asynchronous operations inside actors

> **Audience:** Adopter · **Status:** stable · **Verified-against:** qb 3.0.0 (C++20 default, C++23 supported)

How an actor uses the `qb-io` event loop — deferred callbacks, inactivity timers, and coroutines — to perform time-based and I/O-bound work without blocking its `VirtualCore`.

**Prerequisites:** [Core concepts: the actor model](../2_core_concepts/actor_model.md), [Reference: `qb-io` async system](../3_qb_io/async_system.md) — **See also:** [Network actors](./network_actors.md), [Reference: time utilities](../3_qb_io/utilities.md)

## Summary

Every `VirtualCore` runs a single thread that drives one `qb::io::async::listener` event loop. Actors assigned to that core and the asynchronous I/O objects bound to its loop share the same thread; thread safety comes from isolation, not locks. Because that one thread interleaves actor message handling and event-loop callbacks, an actor must never block it: a synchronous `read`, a `sleep`, or a mutex wait stalls every other actor and every pending I/O event on the core.

This page covers the mechanisms an actor uses to stay non-blocking:

- `qb::io::async::callback` — schedule a callable to run later on the same core's loop.
- `qb::io::async::scoped_callback` — the same, but cancellable through a caller-owned handle.
- `qb::io::async::with_timeout<T>` — a CRTP mixin that fires after a span of inactivity.
- `Actor::spawn_detached` — drive a coroutine on the core's loop, awaiting I/O without blocking.
- Wrapping unavoidable blocking work (such as synchronous file I/O) in a callback or a dedicated worker actor.

## The single-thread-per-core contract

A `VirtualCore` owns its actors exclusively, in one thread. The same thread owns the `listener::current` event loop and every async object registered with it. Two invariants follow, and both are load-bearing:

- **No sharing across threads.** An I/O object (client, server, session, watcher, timer) created on one core's loop must not be touched from another thread. Cross-core communication goes through events (`push`/`broadcast`), never through shared pointers to I/O state. <!-- src: qb/src/qb/io/async/listener.h:63 -->
- **No blocking the loop.** While a callback or message handler runs, the loop cannot dispatch anything else on that core. A blocking call inside it freezes the whole core until it returns. <!-- src: qb/src/qb/core/ICallback.h:160 -->

Within a single core there are no data races to defend against — that is the point of the model — but the cost of that simplicity is the no-blocking rule. The rest of this page is about honoring it.

## `qb::io::async::callback` — deferred and delayed actions

`qb::io::async::callback` schedules a callable to run later on the calling thread's event loop. It is the primary way an actor defers work to a future loop iteration.

```cpp
// Declared in qb/src/qb/io/async/io.h
namespace qb::io::async {
    template <typename _Func>
    void callback(_Func &&func);                          // run on next opportunity

    template <typename _Func, typename Rep, typename Period>
    void callback(_Func &&func, std::chrono::duration<Rep, Period> timeout);
}
```

Key facts, each verified against the header:

- **The delay is a `std::chrono` duration, not a `double`.** Pass `200ms`, `std::chrono::seconds(5)`, or any `std::chrono::duration`. There is no seconds-as-`double` overload. <!-- src: qb/src/qb/io/async/io.h:366 -->
- **A non-positive (or absent) delay fires inline, immediately.** `callback(f)` and `callback(f, d)` with `d <= 0` invoke `func()` synchronously at the call site — they do *not* defer to the next iteration. Use a strictly positive duration when you need the callback to run on a later turn of the loop. <!-- src: qb/src/qb/io/async/io.h:318,368 -->
- **The callback runs on the same `VirtualCore`** that scheduled it, so it may touch that actor's state — but only if the actor is still alive when it fires.
- **Fire-and-forget.** The scheduled timer (`Timeout<F>`) is heap-allocated and deletes itself after firing; there is no handle to cancel it. When you need cancellation, use `scoped_callback` (below).

### Capture safety: the actor may be gone

A delayed callback can outlive the actor that scheduled it. If the lambda captures `this`, guard every member access with `is_alive()`, which returns `false` once the actor has been terminated:

```cpp
// Verify the actor still exists before touching its state.
qb::io::async::callback([this, task_id]() {
    if (!is_alive())
        return;                       // actor was killed; do nothing
    // ... safe to use this-> members here ...
}, std::chrono::seconds(5));
```

For longer or riskier deferrals, prefer the message-back pattern: have the callback `push` an event to the actor's own `id()` instead of mutating state directly. Events addressed to a dead actor are silently dropped, so this needs no liveness check and keeps all state changes inside ordinary `on(Event&)` handlers.

### A timeout-watchdog example

This actor starts an operation and arms a timeout. If a completion signal does not clear the pending flag before the callback fires, the callback declares the operation timed out and resolves it through an event.

```cpp
// Inactivity / operation-timeout pattern using async::callback.
#include <qb/actor.h>
#include <qb/io/async.h>
#include <qb/io.h>
#include <chrono>
#include <map>

using namespace std::chrono_literals;

struct TaskComplete : qb::Event {
    int  task_id;
    bool timed_out;
    TaskComplete(int id, bool timeout) : task_id(id), timed_out(timeout) {}
};

class WatchdogActor : public qb::Actor {
    std::map<int, bool> _pending;   // task_id -> still pending
    int                 _next_id = 1;

public:
    qb::io::async::task<bool> onInit() override {
        registerEvent<TaskComplete>(*this);
        registerEvent<qb::KillEvent>(*this);
        startOperation(5s);
        co_return true;
    }

    void startOperation(qb::duration timeout) {
        const int id = _next_id++;
        _pending[id] = true;
        // ... kick off the real non-blocking operation here ...

        qb::io::async::callback([this, id]() {
            if (!is_alive())
                return;                                   // actor gone
            auto it = _pending.find(id);
            if (it != _pending.end() && it->second) {     // still pending -> timed out
                push<TaskComplete>(this->id(), id, true);
                _pending.erase(it);
            }
        }, timeout);
    }

    void on(const TaskComplete &ev) {
        qb::io::cout() << "task " << ev.task_id
                       << (ev.timed_out ? " timed out\n" : " completed\n");
    }

    void on(const qb::KillEvent &) { kill(); }
};
```

`startOperation` takes a `qb::duration` — the canonical span type used for every timeout, delay, and interval in the public API. It is an alias for `std::chrono::nanoseconds` and accepts any finer-or-equal chrono literal implicitly (`5s`, `200ms`), while rejecting a bare integer at compile time. <!-- src: qb/src/qb/system/time.h:90 -->

### Common uses

- **Operation timeouts.** Arm a callback when you start something; cancel its effect by clearing a pending flag when the result arrives first (as above).
- **Retry with backoff.** On a failed attempt, reschedule the next try with a growing delay.
- **Yielding between steps.** Split a long computation into chunks and schedule the next chunk so the loop can service other actors in between. Use a strictly positive delay, since a zero delay runs inline and does not yield.
- **Periodic work.** A callback can reschedule itself. For strictly periodic, every-iteration work, prefer `qb::ICallback` (see below).

## `qb::io::async::scoped_callback` — cancellable deferred actions

When you need to cancel a pending callback — a per-request deadline, a watchdog you re-arm, a retry you may abandon — use `scoped_callback`. It returns a `std::unique_ptr` to a caller-owned `ScopedTimeout`; destroying or reassigning that pointer cancels the pending call.

```cpp
// Declared in qb/src/qb/io/async/io.h
namespace qb::io::async {
    template <typename _Func, typename Rep, typename Period>
    [[nodiscard]] auto
    scoped_callback(_Func &&func, std::chrono::duration<Rep, Period> timeout);
}
```

```cpp
// Hold the handle as a member; let it cancel automatically on destruction.
class RequestActor : public qb::Actor {
    std::unique_ptr<qb::io::async::ScopedTimeout<std::function<void()>>> _deadline;

public:
    void arm() {
        // Wrap the callable in std::function so the returned
        // ScopedTimeout<std::function<void()>> matches the member's type.
        _deadline = qb::io::async::scoped_callback(
            std::function<void()>{[this]() {
                if (!is_alive())
                    return;
                // deadline elapsed without a response
            }},
            std::chrono::seconds(3));
    }

    void onResponse() {
        _deadline.reset();   // cancels the pending deadline if it has not fired
    }
};
```

`ScopedTimeout` exposes `cancel()` and `fired()`. As with `callback`, a non-positive duration fires inline at construction. The trade-off versus `callback`: `scoped_callback` is cancellable and owned by you; `callback` is fire-and-forget and self-cleaning. <!-- src: qb/src/qb/io/async/io.h:433-437,427; qb/src/qb/io/async/io.h:410 -->

## `qb::io::async::with_timeout<T>` — inactivity timers

For an actor (or any async object) that should act when it has been *idle* for a span — close an idle session, drop a client that stopped sending heartbeats — inherit the `with_timeout<T>` CRTP mixin.

```cpp
// Declared in qb/src/qb/io/async/io.h
template <typename _Derived>
class with_timeout {
public:
    explicit with_timeout(qb::duration timeout = std::chrono::seconds(3));
    void       updateTimeout() noexcept;            // reset the idle countdown
    void       setTimeout(qb::duration timeout) noexcept;  // 0 disables the timer
    qb::duration getTimeout() const noexcept;
    // _Derived must define: void on(qb::io::async::event::timer const&);
};
```

Mechanics, verified against the header:

- The constructor takes a `qb::duration` and **defaults to `std::chrono::seconds(3)`**. A value `<= 0` starts disabled. <!-- src: qb/src/qb/io/async/io.h:118 -->
- `updateTimeout()` records "now" as the last-activity time; call it from handlers that count as activity to push the deadline forward.
- When the configured span elapses with no `updateTimeout()`, the mixin invokes `_Derived::on(qb::io::async::event::timer const&)`. The canonical handler signature takes the event by `const &`. <!-- src: qb/src/qb/io/async/io.h:184; qb/tests/io/system/async/timer-timeout.cpp:91 -->
- `setTimeout(d)` reconfigures and restarts the timer; `setTimeout(qb::duration::zero())` stops it. <!-- src: qb/src/qb/io/async/io.h:146 -->

```cpp
// Session actor that self-terminates after 30s of inactivity.
#include <qb/actor.h>
#include <qb/io/async.h>
#include <qb/io.h>
#include <chrono>

using namespace std::chrono_literals;

struct ClientPing : qb::Event {};

class SessionActor
    : public qb::Actor
    , public qb::io::async::with_timeout<SessionActor> {
public:
    SessionActor() : with_timeout(30s) {}

    qb::io::async::task<bool> onInit() override {
        registerEvent<ClientPing>(*this);
        registerEvent<qb::KillEvent>(*this);
        updateTimeout();          // start the countdown
        co_return true;
    }

    void on(const ClientPing &) {
        updateTimeout();          // activity: defer the deadline
    }

    // Fired by with_timeout once 30s elapse with no updateTimeout().
    void on(qb::io::async::event::timer const &) {
        qb::io::cout() << "session " << id() << " idle, terminating\n";
        kill();
    }

    void on(const qb::KillEvent &) {
        setTimeout(qb::duration::zero());   // stop the timer before teardown
        kill();
    }
};
```

`with_timeout` is the same mixin that network session classes use for idle-connection cleanup; the message-broker server's `BrokerSession` closes its connection from exactly this handler. <!-- src: examples/core_io/message_broker/server/BrokerSession.cpp:159 -->

> **`with_timeout` vs. `callback`.** `with_timeout` models *inactivity* — a deadline that resets on activity and auto-reschedules until the real deadline. `callback`/`scoped_callback` model a *one-shot* deferral at a fixed delay. Reach for `with_timeout` when "reset the clock on every message" is the natural description; reach for a callback when "do X once, T from now" is.

## Coroutines inside actors — `spawn` and `spawn_detached`

An actor can drive a C++ coroutine on its core's event loop. The coroutine can `co_await` asynchronous operations (sleeps, I/O, combinators) and suspend without blocking the loop; the core services other actors while it is parked, then resumes it when the awaited operation completes. There are two entry points:

```cpp
// Declared in qb/src/qb/core/Actor.h
template <typename Func> void spawn(Func &&func) const;            // scoped — cancelled on kill (recommended)
template <typename Func> void spawn_detached(Func &&func) const;   // detached — outlives the actor
```

- **`spawn(func)` — the recommended default.** The coroutine is *scoped* to the actor: when the actor is `kill()`ed, it is cooperatively cancelled at its next cancellation-aware suspension point. Its callable receives a `qb::ScopedCoroContext` (alias `scoped_coro_context`) — a `CoroContext` extended with cancellation-aware operations (`ctx.sleep(d)`, `ctx.until_cancelled()`, `ctx.cancellation_point()`, `ctx.cancellable(awaitable)`, child tokens). Request/response patterns — `qb::ask`, `qb::ask_all`, `qb::run_saga`, … — are **free functions** in `qb/patterns.h` that build on this context. Use `spawn` for any work bound to the actor's lifetime.
- **`spawn_detached(func)` — explicit fire-and-forget.** The coroutine is *not* tied to the actor's lifetime: it runs to completion even after the actor is destroyed and is never cancelled on kill. Its callable receives a plain `qb::CoroContext`. Reach for it only when the work must deliberately outlive its actor, or when the coroutine has no cancellation-aware suspension point to cancel at.

Both return immediately and **share the same safety contract**, because a coroutine frame can still be running a step while — or just after — its actor is destroyed: <!-- src: qb/src/qb/core/Actor.h:1043,1080 -->

- **Never access actor members after a `co_await`.** The actor may have been destroyed while the coroutine was suspended; touching `this->_member` afterward is undefined behavior.
- **Copy everything you need by value before the first `co_await`.** Do not capture `this` or references to actor members into the coroutine.
- **After suspension, use only the `CoroContext`.** `ctx.push<Event>(...)` (to the spawning actor's own id), `ctx.push_to<Event>(dest, ...)` (to another actor), `ctx.id()`, and `ctx.time()` are safe; events to a dead actor are dropped. <!-- src: qb/src/qb/core/VirtualCore.h:1025,1031 -->
- **Keep coroutines short-lived.** The longer a coroutine runs, the wider the window in which its actor can be destroyed.

```cpp
// Safe coroutine: copy state out, await, reply only through the context.
void on(RequestEvent &ev) {
    // Copy ALL needed data BEFORE spawning — no 'this', no member refs.
    std::string key    = ev.key;
    qb::ActorId sender = ev.sender;

    spawn([key, sender](auto ctx) -> qb::io::async::task<void> {
        auto reply = co_await fetch(key);              // actor may die here
        // push_to(dest, ...) addresses another actor; push(...) would go to self.
        ctx.template push_to<ResultEvent>(sender, reply);   // safe via context
    });
}
```

```cpp
// DANGEROUS — do not do this (the footgun is identical for spawn and spawn_detached).
spawn([this](auto ctx) -> qb::io::async::task<void> {
    co_await ctx.sleep(100ms);   // actor may die while suspended
    this->_member = value;       // UNDEFINED BEHAVIOR: actor may be gone
});
```

`Actor::has_active_coroutines()` and `active_coroutine_count()` report whether suspended coroutines are still outstanding — useful before deciding to `kill()`. The coroutine scheduler is shared per `VirtualCore` and established when the core's listener is created, so both `spawn` and `spawn_detached` require no setup beyond running inside the engine. <!-- src: qb/src/qb/core/Actor.h:1211-1213,1196; qb/src/qb/io/async/listener.h:293; qb/src/qb/core/Actor.cpp:240-263 -->

For the scoped-cancellation operations and the native `ask()` request/response pattern, see the [scoped-coroutine and ask recipes](../6_guides/patterns_cookbook.md). For the awaitables themselves (`sleep`, timeouts, channels, `when_all`/`when_any`/`race`), see [Reference: C++20 coroutines](../3_qb_io/coroutines.md).

## Periodic work: `qb::ICallback`

For logic that must run on *every* loop iteration (polling, draining a queue, heartbeats), inherit `qb::ICallback` alongside `qb::Actor` and register with `registerCallback(*this)`. The core calls `on(qb::LoopEvent const&)` once per iteration until you call `unregisterCallback()`. The `qb::LoopEvent` carries per-loop context (`now`, `iteration`).

```cpp
// Declared in qb/src/qb/core/ICallback.h
class ICallback {
public:
    virtual void on(qb::LoopEvent const &) = 0;   // called every loop iteration while registered
};
```

`on(qb::LoopEvent const&)` is bound by the same no-blocking rule as everything else on the core: it must return quickly and must never block, sleep, or do synchronous I/O. <!-- src: qb/src/qb/core/ICallback.h:160 --> For a one-shot or backoff schedule rather than every-iteration work, prefer `async::callback`. For the registration API and a worked heartbeat example, see [Reference: `qb::Actor` (`ICallback`)](../4_qb_core/actor.md).

## Blocking file I/O from an actor

Synchronous file I/O (`qb::io::sys::file::read` / `write`) blocks the calling thread, and an actor's thread is its whole `VirtualCore`. Three patterns keep the core responsive, in increasing order of isolation:

1. **Wrap the blocking call in `async::callback`** (suitable for infrequent, non-critical I/O). The callback still blocks the core *for its own turn*, but it keeps the blocking work out of the actor's main message-handling path. When the I/O finishes, `push` a result event back to the requester. This is what the `file_processor` worker does. <!-- src: examples/core_io/file_processor/file_worker.h:100 -->

   ```cpp
   // FileWorker schedules the blocking read off the message-handling path.
   qb::io::async::callback([this, request, file_content]() {
       qb::io::sys::file file;
       if (file.open(request.filepath.c_str(), O_RDONLY) >= 0) {
           // ... synchronous read into file_content ...
           file.close();
       }
       // push a ReadFileResponse back to request.requestor ...
   });
   ```

2. **Dedicate worker actors to I/O.** Place file-I/O actors on their own core(s) and delegate requests to them as events. Blocking is then confined to that core, leaving the rest of the system unaffected. The `file_processor` example builds exactly this manager-worker topology. <!-- src: examples/core_io/file_processor/main.cpp:14 -->

3. **Watch the filesystem instead of polling it.** To *react* to file or directory changes, use `qb::io::async::file_watcher<T>` / `directory_watcher<T>`, which deliver `on(qb::io::async::event::file const&)` notifications through the loop with no blocking. The `file_monitor` example demonstrates a directory-watcher actor. <!-- src: examples/core_io/file_monitor -->

## Pitfalls

- **Passing a `double` as a delay.** `callback`/`with_timeout` take `qb::duration` (a `std::chrono` span), not seconds-as-`double`. Write `5s` or `std::chrono::milliseconds(200)`, never `5.0`. A bare integer does not compile. <!-- src: qb/src/qb/system/time.h:90 -->
- **Expecting `callback(f)` or a zero delay to defer.** A non-positive duration runs `func()` inline at the call site. Use a strictly positive delay to schedule for a later loop turn. <!-- src: qb/src/qb/io/async/io.h:318,368 -->
- **Touching `this` after the actor died.** A delayed callback can outlive its actor — guard with `is_alive()`, or prefer `push`-back-to-self over direct mutation.
- **Accessing actor state after `co_await`.** Inside a coroutine, the actor may be destroyed across any suspension point. Copy state by value before the first `co_await` and use only the `CoroContext` afterward.
- **Sharing I/O objects across cores.** An async object is bound to one core's loop. Never hand it to another thread; communicate with events instead. <!-- src: qb/src/qb/io/async/listener.h:63 -->
- **Blocking the loop.** A synchronous `read`, a `sleep`, a mutex wait, or an unbounded computation in a handler, callback, or `on(qb::LoopEvent const&)` freezes the entire core. Defer it, chunk it, or offload it to a worker actor.

## See also

- [Network actors](./network_actors.md) — combining `qb::Actor` with `qb::io::use<T>` clients, servers, and sessions.
- [Reference: `qb-io` async system](../3_qb_io/async_system.md) — the listener, timers, and watchers in full.
- [Reference: C++20 coroutines](../3_qb_io/coroutines.md) — `task<T>`, awaiters, and combinators.
- [Reference: `qb::Actor`](../4_qb_core/actor.md) — `qb::ICallback` registration and the periodic-callback lifecycle.
- [Reference: time utilities](../3_qb_io/utilities.md) — `qb::duration`, `qb::mono_time`, `qb::wall_time`.
