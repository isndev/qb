# Asynchronous work inside an actor

> **Audience:** Adopter · **Status:** stable · **Verified-against:** qb 3.0.1 (C++20 default, C++23 supported) — 60487ee7

An actor's thread is not its own — it is the `VirtualCore`, and every other actor on that core is waiting behind it. This page is about the primitives that let a handler *stop doing work and come back later* (`defer`, `callback`, `scoped_callback`, `with_timeout`, `spawn`), and about the one call that looks like it does that and does the opposite.

**Prerequisites:** [Writing actors](../4_qb_core/actor.md) · [The async runtime](../3_qb_io/async_system.md) — **See also:** [Network actors](./network_actors.md) · [C++20 coroutines](../3_qb_io/coroutines.md) · [The engine](../4_qb_core/engine.md) · [The time vocabulary](../0_foundations/time.md)

## One thread, two kinds of work

Every `VirtualCore` runs a single thread that drives exactly one `qb::io::async::listener` event loop. Actors assigned to that core and the async I/O objects bound to its loop share that thread; thread safety comes from isolation, not locks. Because the same thread interleaves message handling and loop callbacks, two invariants govern everything below:

- **No sharing across threads.** An I/O object — client, server, session, watcher, timer — created on one core's loop must not be touched from another. Cross-core communication goes through events (`push`/`broadcast`), never through a shared pointer to live I/O state. <!-- src: qb/src/qb/io/async/listener.h:66-67,69-81 -->
- **No blocking the loop.** While a handler or a callback runs, nothing else on that core can be dispatched. A blocking call inside it freezes the whole core until it returns. <!-- src: qb/src/qb/core/ICallback.h:163-165 -->

Within a core there are no data races to defend against — that is the point of the model. The price is the second rule, and the rest of this page is about paying it.

## The two call chains

This is the comparison that matters, and it is a comparison of *stacks*, not of styles. Both start in the same place: your `on(Event&)` handler, dispatched at step 6 of [the loop pass](../4_qb_core/engine.md#the-loop-pass), **after** `listener::current.run()` has already returned for this pass.

### `co_await` — the stack unwinds and the core moves on

```
VirtualCore::__workflow__                          ← pass N
 ├─ listener::current.run(EVRUN_NOWAIT)            step 3 (VirtualCore.cpp:691)
 ├─ __flush_all__()                                step 5 (VirtualCore.cpp:704)
 └─ __receive__()                                  step 6 (VirtualCore.cpp:706)
     └─ your on(RequestEvent&)
         └─ spawn([...](auto ctx) -> task<void> { ... });
            └─ registers a frame with the core's scheduler and RETURNS
     └─ …the rest of this core's inbound events are dispatched…
 ├─ ICallback ticks · reap · idle
 └─ next pass

VirtualCore::__workflow__                          ← pass N+1
 └─ listener::current.run(EVRUN_NOWAIT)
     └─ CoroutineScheduler::run_ready(65536)
         └─ resumes your frame
             └─ co_await ctx.sleep(50ms)
                └─ await_suspend returns → the stack UNWINDS to run_ready
     └─ run_ready returns → run() returns
 └─ __flush_all__ · __receive__ · ticks · reap      ← every actor gets its turn
```

The suspension is a *return*. Between the `co_await` and the resume, this core does everything it normally does: flushes its pipes, drains its mailbox, dispatches every other actor's events, ticks its callbacks, reaps the dead. Your coroutine costs the core one frame of memory and nothing else.

### `run_sync` — the stack stays, and step 6 never finishes

```
VirtualCore::__workflow__                          ← pass N
 ├─ listener::current.run(EVRUN_NOWAIT)            step 3 — already returned
 ├─ __flush_all__()                                step 5
 └─ __receive__()                                  step 6
     └─ your on(RequestEvent&)
         └─ run_sync(awaitable)
             ├─ ensure_not_inside_ready_drain("run_sync()")   ← passes: see below
             └─ pump(done):
                 while (!done)
                     listener::current.run(EVRUN_NOWAIT)   ×≤16 while coroutines are ready
                     sleep_for(1ms)                        when nothing is ready
             ↑ does not return until the awaitable completes
     ✗ the remaining inbound events of this pass are never dispatched
 ✗ no ICallback tick, no reap, no next __flush_all__, no next __receive__
```

Everything below the `run_sync` frame is postponed for as long as the awaitable takes. And note what the pump *does* keep doing: it calls `listener::current.run()` in a loop, so sockets are serviced, timers fire, deferred callbacks drain and other coroutines resume. **The I/O layer of this core stays alive; the actor layer of this core is stopped.** That is what makes it hard to notice — a sanity check that "the socket still responds" passes, and the only symptom is actor latency.

There is a second-order effect worth knowing, because it shows up as a problem somewhere else. Because the core stops draining its own mailbox, peers pushing to it eventually find the ring full: a guaranteed event burns its bounded backoff and the sender partial-bails and retries next pass, so the stall propagates outward as [backpressure](../4_qb_core/engine.md#backpressure-why-the-flush-always-terminates); a `qb::EventQOS0` event is simply dropped.

### The guard passes, and that is the whole problem

`run_sync` and `run_for` both open with `ensure_not_inside_ready_drain(...)`, which asks exactly one question — is this scheduler currently inside `CoroutineScheduler::run_ready()`? An actor handler is dispatched from `__receive__`, *after* `run()` has returned, so the flag is false, the guard passes, and there is **no assertion, no throw, no log, no trace**. [The async runtime](../3_qb_io/async_system.md#the-guard-and-what-it-actually-checks) owns that mechanism and the scope test that follows from it; this page does not restate it.

The rule, in the form that matters here:

> **Inside an actor, `co_await` — through `Actor::spawn` and the free `qb::ask` — is the only correct form.** `run_sync` and `run_for` belong where the calling thread is yours to block: a `main()`, a test fixture, a CLI, a setup step before `qb::Main::start()`.

The framework's own best statement of the legitimate case is a comment in an example:

> *Pre-engine setup: there is no actor loop yet, so we drive a coroutine to completion synchronously.*
> — `examples/07-applications/02-auction-house/src/main.cpp:48-49`

## Coroutines from a handler: `spawn` and `spawn_detached`

```cpp
// Declared in qb/src/qb/core/Actor.h
template <typename Func> void spawn(Func &&func) const;            // scoped — cancelled on kill (recommended)
template <typename Func> void spawn_detached(Func &&func) const;   // detached — outlives the actor
```

- **`spawn(func)` — the default.** The coroutine joins the actor's cancellation scope, so a `kill()` signals it. Its callable receives a `qb::ScopedCoroContext` (alias `qb::scoped_coro_context`) — a `CoroContext` extended with cancellation-aware operations (`ctx.sleep(d)`, `ctx.until_cancelled()`, `ctx.cancellation_point()`, `ctx.cancellable(task)`, `ctx.child_token()`). The request/response and orchestration patterns — `qb::ask`, `qb::ask_all`, `qb::run_saga`, … — are **free functions** in `qb/patterns.h` that take that context.
- **`spawn_detached(func)` — explicit fire-and-forget.** Not tied to the actor's lifetime, never cancelled on kill, and its callable receives a plain `qb::CoroContext`. Reach for it only when the work must deliberately outlive its actor.

Both return immediately and **share the same safety contract**, because a coroutine frame can still be running a step while — or just after — its actor is destroyed: <!-- src: qb/src/qb/core/Actor.h:1160-1176,1207,1244 -->

- **Never access actor members after a `co_await`.** The actor may have been destroyed while the coroutine was suspended; touching `this->_member` afterwards is undefined behaviour.
- **Copy everything you need by value before the first `co_await`.** Do not capture `this` or a reference to a member.
- **After suspension, use only the context.** `ctx.push<Event>(...)` (to the spawning actor), `ctx.push_to<Event>(dest, ...)`, `ctx.broadcast<Event>(...)`, `ctx.id()` and `ctx.time()` are safe; an event addressed to an actor that is gone finds no handler and is disposed. <!-- src: qb/src/qb/core/VirtualCore.h:1049-1067 -->
- **Keep coroutines short-lived.** The longer one runs, the wider the window in which its actor can be destroyed.

```cpp
// Safe: copy state out, await, answer only through the context.
void on(RequestEvent &ev) {
    std::string key    = ev.key;            // copy ALL needed data BEFORE spawning
    qb::ActorId sender = ev.getSource();

    spawn([key, sender](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
        auto reply = co_await fetch(key);              // the actor may die here
        ctx.push_to<ResultEvent>(sender, reply);       // safe: the id was captured by value
    });
}
```

```cpp
// DANGEROUS — the footgun is identical for spawn and spawn_detached.
spawn([this](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
    co_await ctx.sleep(100ms);   // the actor may die while suspended
    this->_member = value;       // UNDEFINED BEHAVIOUR: the actor may be gone
});
```

`Actor::has_active_coroutines()` and `active_coroutine_count()` report whether suspended coroutines are still outstanding — useful before deciding to `kill()`. The coroutine scheduler is owned by the core's `listener`, one per `VirtualCore`, but it is **not** built when the listener is: `listener::coro_scheduler()` creates it on first access. `spawn` / `spawn_detached` bind to whichever scheduler is current on the calling thread and fall back to that accessor when none exists yet, so both require no setup beyond running inside the engine. <!-- src: qb/src/qb/core/Actor.h:1268-1270,1304-1305; qb/src/qb/io/async/listener.h:881,885-892; qb/src/qb/core/Actor.cpp:240-263 -->

What a `kill()` does to a coroutine that is already parked is on [Writing actors](../4_qb_core/actor.md#killed-while-parked) (the actor-lifecycle half) and [C++20 coroutines](../3_qb_io/coroutines.md#safe-integration-with-qbactor) (which awaiters are cancellation-aware).

## `defer` — continue after the handler unwinds

`qb::io::async::defer(func)` queues `func` to run **once, at the tail of the current loop turn**, after every watcher for that turn has returned. It is the primitive an actor uses to move work off the current handler frame without a timer — and the only one guaranteed never to run re-entrantly. Use it whenever a handler must destroy or replace the object it is running on; a reconnect that frees and recreates its connection is the canonical case.

```cpp
// Declared in qb/src/qb/io/async/listener.h
namespace qb::io::async {
    template <typename _Func>
    void defer(_Func &&func);          // tail of this loop turn — never re-entrant
}
```
<!-- src: qb/src/qb/io/async/listener.h:1032 (async::defer); qb/src/qb/io/async/listener.h:813 (listener::defer) -->

Captured state is released when the callback fires or when the loop is torn down, whichever comes first, so a `shared_ptr` capture is leak-free. Same-thread only. `defer()` does **not** keep the actor alive — the liveness guard below applies to it exactly as it does to a delayed `callback`.

## `callback` — delayed actions, and the overload that is not delayed at all

`qb::io::async::callback(func, delay)` arms a one-shot timer on the calling thread's event loop. Despite the name, the **no-delay overload does not schedule anything**.

```cpp
// Declared in qb/src/qb/io/async/io.h
namespace qb::io::async {
    template <typename _Func>
    void callback(_Func &&func);                          // runs func() INLINE, right now

    template <typename _Func, typename Rep, typename Period>
    void callback(_Func &&func, std::chrono::duration<Rep, Period> timeout);
}
```

Key facts, each verified against the header:

- **The delay is a `std::chrono` duration, not a `double`.** Pass `200ms`, `std::chrono::seconds(5)`, or any `std::chrono::duration`. There is no seconds-as-`double` overload. <!-- src: qb/src/qb/io/async/io.h:372-374 -->
- **A non-positive (or absent) delay fires inline, immediately.** `callback(f)` and `callback(f, d)` with `d <= 0` invoke `func()` synchronously at the call site — they do *not* defer to the next iteration. To run after the current handler unwinds, use `defer(f)`; do not reach for `callback(f, 1ms)`, which only hides the re-entrancy behind a timer. <!-- src: qb/src/qb/io/async/io.h:348-364,368-369,376-378 -->
- **The callback runs on the same `VirtualCore`** that scheduled it, so it may touch that actor's state — but only if the actor is still alive when it fires.
- **Fire-and-forget.** The scheduled `Timeout<F>` is heap-allocated and deletes itself after firing; there is no handle to cancel it. When you need cancellation, use `scoped_callback`.

### Capture safety: the actor may be gone

A delayed callback can outlive the actor that scheduled it, and **no guard written inside the lambda can repair that**. The `Timeout<F>` allocated by `callback(func, delay)` is registered as a *loop-owned* object: the listener owns it, it deletes itself when it fires, and nothing binds it to any `qb::Actor`. It fires when the loop says so, whatever happened to the actor meanwhile. <!-- src: qb/src/qb/io/async/io.h:312-318,343 -->

So the check that looks like the remedy is itself the defect:

```cpp
// WRONG — do not copy this shape.
qb::io::async::callback([this, task_id]() {
    if (!is_alive())
        return;                       // too late: evaluating this IS the invalid access
    // ... members ...
}, std::chrono::seconds(5));
```

`Actor::is_alive()` is a plain read of the actor's `_alive` member. If the actor was already destroyed when the timer fires, *evaluating the guard* is the heap-use-after-free — the branch never gets the chance to protect anything. That is measured, not theoretical: a pre-3.0 example carried exactly this shape at eight sites and AddressSanitizer aborted on it, 3 runs of 3. That program has since been retired; the shape has not. <!-- src: qb/src/qb/core/Actor.cpp:205-208 -->

Two arguments are commonly offered for the guard, and neither holds:

- *"The callback runs on the actor's own core, so capturing `this` is safe — there is no cross-thread access."* This answers the wrong question. The hazard is **lifetime**, not threading; running on the right thread says nothing about whether the object still exists.
- *"The guard covers the killed-but-not-yet-reaped window, which is the one that matters."* Backwards for a *delayed* callback. `kill()` only flags, but `VirtualCore` reaps in the same or the next loop turn — it unregisters the actor's callbacks and destroys it right there. A 5-second timer fires long after the reap. The guard covers microseconds; the hazard window is the whole delay. <!-- src: qb/src/qb/core/VirtualCore.cpp:736,898 -->

**The fix is to bind the delay to the actor's lifetime instead of guarding after the fact.** `Actor::spawn` runs a coroutine under the actor's cancellation scope, and `kill()` cancels that scope, so a pending `ctx.sleep` unwinds rather than resuming into a destroyed actor:

```cpp
// RIGHT — the delay is cancelled when the actor is killed, and nothing captures `this`.
void arm(int task_id) {
    spawn([task_id](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
        co_await ctx.sleep(std::chrono::seconds(5));   // cancelled if this actor is killed
        ctx.template push<DeadlineElapsed>(task_id);   // back on the actor's own frame
    });
}
```
<!-- src: qb/src/qb/core/Actor.h:1243-1244,1722-1724, qb/src/qb/core/Actor.cpp:283-289 -->

Copy by value everything the body needs before the first `co_await`, and **never capture `this`**. The `ScopedCoroContext` carries the actor's `ActorId` by value, so the only way back into the actor is a `push` — which is exactly the message-back pattern, now with no member access at all. Handle the event in an ordinary `on(Event&)` handler and every state change happens on an actor the dispatcher has already proved is alive.

That distinction is why the coroutine form supersedes the older advice to "have the callback `push` to `id()`". Pushing back to self is the right destination, but `push` and `id()` called from inside a `[this]`-capturing, loop-owned timer are still member calls on a possibly-dead actor — the message-back only becomes safe once the id travels by value, which is what `ctx` does.

When you genuinely want a timer you can cancel by hand rather than a coroutine, the other lifetime-bound answer is `scoped_callback` held as an actor member: the actor's destructor destroys the handle, which cancels the pending call. See [`scoped_callback` — when you need the handle back](#scoped_callback--when-you-need-the-handle-back) below.

### A timeout-watchdog example

This actor starts an operation and arms a timeout. The deadline is a coroutine, so nothing survives the actor; when it elapses the coroutine sends the actor a `DeadlineElapsed` event, and an ordinary handler — running on an actor the dispatcher has proved is alive — consults `_pending` and decides whether the operation really timed out.

```cpp
// Operation-timeout pattern. The delay lives in a coroutine bound to this actor's
// cancellation scope; the member map is only ever touched from an event handler.
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

// Self-addressed: "the deadline for task N has elapsed".
struct DeadlineElapsed : qb::Event {
    int task_id;
    explicit DeadlineElapsed(int id) : task_id(id) {}
};

class WatchdogActor : public qb::Actor {
    std::map<int, bool> _pending;   // task_id -> still pending
    int                 _next_id = 1;

public:
    qb::io::async::task<bool> onInit() override {
        registerEvent<TaskComplete>(*this);
        registerEvent<DeadlineElapsed>(*this);
        registerEvent<qb::KillEvent>(*this);
        startOperation(5s);
        co_return true;
    }

    void startOperation(qb::duration timeout) {
        const int id = _next_id++;
        _pending[id] = true;
        // ... kick off the real non-blocking operation here ...

        // Copy `id` and `timeout` by value; capture nothing else, and never `this`.
        spawn([id, timeout](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            co_await ctx.sleep(timeout);              // cancelled if the actor is killed
            ctx.template push<DeadlineElapsed>(id);   // resume on the actor's own frame
        });
    }

    // The deadline decision happens here, in an ordinary handler — `_pending` is a
    // member and is never read from a coroutine frame.
    void on(const DeadlineElapsed &ev) {
        auto it = _pending.find(ev.task_id);
        if (it != _pending.end() && it->second) {     // still pending -> timed out
            _pending.erase(it);
            push<TaskComplete>(id(), ev.task_id, true);
        }
    }

    void on(const TaskComplete &ev) {
        qb::io::cout() << "task " << ev.task_id
                       << (ev.timed_out ? " timed out\n" : " completed\n");
    }

    void on(const qb::KillEvent &) { kill(); }
};
```

Three things the coroutine form buys here. The `co_await ctx.sleep(timeout)` is cancelled by `kill()`, so a dying actor does not leave a five-second timer armed against it. Nothing captures `this`, so there is no member access to get wrong. And `_pending` — a `std::map` whose iterators the coroutine would otherwise be holding across a suspension — is only ever reached from a handler, where the actor is live by construction. <!-- src: qb/src/qb/core/Actor.h:1243-1244,1722-1724, qb/src/qb/core/Actor.cpp:283-289, examples/01-actors/06-doing-things-later.cpp:237-250 -->

`startOperation` takes a `qb::duration` — the canonical span type used for every timeout, delay and interval in the public API. It is an alias for `std::chrono::nanoseconds` and accepts any finer-or-equal chrono literal implicitly (`5s`, `200ms`), while rejecting a bare integer at compile time. <!-- src: qb/src/qb/system/time.h:90 -->

### Common uses

Each of these is a *delay owned by an actor*, so each is a `spawn` + `co_await ctx.sleep(...)` that comes back through a self-addressed event — the form shown above.

- **Operation timeouts.** Arm the deadline when you start something; the handler that receives the elapsed event drops it if a pending flag was already cleared by the real result, as above.
- **Retry with backoff.** On a failed attempt, sleep for a growing delay and then push yourself a retry event.
- **Yielding between steps.** Split a long computation into chunks and sleep between them so the loop can service other actors. Use a strictly positive delay — a zero delay yields nothing. (`co_await ctx.cancellation_point()` is the cheaper way to yield inside one coroutine's loop.)
- **Periodic work.** The tick handler re-arms the next sleep. For strictly every-iteration work, prefer `qb::ICallback`.

Reach for a bare `qb::io::async::callback(func, delay)` only for work that must deliberately outlive the actor that scheduled it — that is the one job the loop-owned timer is right for.

## `scoped_callback` — when you need the handle back

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
                // `this` is sound here — and ONLY here — because the timer is a member:
                // it cannot outlive the actor. The check is for the killed-but-not-yet-
                // destroyed window, where reading `_alive` is still valid.
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

`ScopedTimeout` exposes `cancel()` and `fired()`. As with `callback`, a non-positive duration fires inline at construction. The trade-off: `scoped_callback` is cancellable and owned by you; `callback` is fire-and-forget and self-cleaning. Owning the handle as an actor member is also the strongest form of the liveness guard, because the actor's own destructor cancels the watcher. <!-- src: qb/src/qb/io/async/io.h:433-437,428-431; qb/src/qb/io/async/io.h:410,415-424 -->

## `with_timeout<T>` — inactivity, not a deadline

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

- The constructor takes a `qb::duration` and **defaults to `std::chrono::seconds(3)`**. A value `<= 0` starts disabled. <!-- src: qb/src/qb/io/async/io.h:121-124 -->
- `updateTimeout()` records "now" as the last-activity time; call it from handlers that count as activity to push the deadline forward. It does not re-arm the watcher, so it is cheap enough to call on every byte received.
- When the configured span elapses with no `updateTimeout()`, the mixin invokes `_Derived::on(qb::io::async::event::timer const&)`. The canonical handler signature takes the event by `const &`. <!-- src: qb/src/qb/io/async/io.h:184-185; qb/tests/io/system/async/timer-timeout.cpp:91 -->
- `setTimeout(d)` reconfigures and restarts the timer; `setTimeout(qb::duration::zero())` stops it. <!-- src: qb/src/qb/io/async/io.h:148-157 -->

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

`with_timeout` is the same mixin network session classes use for idle-connection cleanup; the message-broker server's `BrokerSession` closes its connection from exactly this handler. <!-- src: examples/05-services/02-pubsub-broker/server/BrokerSession.cpp:156-157 -->

> **`with_timeout` versus `callback`.** `with_timeout` models *inactivity* — a deadline that resets on activity and re-arms itself until the real deadline. `callback`/`scoped_callback` model a *one-shot* deferral at a fixed delay. Reach for `with_timeout` when "reset the clock on every message" is the natural description, and a callback when "do X once, T from now" is.

## Choosing between them

| You want | Use | Because |
|---|---|---|
| to continue after this handler returns | `defer(f)` | tail of this turn, never re-entrant, safe to destroy the object you are running on |
| to run something T from now, once | `callback(f, T)` | a real timer; `T` must be strictly positive |
| the same, but cancellable | `scoped_callback(f, T)` | you own the handle; `reset()` cancels |
| to act after a span of *silence* | `with_timeout<T>` | resets on activity, re-arms itself |
| something every loop pass | `qb::ICallback` + `registerCallback(*this)` | one `on(qb::LoopEvent const&)` per pass ([registration and the tick's position](../4_qb_core/actor.md#periodic-work-qbicallback)) |
| to await an asynchronous result | `spawn` + `co_await` | the stack unwinds; the core keeps serving |
| to await from a `main()` or a test | `run_sync` | your thread, yours to block |

## Periodic work: `qb::ICallback`

For logic that must run on *every* loop pass — polling, draining a queue, heartbeats — inherit `qb::ICallback` alongside `qb::Actor` and register with `registerCallback(*this)`. The core calls `on(qb::LoopEvent const&)` once per pass until you call `unregisterCallback()`. The `qb::LoopEvent` carries per-pass context (`now`, `iteration`).

```cpp
// Declared in qb/src/qb/core/ICallback.h
class ICallback {
public:
    virtual void on(qb::LoopEvent const &) = 0;   // called every loop pass while registered
};
```

`on(qb::LoopEvent const&)` is bound by the same no-blocking rule as everything else on the core: it must return quickly and must never block, sleep, or do synchronous I/O. <!-- src: qb/src/qb/core/ICallback.h:163-165 --> The tick fires *after* the pass has flushed its pipes and dispatched its events, so anything it pushes leaves the core on the next pass — see [the loop pass](../4_qb_core/engine.md#the-loop-pass). For a one-shot or backoff schedule rather than every-pass work, prefer `spawn` + `co_await ctx.sleep(...)`; for the registration API and a worked heartbeat example, see [Writing actors](../4_qb_core/actor.md#periodic-work-qbicallback).

## Blocking file I/O from an actor

Synchronous file I/O (`qb::io::sys::file::read` / `write`) blocks the calling thread, and an actor's thread is its whole `VirtualCore`. This is a genuine capability gap rather than an oversight — `async::file` watches metadata by polling and then performs a blocking read, and [what has no coroutine form](../3_qb_io/gaps.md#file-io-is-polled-metadata-plus-a-blocking-read) explains why. Three patterns keep the core responsive, in increasing order of isolation:

1. **Wrap the blocking call in `async::callback`** (suitable for infrequent, non-critical I/O). The callback still blocks the core *for its own turn*, but it keeps the blocking work out of the actor's message-handling path. When the I/O finishes, `push` a result event back to the requester. This is what the `qb-example-services-file-pipeline` worker does. <!-- src: examples/05-services/03-file-pipeline/file_worker.h:112 -->

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

2. **Dedicate worker actors to I/O.** Place file-I/O actors on their own core(s) and delegate requests to them as events. Blocking is then confined to that core, leaving the rest of the system unaffected. The `qb-example-services-file-pipeline` example builds exactly this manager-worker topology. <!-- src: examples/05-services/03-file-pipeline/main.cpp:280,286-290 -->

3. **Watch the filesystem instead of polling it.** To *react* to file or directory changes, use `qb::io::async::file_watcher<T>` / `directory_watcher<T>`, which deliver `on(qb::io::async::event::file const&)` notifications through the loop with no blocking. `examples/02-io/08-timeouts-and-watchers.cpp` demonstrates both watchers, and is candid about the limit: a directory event says THAT something changed, never WHAT. <!-- src: examples/02-io/08-timeouts-and-watchers.cpp:194,204 -->

## Pitfalls

- **`run_sync` / `run_for` inside a handler.** The thread you block is the `VirtualCore`. The framework's guard does not fire, the loop keeps turning so the I/O looks healthy, and the only symptom is actor latency. Use `spawn` + `co_await`.
- **Expecting `callback(f)` or a zero delay to defer.** A non-positive duration runs `func()` inline at the call site. Use a strictly positive delay, or `defer(f)`.
- **Passing a `double` as a delay.** `callback`, `with_timeout` and `connect` take `qb::duration` (a `std::chrono` span), never seconds-as-`double`. Write `5s` or `std::chrono::milliseconds(200)`; a bare integer does not compile. <!-- src: qb/src/qb/system/time.h:90 -->
- **Touching `this` after the actor died.** A delayed `callback` outlives its actor, and `is_alive()` inside the closure does not save you — reading `_alive` on a destroyed actor *is* the invalid access. Bind the delay to the actor instead: `spawn` + `co_await ctx.sleep(...)` + a self-addressed event, or own a `scoped_callback` handle as a member so the destructor cancels it. <!-- src: qb/src/qb/core/Actor.cpp:205-208 -->
- **Accessing actor state after `co_await`.** Copy state by value before the first suspension and use only the `CoroContext` afterwards.
- **Sharing I/O objects across cores.** An async object is bound to one core's loop. Never hand it to another thread; move a socket into an event instead. <!-- src: qb/src/qb/io/async/listener.h:66-67,69-81 -->
- **Blocking the loop at all.** A synchronous `read`, a `sleep`, a mutex wait, or an unbounded computation in a handler, a callback or an `on(qb::LoopEvent const&)` tick freezes the entire core. Defer it, chunk it, or offload it to a worker actor.

## See also

- [The async runtime](../3_qb_io/async_system.md) — the loop turn these primitives sit inside, and the owner of the `run_sync` rule.
- [C++20 coroutines](../3_qb_io/coroutines.md) — `task<T>`, the combinators (`when_all`, `when_any`, `race`), channels, and which awaiters are cancellation-aware.
- [Network actors](./network_actors.md) — the same core, driving TCP, UDP and TLS endpoints through `qb::io::use<T>`.
- [Writing actors](../4_qb_core/actor.md) — `qb::ICallback` registration, and what a `kill()` does to a coroutine that is already parked.
- [The engine](../4_qb_core/engine.md) — the pass whose step 6 dispatches your handler.
- [The time vocabulary](../0_foundations/time.md) — `qb::duration`, `qb::mono_time`, `qb::wall_time`.
