# Patterns cookbook

> **Audience:** Adopter · **Status:** stable · **Verified-against:** qb 3.0.0 (C++20 default, C++23 supported)

Task-oriented recipes for the interactions you reach for most often: one-shot and periodic timers, request/reply, actor-scoped coroutines, coroutine `ask`, typed request/response with scatter-gather and saga, resilient ask (retry & circuit breaker), worker pools and pub/sub, supervision and restart strategies, broadcast fan-out, multi-stage pipelines, and graceful shutdown — each a complete, compilable snippet.

**Prerequisites:** [Writing actors with `qb::Actor`](../4_qb_core/actor.md), [Event messaging](../4_qb_core/messaging.md) — **See also:** [Actor patterns](../4_qb_core/patterns.md), [Asynchronous operations inside actors](../5_core_io_integration/async_in_actors.md), [Error handling and resilience](./error_handling.md)

## Summary

This page is a recipe collection. Each recipe states the task, shows a self-contained snippet, and
calls out the one or two pitfalls that bite in practice. It does not re-derive the `qb::Actor` API —
the send primitives (`push`, `send`, `broadcast`, `reply`, `forward`, `to`), `qb::ICallback`, and the
lifecycle hooks are defined on [the actor page](../4_qb_core/actor.md), and the structural patterns
they compose into (finite state machines, service registries, publish/subscribe, request/response
with a timeout, supervision, discovery) live on [the actor patterns page](../4_qb_core/patterns.md).
Reach for the patterns page when you are choosing an architecture; reach for this page when you know
the shape and need the few lines that implement it.

## Concepts

Every recipe rests on the same invariants, covered in full on [the threading model
page](../2_core_concepts/threading_model.md):

- **One handler at a time.** An actor processes one event before the next, on a single
  `VirtualCore` thread, so its members need no locking.
- **Ordered, per-destination delivery.** `push<E>(dest, …)` events to the same destination from the
  same source arrive in send order; `send<E>(dest, …)` is unordered and restricted to
  trivially-destructible events.
- **`kill()` only flags.** Termination sets `_alive = false`; destruction happens later under
  `VirtualCore` control. That deferral is what makes shutdown sequencing tractable.

Three timing tools from `qb-io` recur throughout, in order of preference:

- `spawn(f)` + `co_await ctx.sleep(delay)` is the default. `f` takes a `qb::ScopedCoroContext` and
  runs under the actor's cancellation scope, so `kill()` cancels a pending sleep instead of letting it
  resume into a destroyed actor (`qb/src/qb/core/Actor.h`, `qb/src/qb/core/Actor.cpp`).
- `qb::io::async::scoped_callback(func, delay)` returns a caller-owned `std::unique_ptr<ScopedTimeout<…>>`
  whose destruction cancels the pending callback. Reach for it when you want a timer *handle* — held as
  an actor member, the actor's own destructor cancels it (`qb/io/async/io.h`).
- `qb::io::async::callback(func, delay)` schedules `func` on the actor's own `VirtualCore` loop after
  `delay`, a `std::chrono::duration` (`qb/io/async/io.h`). A non-positive `delay` — or the no-duration
  overload `callback(func)` — runs `func` immediately and inline. The timer is one-shot, owned by the
  event loop, and deletes itself after firing.

That last one is the sharp edge, and it is why the recipes below never use it for actor work. Its
timer holds **no claim on any actor**: it fires when the loop says so, whatever happened to the actor
meanwhile. Capturing `this` in it is a use-after-free waiting on timing, and adding `if (!is_alive())`
does not help — `is_alive()` reads an actor member, so on a destroyed actor *evaluating the guard is
itself the invalid access*. The rule for anything deferred out of an `on()` handler is therefore:
**copy by value what the body needs, never capture `this`, and come back through a self-addressed
event handled in an ordinary `on()`.** Use a bare `callback` only for work that must deliberately
outlive its actor. See [the `async::callback` lifetime rules](./error_handling.md) and
[Capture safety](../5_core_io_integration/async_in_actors.md#capture-safety-the-actor-may-be-gone) for
the full contract.
<!-- src: qb/src/qb/core/Actor.h:1238-1239,1717-1719, qb/src/qb/core/Actor.cpp:205-208,283-289, qb/src/qb/io/async/io.h:312-318,343 -->

## Recipe: one-shot timer

**Task.** Run an action once, after a delay, from inside an actor.

Spawn a coroutine that sleeps and then posts a self-event. Routing the deferred work back through an
event (rather than doing it in the lambda) keeps it on the actor's single-threaded handler path, where
member access is safe — and because `ctx` carries the `ActorId` **by value**, the coroutine never needs
`this` at all.

```cpp
// src: derived from qb/tests/core/system/coroutine/coroutine-scope.cpp
#include <qb/actor.h>
#include <qb/main.h>
#include <qb/io.h>
#include <qb/io/async.h>
#include <chrono>

using namespace std::chrono_literals;

struct Tick : qb::Event {};

class OneShotActor : public qb::Actor {
public:
    qb::io::async::task<bool> onInit() override {
        registerEvent<Tick>(*this);
        // Fire Tick on ourselves 200 ms from now. Cancelled if we are killed first.
        spawn([](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            co_await ctx.sleep(200ms);
            ctx.template push<Tick>();
        });
        co_return true;
    }

    void on(const Tick &) {
        qb::io::cout() << "timer fired\n";
        kill();                             // one-shot work is done
    }
};

int main() {
    qb::Main engine;
    engine.addActor<OneShotActor>(0);
    engine.start();
    engine.join();
    return 0;
}
```

**Pitfalls.**

- The coroutine body runs outside any handler, so it must reach the actor only through `ctx`. Capture
  by value before the first `co_await` and never capture `this` — an `is_alive()` guard cannot make a
  member access safe, because reading `_alive` on a destroyed actor is already the invalid access. A
  `ctx.push` to a dead actor is harmless: the event is dropped.
- `spawn` registers with *the calling thread's* scheduler. Call it from an actor handler (or `onInit`),
  not from `main` or another thread, so the sleep lands on the actor's own `VirtualCore`.

### Cancellable variant

When you must be able to cancel the timer (a deadline that the response may beat), use
`scoped_callback` and hold the returned handle. Destroying or reassigning it stops the pending
callback.

```cpp
// src: derived from qb/src/qb/io/async/io.h (ScopedTimeout / scoped_callback)
#include <qb/actor.h>
#include <qb/io.h>
#include <qb/io/async.h>
#include <chrono>
#include <memory>

using namespace std::chrono_literals;

struct Deadline : qb::Event {};
struct Response : qb::Event {};

class DeadlineActor : public qb::Actor {
    // Owns the timer; resetting/destroying it cancels the callback.
    std::unique_ptr<qb::io::async::ScopedTimeout<std::function<void()>>> _deadline;

public:
    qb::io::async::task<bool> onInit() override {
        registerEvent<Deadline>(*this);
        registerEvent<Response>(*this);
        // `this` is sound here — and only here — because the timer is a member and so
        // cannot outlive the actor; the guard covers the killed-but-not-yet-destroyed
        // window, where reading `_alive` is still valid.
        _deadline = qb::io::async::scoped_callback(std::function<void()>([this]() {
            if (is_alive())
                push<Deadline>(id());
        }), 500ms);
        co_return true;
    }

    void on(const Response &) {
        _deadline.reset();                  // response won the race: cancel the deadline
        kill();
    }

    void on(const Deadline &) {
        qb::io::cout() << "deadline elapsed before response\n";
        kill();
    }
};
```

## Recipe: periodic work

**Task.** Run an action on every loop iteration (a heartbeat, a poll, a drain step).

Inherit from `qb::ICallback` and register it. `on(qb::LoopEvent const&)` is invoked once per `VirtualCore` loop
pass — after that pass has flushed its outbound pipes *and* drained its mailbox, so anything the tick pushes
leaves the core on the **next** pass ([the loop pass](../4_qb_core/engine.md#the-loop-pass)). The `qb::LoopEvent`
carries per-pass context (`now`, `iteration`). It runs on the
event-loop thread, so it must be fast and non-blocking.

```cpp
// src: derived from qb/tests/core/system/actor/actor-callback.cpp
#include <qb/actor.h>
#include <qb/icallback.h>
#include <qb/main.h>
#include <qb/io.h>

class HeartbeatActor : public qb::Actor, public qb::ICallback {
    uint64_t _ticks = 0;
public:
    qb::io::async::task<bool> onInit() override {
        registerCallback(*this);            // start the per-iteration tick
        co_return true;
    }

    void on(qb::LoopEvent const &) override {
        if (++_ticks >= 1000) {
            unregisterCallback();           // stop ticking
            kill();
        }
    }
};

int main() {
    qb::Main engine;
    engine.addActor<HeartbeatActor>(0);
    engine.start();
    engine.join();
    return 0;
}
```

**Periodic at a fixed interval.** `on(qb::LoopEvent const&)` fires as fast as the loop turns, which is not a fixed
period. For a steady wall-clock interval, chain self-scheduled coroutine sleeps instead — the tick
handler re-arms the next one:

```cpp
// src: derived from qb/tests/core/system/coroutine/coroutine-scope.cpp
#include <qb/actor.h>
#include <qb/io/async.h>
#include <chrono>

using namespace std::chrono_literals;

struct PollNow : qb::Event {};

class PollingActor : public qb::Actor {
public:
    qb::io::async::task<bool> onInit() override {
        registerEvent<PollNow>(*this);
        push<PollNow>(id());                // kick off the first cycle
        co_return true;
    }

    void on(const PollNow &) {
        // ... do one unit of polling work ...
        arm_next();
    }

private:
    void arm_next() {                       // re-arm the next tick at a fixed delay
        spawn([](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            co_await ctx.sleep(1s);
            ctx.template push<PollNow>();
        });
    }
};
```

The chain stops itself: `kill()` cancels the actor's coroutine scope, so a parked `ctx.sleep` unwinds
rather than pushing one more tick at a dead actor. A `callback`-based chain has no such property — its
timer is loop-owned, so the last one armed still fires, and `[this]` inside it is a use-after-free.
<!-- src: qb/src/qb/core/Actor.cpp:283-289 -->

**Pitfalls.**

- `on(qb::LoopEvent const&)` blocks the whole core while it runs. Never sleep, wait on a mutex, or do synchronous
  I/O inside it.
- `qb::ICallback` frequency depends on loop rate and the configured idle latency
  (`CoreInitializer::setLatency`), not a clock. Use the self-scheduled `ctx.sleep` variant when the
  interval must be predictable.

## Recipe: request/reply

**Task.** Send a request to another actor and handle its response, with the response routed back to
the original sender automatically.

Reuse the request event for the reply. The responder takes the event by **non-const reference**,
fills in the result, and calls `reply(event)`, which swaps the event's source and destination so it
returns to the requester. No bookkeeping of `ActorId`s is needed.

```cpp
// src: derived from qb/tests/core/system/messaging/messaging-reply-forward.cpp (reply/forward)
#include <qb/actor.h>
#include <qb/main.h>
#include <qb/io.h>

struct Query : qb::Event {
    int    input  = 0;
    int    result = 0;              // filled in by the responder
    explicit Query(int v) : input(v) {}
};

class Responder : public qb::Actor {
public:
    qb::io::async::task<bool> onInit() override {
        registerEvent<Query>(*this);
        co_return true;
    }

    void on(Query &event) {         // non-const: reply() mutates and consumes it
        event.result = event.input * event.input;
        reply(event);               // returns the event to its source
    }
};

class Requester : public qb::Actor {
    qb::ActorId _responder;
public:
    explicit Requester(qb::ActorId responder) : _responder(responder) {}

    qb::io::async::task<bool> onInit() override {
        registerEvent<Query>(*this);
        push<Query>(_responder, 7); // request
        co_return true;
    }

    void on(Query &event) {         // the reply arrives as the same event type
        qb::io::cout() << "result = " << event.result << '\n';
        push<qb::KillEvent>(_responder);
        kill();
    }
};

int main() {
    qb::Main engine;
    auto responder = engine.addActor<Responder>(0);
    engine.addActor<Requester>(0, responder);
    engine.start();
    engine.join();
    return 0;
}
```

**Pitfalls.**

- The handler must take the event by non-const reference (`Query &`). `reply` and `forward` mutate the
  event in place; a `const` parameter will not compile against them.
- After `reply(event)` (or `forward(dest, event)`) the event object is consumed — do not read or
  modify it afterward.
- `reply` is the most allocation-light response path because it reuses the inbound event. When the
  responder needs a different event type, send a fresh one to `event.getSource()` instead.
- For a request that may never be answered, add a deadline with the [cancellable timer
  recipe](#cancellable-variant). The full request/response-with-timeout pattern is on [the actor
  patterns page](../4_qb_core/patterns.md).

### Forwarding to a worker

`forward(dest, event)` re-routes a received event to a new destination while **preserving the
original source**, so a worker's eventual `reply` still reaches the first sender — the forwarder drops
out of the return path.

```cpp
// src: derived from qb/tests/core/benchmark/messaging/forward-vs-direct.cpp
void on(Query &event) {
    forward(_worker_id, event);     // worker's reply goes back to event.getSource(), not here
}
```

## Recipe: actor-scoped coroutine (cancelled on kill)

**Task.** Run async work inside an actor so it is **automatically cancelled when the actor is killed**,
instead of leaving it blocked on a long timeout or I/O.

`spawn` binds the coroutine to a per-actor cancellation scope. The lambda receives a
`qb::ScopedCoroContext` whose `sleep` / `until_cancelled` / `cancellation_point` / `cancellable`
helpers are cancellation-aware: when the actor is killed, the coroutine wakes within the next loop
iteration, throws `qb::io::async::cancelled_error`, and unwinds cleanly. Capture **by value** — never
`this`.

```cpp
// src: derived from qb/tests/core/system/coroutine/coroutine-scope.cpp
#include <qb/actor.h>
#include <qb/main.h>
#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>
using namespace std::chrono_literals;

struct Result : qb::Event { int value = 0; };

class Worker : public qb::Actor {
public:
    qb::io::async::task<bool> onInit() override {
        registerEvent<Result>(*this);
        spawn([](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            co_await ctx.sleep(2s);          // cancelled instantly if the actor is killed
            ctx.push<Result>();              // talk back only through ctx, never `this`
        });
        co_return true;
    }
    void on(const Result &) { kill(); }
};
```

**Pitfalls.**

- `spawn_detached` (the low-level form) is **not** cancelled on kill — its coroutine stays detached and
  runs to completion. Use it only for fire-and-forget work that must outlive the actor.
- Cancellation is cooperative: a bare `qb::io::async::sleep` or raw socket await inside a scoped
  coroutine is *not* interrupted. Await the `ctx.*` helpers (or wrap with `ctx.cancellable(task)`).
- Use `ctx.until_cancelled()` for a coroutine that only waits to be told to stop (no timer allocated),
  and `co_await ctx.cancellation_point()` between iterations of a compute loop.

## Recipe: coroutine ask (linear request/response)

**Task.** Send a request and `co_await` the reply on one line — with timeout and cancel-on-kill —
instead of hand-rolling correlation ids and pending-state bookkeeping (compare the callback-style
[request/reply recipe](#recipe-requestreply) above).

The exchange uses a **single event type** deriving from `qb::AskEvent`. The responder fills the
response fields and `reply()`s it back (preserving the correlation id stamped by `ask`); the asker
routes replies by calling `resolve_ask(e)` at the top of its own `on(E&)`.

```cpp
// src: derived from qb/tests/core/system/coroutine/ask-roundtrip.cpp
#include <qb/patterns.h>   // qb::ask, qb::answer, … (pulls in qb/actor.h)
#include <qb/main.h>
#include <qb/io/async/coroutine.h>
using namespace std::chrono_literals;

struct PriceQuery : qb::AskEvent {      // single request/response envelope
    int query = 0;
    int price = 0;                        // filled by the responder
    PriceQuery() = default;
    explicit PriceQuery(int q) : query(q) {}
};

class Market : public qb::Actor {        // responder
public:
    qb::io::async::task<bool> onInit() override { registerEvent<PriceQuery>(*this); co_return true; }
    void on(PriceQuery &q) { q.price = q.query * 2; reply(q); }   // reply preserves correlation_id
};

class Trader : public qb::Actor {        // asker
    qb::ActorId _market;
public:
    explicit Trader(qb::ActorId m) : _market(m) {}
    qb::io::async::task<bool> onInit() override {
        registerEvent<PriceQuery>(*this);
        auto mkt = _market;
        spawn([mkt](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            try {
                auto r = co_await qb::ask(ctx, mkt, PriceQuery{21}, 500ms);
                qb::io::cout() << "price = " << r.price << '\n';   // 42
            } catch (const qb::io::async::timeout_error &)   { /* no reply in time */ }
              catch (const qb::io::async::cancelled_error &) { /* actor was killed   */ }
            qb::Main::stop();
        });
        co_return true;
    }
    void on(PriceQuery &e) { if (resolve_ask(e)) return; /* else: unsolicited request */ }
};
```

**Pitfalls.**

- The asker **must** call `resolve_ask(e)` in its `on(E&)` handler; otherwise replies are never
  delivered and every `ask` times out.
- `ask` must be awaited from a `spawn` coroutine (it uses the actor's cancellation scope).
- Pass a positive `timeout`; on expiry `ask` throws `qb::io::async::timeout_error`. A `<= 0` timeout
  waits indefinitely (until reply or kill).

## Recipe: typed request/response, scatter-gather & saga

**Task.** Build request/response flows with less boilerplate, fan a request out to many actors, and
orchestrate multi-step transactions that roll back on failure — all on top of `ask`.

These live in the patterns library (`#include <qb/patterns.h>`): `qb::ask`, `qb::answer`,
`qb::ask_all`, `qb::ask_any`, `qb::run_saga` are **free functions** taking the context/actor — the
`Actor`/`ScopedCoroContext` kernel only holds the primitives they compose.

**Typed envelope.** Derive your exchange from `qb::Request<Resp>`: the base supplies the `response`
slot, so the request and its response travel in **one** event type. The responder fills it with the
`qb::answer(*this, e, fn)` helper (it routes its own replies via `resolve_ask` first, then computes + replies).

```cpp
struct Quote : qb::Request<double> { qb::string<16> symbol; };       // request: symbol — response: double
// Two things above are load-bearing:
//   * `qb::string<N>`, NOT `std::string` — an event is memcpy-relocated on every path (pipe
//     growth or compaction, reply/forward, and the cross-core hop), and a short std::string
//     points into its own storage on libstdc++ (see the pitfall at the end).
//   * the DESIGNATED form below (`{.symbol = ...}`). `Request<>` is an aggregate with a base, so
//     a positional `Quote{"BTC"}` initialises the `Event` BASE and does not compile.

// responder:
void on(Quote &q) { qb::answer(*this, q, [](Quote const &r){ return lookup(r.symbol); }); }

// asker (inside a spawn() coroutine):
auto q = co_await qb::ask(ctx, market, Quote{.symbol = "BTC"}, 500ms);
use(q.response);
```

**Scatter-gather.** `ask_all` sends a copy to every target and waits for all replies; `ask_any`
resolves with the first (fastest wins); `ask_quorum` resolves with the first **k** replies (the
majority middle-ground). All throw `timeout_error` if the deadline / quorum can't be met.

```cpp
spawn([markets](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
    auto quotes = co_await qb::ask_all(ctx, markets, Quote{.symbol = "BTC"}, 500ms);   // all N — std::vector<Quote>
    for (auto const &q : quotes) use(q.response);
    auto fastest = co_await qb::ask_any(ctx, markets, Quote{.symbol = "BTC"}, 500ms);  // first reply (k = 1)

    // Quorum: the first k of N (e.g. a majority of replicas); throws if k can't be reached.
    auto majority = co_await qb::ask_quorum(ctx, replicas, replicas.size()/2 + 1, Read{key}, 200ms);

    // Bounded scatter: cap concurrency for a large fan-out (cancel-safe sliding window).
    auto all = co_await qb::ask_all(ctx, many_targets, Probe{}, 200ms, /*max_in_flight*/ 8);
});
```

**Saga.** `run_saga` runs a sequence of steps, each registering a compensation; if a later step
fails, the registered compensations run in **reverse** order before the error propagates. (A kill
before a step fails — `cancelled_error` — aborts hard, without rollback; a kill **during** rollback
stops the remaining compensations cleanly rather than spinning through them.)

```cpp
co_await qb::run_saga(ctx, [inventory, payment](qb::ScopedCoroContext ctx, qb::SagaScope &saga)
                               -> qb::io::async::task<void> {
    co_await qb::ask(ctx, inventory, Reserve{item}, 1s);
    saga.on_compensate([ctx, inventory, item]() -> qb::io::async::task<void> {
        co_await qb::ask(ctx, inventory, Release{item}, 1s);     // undo the reserve
    });
    co_await qb::ask(ctx, payment, Charge{amount}, 1s);          // if this throws, Release runs
});
```

**Pitfalls.**

- A cross-core `ask_all` keeps N requests in flight at once (bounded by `timeout`) — pass a
  `max_in_flight` cap (`ask_all(ctx, targets, req, timeout, cap)`) to bound it; `ask_any`'s and
  `ask_quorum`'s surplus replies (beyond the winner / beyond `k`) are not cancelled — they linger
  until their own `timeout`.
- A field named `id` in a `Request`/`AskEvent` subtype shadows the `Event` type-id field — name request
  fields anything else (`symbol`, `key`, …).
- Compensations are best-effort (a throwing compensation is swallowed so the rest still run) and run on
  the live actor scope — design them to be idempotent.

## Recipe: resilient ask (retry & circuit breaker)

**Task.** Survive flaky responders: retry transient timeouts with backoff, and stop hammering a
responder that is consistently failing.

**Retry with backoff.** `ask_retry` re-sends on timeout up to `max_attempts`, waiting
`backoff * multiplier^(n-1)` (capped at `max_backoff`) between tries. Only timeouts retry; a kill
aborts the loop at once (the backoff waits are cancellation-aware).

```cpp
// jitter (0..1) randomizes each backoff over [backoff*(1-jitter), backoff] to avoid retry storms.
qb::retry_policy policy{ .max_attempts = 5, .backoff = 50ms, .multiplier = 2.0,
                         .max_backoff = 1s, .jitter = 0.2 };
auto r = co_await qb::ask_retry(ctx, market, Quote{.symbol = "BTC"}, 200ms, policy);
```

**Circuit breaker.** A `qb::CircuitBreaker` trips **open** after N consecutive failures, fails fast for
a cooldown, then admits a **half-open** trial. Hold it by `shared_ptr` so the coroutine captures it by
value (it outlives the actor); `ask_guarded` checks it, sends the ask, and records the outcome.

```cpp
// actor member: std::shared_ptr<qb::CircuitBreaker> breaker_ = std::make_shared<qb::CircuitBreaker>(5, 2s);
spawn([breaker = breaker_, market](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
    try {
        auto r = co_await qb::ask_guarded(ctx, breaker, market, Quote{.symbol = "BTC"}, 200ms);
        use(r.response);
    } catch (const qb::circuit_open_error &) {
        // breaker is open — fall back without touching the failing market
    } catch (const qb::io::async::timeout_error &) {
        // counted as a breaker failure
    }
});
```

**Rate limiter.** A `qb::rate_limiter` (token bucket) throttles a call site to a steady rate with a
burst allowance. `acquire(ctx)` waits (cancellation-aware) for a token; share it by `shared_ptr`.

```cpp
// ≤ ~100 calls/s, bursts up to 10:
auto limiter = std::make_shared<qb::rate_limiter>(10.0, 10ms);
spawn([limiter, svc](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
    co_await limiter->acquire(ctx);                 // throttle before the call
    auto r = co_await qb::ask(ctx, svc, Req{}, 1s);
});
```

**Bulkhead.** A `qb::bulkhead` caps the number of *concurrent* calls through a resource so a slow
dependency can't exhaust the core. `enter(ctx)` waits (cancellation-aware) for a slot and returns
an RAII handle freed on scope exit.

```cpp
auto bh = std::make_shared<qb::bulkhead>(8); // at most 8 concurrent calls to `svc`
spawn([bh, svc](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
    auto slot = co_await bh->enter(ctx);            // waits if 8 are already in flight
    auto r    = co_await qb::ask(ctx, svc, Req{}, 1s);
});                                                  // slot frees on scope exit
```

**Deadline budget.** Thread one absolute `qb::deadline` through a chain of `ask_by` so the *whole*
chain is bounded (a per-`ask` `timeout` resets at each hop; a deadline does not).

```cpp
auto dl = qb::deadline_in(ctx, 1s);                       // the whole chain must finish in 1 s
auto a  = co_await qb::ask_by(ctx, svc1, R1{}, dl);
auto b  = co_await qb::ask_by(ctx, svc2, R2{a.response}, dl); // gets only the time svc1 left over
```

**Pitfalls.**

- A success **closes** the breaker, a timeout (or other non-cancellation error) is a **failure**; a kill
  (`cancelled_error`) is *not* counted — it is a controlled shutdown, not a responder fault.
- Capture the breaker / limiter `shared_ptr` **by value** — never a reference to an actor member.
- A `CircuitBreaker` / `rate_limiter` is **core-local**: do not share one across `VirtualCore`s
  (their state is single-thread, unsynchronized by design).
- Compose them by retrying *around* a guarded ask (each guarded attempt also feeds the breaker), and
  `acquire`-ing a limiter token before the ask.

## Recipe: idempotency, batching & streaming

**Idempotency (exactly-once effects).** A retried `ask` re-sends with a fresh `correlation_id`, so a
reply lost to a timeout would run the responder's effect twice. Carry a **stable** `idempotency_key`
on the request (preserved across retries — each attempt copies the request) and de-duplicate on the
responder with `qb::answer_idempotent` + a bounded-LRU `qb::dedup_map`.

```cpp
struct Charge : qb::Request<Receipt> { std::uint64_t idempotency_key{}; double amount{}; };

class Bank : public qb::Actor {
    qb::dedup_map<std::uint64_t, Receipt> _seen{4096};        // bounded LRU of key → response
public:
    qb::io::async::task<bool> onInit() override { registerEvent<Charge>(*this); co_return true; }
    void on(Charge &c) {
        qb::answer_idempotent(*this, c, _seen, [&](Charge const &r){ return do_charge(r); });
    } // first request runs do_charge; a repeat with the same key replays the cached Receipt
};
```

**Batching (aggregate by size or time).** Coalesce many small events into one amortized action with
`qb::batcher<T>` — it flushes when `max` items accumulate *or* a time `window` elapses, whichever
first. The window timer is scope-bound (a kill cancels it).

```cpp
class Writer : public qb::Actor {
    qb::batcher<Row> _batch{128, 50ms, [this](std::vector<Row> &&rows){ db_write(std::move(rows)); }};
public:
    void on(Row &e) { _batch.add(context(), e.row); } // one db_write per 128 rows or per 50 ms
};
```

**Streaming (many replies for one request).** `qb::ask_stream` returns a `qb::stream<E>` you drain
with `co_await s.next()` until end-of-stream. The responder emits chunks with `qb::yield_answer` and
finishes with `qb::end_stream`; the asker routes chunks with `resolve_ask` (they are `AskEvent`s).

```cpp
struct Tail : qb::StreamRequest<LogLine> { qb::string<64> file; };   // qb::string, not std::string

// responder:
void on(Tail &t) {
    for (auto const &line : read(t.file)) qb::yield_answer(*this, t, line);
    qb::end_stream(*this, t);
}
// asker (in a spawn() body or directly in onInit):
auto s = qb::ask_stream(ctx, tailer, Tail{.file = "app.log"}, 1s);
while (auto line = co_await s.next()) print(line->chunk); // nullopt at end-of-stream
// asker's on(Tail&): if (resolve_ask(e)) return;
```

**Pitfalls.**

- A default-valued `idempotency_key` (`{}`) is **never** de-duplicated (always runs the effect) —
  set a stable key to opt in. `dedup_map` is core-local; size it to your retry/dup window.
- `batcher::on_flush` runs on the loop; on an abrupt kill the buffered items are **dropped** — call
  `flush()` from a shutdown handler if you need a final drain.
- A `stream` is **single-consumer**: do not call `next()` concurrently. If the responder outpaces the
  buffer (`capacity`), `next()` throws `qb::stream_overflow_error` rather than dropping silently —
  raise `capacity` or slow the producer. The asker's `on(E&)` must call `resolve_ask(e)`.
- The whole pattern library — `ask`, `ask_all/any/quorum`, `ask_by`, **`ask_stream`**, **`ping`**,
  **`require`** — is usable directly inside `onInit()` (replies reach the *Activating* actor via the
  continuation registry), bounded by `activation_deadline_ns`.

## Recipe: worker pool & pub/sub

**Task.** Spread work across a pool of identical workers, or decouple publishers from subscribers by
topic.

**Worker pool.** `qb::WorkerPool` holds a list of worker `ActorId`s and picks one per send —
round-robin (`next()`) or sticky-by-key (`for_key(k)`, so the same key always lands on the same
worker). Broadcast by iterating `workers()`.

```cpp
qb::WorkerPool pool{ {w0, w1, w2} };
void on(Job &j)     { push<Task>(pool.next(), j.payload); }            // round-robin
void on(Session &s) { push<Frame>(pool.for_key(s.user_id), s.frame); } // session-affine
```

**Pub/sub by topic.** `qb::PubSub<Topic>` is a per-core `ServiceActor`. Add one bus per core that needs
the topic; same-core actors reach it with `getService<qb::PubSub<Topic>>()`.

```cpp
core(0).addActor<qb::PubSub<PriceTick>>();          // wire the bus at startup

// subscriber (same core):
registerEvent<PriceTick>(*this);
getService<qb::PubSub<PriceTick>>()->subscribe(id());
void on(PriceTick &t) { /* … */ }

// publisher (same core):
getService<qb::PubSub<PriceTick>>()->publish(symbol, price);   // builds + fans out PriceTick
```

**Pitfalls.**

- `WorkerPool` does not track worker liveness — if workers can die, pair it with discovery
  (`require<T>()`) or a supervisor, and `remove()` dead ids.
- `PubSub` is **per-core**: a publication reaches subscribers on the bus's own core only. For
  cross-core topics, add a bus per core and bridge publications between them.
- A `PubSub` subscriber must also `registerEvent<Topic>(*this)` — `subscribe()` only adds it to the
  fan-out list.

## Recipe: supervision (restart children on failure)

**Task.** Keep a set of child actors running: when one fails, restart it (and, depending on the
strategy, its siblings).

Derive a supervisor from `qb::Supervisor`, override `spawn_child` to create each child as a
`qb::SupervisedActor`, and pick a `restart_strategy`. A child calls `stop()` to terminate; the
supervisor restarts per the strategy, bumping a per-slot generation so stale notifications are ignored.

```cpp
class Worker : public qb::SupervisedActor {
public:
    Worker(qb::ActorId sup, std::size_t slot, std::uint64_t gen)
        : qb::SupervisedActor(sup, slot, gen) {}
    qb::io::async::task<bool> onInit() override { registerEvent<Job>(*this); co_return true; }
    void on(Job &j) { if (!process(j)) stop(); }   // failure -> notify supervisor + kill
};

class Pool : public qb::Supervisor {
public:
    Pool() : qb::Supervisor(qb::restart_strategy::one_for_one, /*children*/ 4, /*max_restarts*/ 8) {}
protected:
    qb::ActorId spawn_child(std::size_t slot, std::uint64_t gen) override {
        return addRefActor<Worker>(id(), slot, gen).id();   // same-core referenced child (handle.id())
    }
    void on_escalate() override { kill(); }   // give up after too many restarts
};
```

Strategies: `one_for_one` (restart just the failed child), `one_for_all` (restart all), `rest_for_one`
(restart the failed child and those started after it).

**Pitfalls.**

- Supervision is **cooperative and per-core**: a child must call `stop()` (or
  `notify_supervisor_down()`) to be restarted — a child that dies silently (e.g. a failed `onInit`) is
  not auto-detected. Children are `addRefActor` referenced actors on the supervisor's core.
- `max_restarts` bounds the restart intensity; past it, `on_escalate()` runs instead of restarting
  (escalate by killing the supervisor, alerting, etc.) — otherwise a crash-looping child restarts forever.
  By default the cap is **cumulative** over the supervisor's life; pass a `restart_window`
  (`Supervisor(strategy, count, max_restarts, window)`) to count it as a sliding window ("N within T").
- Killing the supervisor (a `KillEvent`) tears down its children first — they are never orphaned.
  `Main::stop()` / `SIGINT` already broadcasts to every actor, so children stop there too.

## Recipe: broadcast fan-out

**Task.** Deliver one event to many actors at once.

`broadcast<E>(args…)` sends a freshly constructed event to every actor on every `VirtualCore`. To
target a single core, `push<E>` to a `qb::BroadcastId(coreId)`.

```cpp
// src: derived from qb/tests/core/system/messaging/messaging-api.cpp (BroadcastId)
#include <qb/actor.h>
#include <qb/main.h>
#include <qb/io.h>

struct Announce : qb::Event {
    qb::string<64> text;
    explicit Announce(const char *t) : text(t) {}
};

class Listener : public qb::Actor {
public:
    qb::io::async::task<bool> onInit() override {
        registerEvent<Announce>(*this);
        // qb::KillEvent is already registered by the default constructor; its
        // inherited handler calls kill(), so broadcast<KillEvent>() stops this actor.
        co_return true;
    }
    void on(const Announce &e) {
        qb::io::cout() << "listener " << id() << ": " << e.text << '\n';
    }
};

class Publisher : public qb::Actor {
public:
    qb::io::async::task<bool> onInit() override {
        broadcast<Announce>("system online");  // every actor on every core
        broadcast<qb::KillEvent>();            // then shut the system down
        kill();
        co_return true;
    }
};

int main() {
    qb::Main engine;
    engine.addActor<Listener>(0);
    engine.addActor<Listener>(0);
    engine.addActor<Publisher>(0);
    engine.start();
    engine.join();
    return 0;
}
```

**Pitfalls.**

- A broadcast reaches *every* actor, including ones that never registered a handler for the event;
  unhandled events are dropped safely, but design the event so any recipient can ignore it.
- `broadcast` constructs a separate event copy per recipient. For a large payload delivered to a
  curated subscriber list, prefer a broker actor that forwards a shared payload (see the
  publish/subscribe pattern on [the actor patterns page](../4_qb_core/patterns.md), and the
  `examples/core_io/message_broker/` example).

## Recipe: pipeline

**Task.** Move an item through a chain of processing stages, one actor per stage.

Each stage handles its input event, transforms it, and `push`es the next stage's event to the next
actor. Wiring the stages with `to(dest).push<…>()` chains is convenient when one handler sends several
ordered events to the same destination.

```cpp
// src: derived from qb/tests/core/system/messaging/messaging-api.cpp (to().push chaining)
#include <qb/actor.h>
#include <qb/main.h>
#include <qb/io.h>

struct RawItem    : qb::Event { int value; explicit RawItem(int v) : value(v) {} };
struct ParsedItem : qb::Event { int value; explicit ParsedItem(int v) : value(v) {} };
struct DoneItem   : qb::Event { int value; explicit DoneItem(int v) : value(v) {} };

class StageSink : public qb::Actor {
public:
    qb::io::async::task<bool> onInit() override {
        registerEvent<DoneItem>(*this);
        co_return true;
    }
    void on(const DoneItem &e) {
        qb::io::cout() << "result: " << e.value << '\n';
        kill();
    }
};

class StageTransform : public qb::Actor {
    qb::ActorId _sink;
public:
    explicit StageTransform(qb::ActorId sink) : _sink(sink) {}
    qb::io::async::task<bool> onInit() override {
        registerEvent<ParsedItem>(*this);
        co_return true;
    }
    void on(const ParsedItem &e) {
        push<DoneItem>(_sink, e.value * 10);  // hand off to the next stage
    }
};

class StageParse : public qb::Actor {
    qb::ActorId _transform;
public:
    explicit StageParse(qb::ActorId transform) : _transform(transform) {}
    qb::io::async::task<bool> onInit() override {
        registerEvent<RawItem>(*this);
        push<RawItem>(id(), 42);              // feed the pipeline
        co_return true;
    }
    void on(const RawItem &e) {
        push<ParsedItem>(_transform, e.value + 1);
    }
};

int main() {
    qb::Main engine;
    auto sink      = engine.addActor<StageSink>(0);
    auto transform = engine.addActor<StageTransform>(0, sink);
    engine.addActor<StageParse>(0, transform);
    engine.start();
    engine.join();
    return 0;
}
```

**Pitfalls.**

- Wire the stages back-to-front at construction so each upstream stage knows its downstream
  `ActorId`. `addActor` returns the new actor's id; pass it into the next constructor.
- Spreading stages across cores (`engine.addActor<Stage>(coreId, …)`) parallelizes the pipeline, but
  cross-core hops cost more than same-core ones. See [the threading
  model](../2_core_concepts/threading_model.md) and [performance tuning](./performance_tuning.md).
- `forward(next_stage, event)` is an alternative to constructing a new event per stage when the same
  event type flows through and the original source should be preserved.

## Recipe: graceful shutdown

**Task.** Stop the system cleanly — let actors release resources before the engine joins.

`kill()` does not destroy an actor immediately; it sets `_alive = false`, and the `VirtualCore`
destroys the actor later. A `qb::KillEvent` handler is the actor's chance to flush buffers, close
files, or notify peers before it goes. For a system-wide stop, `broadcast<qb::KillEvent>()` flags
every actor; `engine.join()` returns once they have all terminated.

```cpp
// src: derived from examples/core/example5_timers.cpp (broadcast<KillEvent>) and
//      qb/src/qb/core/Main.h (registerSignal)
#include <qb/actor.h>
#include <qb/main.h>
#include <qb/io.h>
#include <csignal>

class Worker : public qb::Actor {
public:
    qb::io::async::task<bool> onInit() override {
        registerEvent<qb::KillEvent>(*this);   // route KillEvent to the custom handler below
        co_return true;
    }

    void on(const qb::KillEvent &) {           // overrides the default kill()-only handler
        qb::io::cout() << "worker " << id() << ": releasing resources\n";
        // ... close files, flush state ...
        kill();                                // confirm termination
    }
};

class Supervisor : public qb::Actor {
public:
    qb::io::async::task<bool> onInit() override {
        registerEvent<qb::KillEvent>(*this);
        co_return true;
    }

    void on(const qb::KillEvent &) {
        broadcast<qb::KillEvent>();            // fan shutdown out to every actor
        kill();
    }
};

int main() {
    qb::Main engine;
    // start() installs a SIGINT handler. Registering SIGTERM makes it broadcast a
    // qb::SignalEvent too; an actor must handle on(qb::SignalEvent&) to act on it
    // (see the pitfalls below).
    qb::Main::registerSignal(SIGTERM);

    engine.addActor<Worker>(0);
    engine.addActor<Worker>(0);
    engine.addActor<Supervisor>(0);
    engine.start();
    engine.join();                             // returns once all actors have stopped
    return 0;
}
```

**Pitfalls.**

- A default-constructed actor is already subscribed to `qb::KillEvent`; its inherited handler
  (`qb::Actor::on(qb::KillEvent const &)`) calls `kill()` and nothing else. You define your own
  `on(const qb::KillEvent &)` to run cleanup first — and a custom handler **must** call `kill()`
  itself, or the actor never terminates and `join()` blocks. An actor built with
  `qb::no_default_events` registers nothing; it must `registerEvent<qb::KillEvent>(*this)` in
  `onInit()` to take part in ordered shutdown at all.
- Prefer RAII for cleanup. Holding resources in members whose destructors release them means
  correctness does not hinge on the `KillEvent` handler running. See [resource
  management](./resource_management.md).
- Signal handling covers **both terminal signals** by default. `start()` installs handlers for
  `SIGINT` *and* `SIGTERM`; when either fires, the engine broadcasts a `qb::SignalEvent`, and the
  default `qb::Actor::on(qb::SignalEvent const &)` handler calls `kill()` **when `event.signum` is
  `SIGINT` or `SIGTERM`**. Everything else stays non-terminal on purpose, so a reload signal does
  not tear the process down.
  `Main::registerSignal(int)` makes another signal (for example `SIGHUP` or `SIGUSR1`)
  broadcast the same `qb::SignalEvent` carrying that signal number — but to act on it you must
  define your own `on(qb::SignalEvent &)` and inspect `event.signum`. `Main::unregisterSignal(int)`
  restores the default OS disposition, and `Main::ignoreSignal(int)` ignores a signal.

## See also

- [Actor patterns](../4_qb_core/patterns.md) — the structural patterns (FSM, service registry,
  pub/sub, request/response-with-timeout, supervision, discovery) these recipes compose into.
- [Writing actors with `qb::Actor`](../4_qb_core/actor.md) — the full send-primitive and lifecycle API.
- [Event messaging](../4_qb_core/messaging.md) — event types, QoS, and the pipe API.
- [Asynchronous operations inside actors](../5_core_io_integration/async_in_actors.md) — `async::callback`,
  coroutines, and non-blocking I/O from a handler.
- [Error handling and resilience](./error_handling.md) — the `async::callback` lifetime rules and the
  fail-stop boundary.
