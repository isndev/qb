# Patterns cookbook

> **Audience:** Adopter · **Status:** stable · **Verified-against:** qb 2.0.0 (C++20 default, C++23 supported)

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

Two timing tools from `qb-io` recur throughout:

- `qb::io::async::callback(func, delay)` schedules `func` on the actor's own `VirtualCore` loop after
  `delay`, a `std::chrono::duration` (`qb/io/async/io.h`). A non-positive `delay` — or the no-duration
  overload `callback(func)` — runs `func` immediately and inline. The timer is one-shot and deletes
  itself after firing.
- `qb::io::async::scoped_callback(func, delay)` returns a caller-owned `std::unique_ptr<ScopedTimeout<…>>`
  whose destruction cancels the pending callback. Prefer it when you need to cancel a timer or reuse
  the handle (`qb/io/async/io.h`).

Because a scheduled callback runs outside any `on()` handler, it must capture only what it needs and
guard re-entry into the actor with `is_alive()` before touching actor state. See [the `async::callback`
lifetime rules](./error_handling.md) for the full contract.

## Recipe: one-shot timer

**Task.** Run an action once, after a delay, from inside an actor.

Schedule a `callback` that posts a self-event when the timer fires. Routing the deferred work back
through an event (rather than doing it directly in the lambda) keeps it on the actor's single-threaded
handler path, where member access is safe.

```cpp
// src: derived from qb/source/core/tests/system/test-actor-delayed-events.cpp
#include <qb/actor.h>
#include <qb/main.h>
#include <qb/io.h>
#include <qb/io/async.h>
#include <chrono>

using namespace std::chrono_literals;

struct Tick : qb::Event {};

class OneShotActor : public qb::Actor {
public:
    bool onInit() override {
        registerEvent<Tick>(*this);
        // Fire Tick on ourselves 200 ms from now.
        qb::io::async::callback([this]() {
            if (is_alive())                 // the actor may already be gone
                push<Tick>(id());
        }, 200ms);
        return true;
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

- The lambda runs outside any handler. Guard every actor access with `is_alive()`; a `push` to a dead
  actor's `id()` is harmless, but reading or mutating members after destruction is undefined behavior.
- `callback` schedules on *the calling thread's* loop. Call it from an actor handler (or `onInit`),
  not from `main` or another thread, so the timer lands on the actor's own `VirtualCore`.

### Cancellable variant

When you must be able to cancel the timer (a deadline that the response may beat), use
`scoped_callback` and hold the returned handle. Destroying or reassigning it stops the pending
callback.

```cpp
// src: derived from qb/include/qb/io/async/io.h (ScopedTimeout / scoped_callback)
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
    bool onInit() override {
        registerEvent<Deadline>(*this);
        registerEvent<Response>(*this);
        _deadline = qb::io::async::scoped_callback(std::function<void()>([this]() {
            if (is_alive())
                push<Deadline>(id());
        }), 500ms);
        return true;
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

Inherit from `qb::ICallback` and register it. `onCallback()` is invoked once per `VirtualCore` loop
iteration — after the mailbox is drained and before outgoing pipes are flushed. It runs on the
event-loop thread, so it must be fast and non-blocking.

```cpp
// src: derived from qb/source/core/tests/system/test-actor-callback.cpp
#include <qb/actor.h>
#include <qb/icallback.h>
#include <qb/main.h>
#include <qb/io.h>

class HeartbeatActor : public qb::Actor, public qb::ICallback {
    uint64_t _ticks = 0;
public:
    bool onInit() override {
        registerCallback(*this);            // start the per-iteration tick
        return true;
    }

    void onCallback() override {
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

**Periodic at a fixed interval.** `onCallback()` fires as fast as the loop turns, which is not a fixed
period. For a steady wall-clock interval, chain self-scheduled `callback`s instead — each handler
re-arms the next tick:

```cpp
// src: derived from qb/source/core/tests/system/test-actor-delayed-events.cpp
#include <qb/actor.h>
#include <qb/io/async.h>
#include <chrono>

using namespace std::chrono_literals;

struct PollNow : qb::Event {};

class PollingActor : public qb::Actor {
public:
    bool onInit() override {
        registerEvent<PollNow>(*this);
        push<PollNow>(id());                // kick off the first cycle
        return true;
    }

    void on(const PollNow &) {
        // ... do one unit of polling work ...
        qb::io::async::callback([this]() {  // re-arm the next tick at a fixed delay
            if (is_alive())
                push<PollNow>(id());
        }, 1s);
    }
};
```

**Pitfalls.**

- `onCallback()` blocks the whole core while it runs. Never sleep, wait on a mutex, or do synchronous
  I/O inside it.
- Callback frequency depends on loop rate and the configured idle latency
  (`CoreInitializer::setLatency`), not a clock. Use the self-scheduled-`callback` variant when the
  interval must be predictable.

## Recipe: request/reply

**Task.** Send a request to another actor and handle its response, with the response routed back to
the original sender automatically.

Reuse the request event for the reply. The responder takes the event by **non-const reference**,
fills in the result, and calls `reply(event)`, which swaps the event's source and destination so it
returns to the requester. No bookkeeping of `ActorId`s is needed.

```cpp
// src: derived from qb/source/core/tests/system/test-actor-event.cpp (reply/forward)
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
    bool onInit() override {
        registerEvent<Query>(*this);
        return true;
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

    bool onInit() override {
        registerEvent<Query>(*this);
        push<Query>(_responder, 7); // request
        return true;
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
// src: derived from qb/source/core/tests/benchmark/bm-forward-reply.cpp
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
// src: derived from qb/source/core/tests/system/test-actor-coroutine-scope.cpp
#include <qb/actor.h>
#include <qb/main.h>
#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>
using namespace std::chrono_literals;

struct Result : qb::Event { int value = 0; };

class Worker : public qb::Actor {
public:
    bool onInit() override {
        registerEvent<Result>(*this);
        spawn([](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            co_await ctx.sleep(2s);          // cancelled instantly if the actor is killed
            ctx.push<Result>();              // talk back only through ctx, never `this`
        });
        return true;
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
// src: derived from qb/source/core/tests/system/test-actor-coroutine-ask.cpp
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
    bool onInit() override { registerEvent<PriceQuery>(*this); return true; }
    void on(PriceQuery &q) { q.price = q.query * 2; reply(q); }   // reply preserves correlation_id
};

class Trader : public qb::Actor {        // asker
    qb::ActorId _market;
public:
    explicit Trader(qb::ActorId m) : _market(m) {}
    bool onInit() override {
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
        return true;
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
struct Quote : qb::Request<double> { std::string symbol; };          // request: symbol — response: double

// responder:
void on(Quote &q) { qb::answer(*this, q, [](Quote const &r){ return lookup(r.symbol); }); }

// asker (inside a spawn() coroutine):
auto q = co_await qb::ask(ctx, market, Quote{"BTC"}, 500ms);
use(q.response);
```

**Scatter-gather.** `ask_all` sends a copy to every target and waits for all replies; `ask_any`
resolves with the first (fastest wins). Both throw `timeout_error` if the deadline passes.

```cpp
spawn([markets](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
    auto quotes = co_await qb::ask_all(ctx, markets, Quote{"BTC"}, 500ms);   // std::vector<Quote>
    for (auto const &q : quotes) use(q.response);
    auto fastest = co_await qb::ask_any(ctx, markets, Quote{"BTC"}, 500ms);  // first reply
});
```

**Saga.** `run_saga` runs a sequence of steps, each registering a compensation; if a later step
fails, the registered compensations run in **reverse** order before the error propagates. (A kill —
`cancelled_error` — aborts hard, without rollback.)

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

- A cross-core `ask_all` keeps N requests in flight at once (bounded by `timeout`); `ask_any`'s losers
  are not cancelled — they linger until their own `timeout`.
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
qb::retry_policy policy{ .max_attempts = 5, .backoff = 50ms, .multiplier = 2.0, .max_backoff = 1s };
auto r = co_await qb::ask_retry(ctx, market, Quote{"BTC"}, 200ms, policy);
```

**Circuit breaker.** A `qb::CircuitBreaker` trips **open** after N consecutive failures, fails fast for
a cooldown, then admits a **half-open** trial. Hold it by `shared_ptr` so the coroutine captures it by
value (it outlives the actor); `ask_guarded` checks it, sends the ask, and records the outcome.

```cpp
// actor member: std::shared_ptr<qb::CircuitBreaker> breaker_ = std::make_shared<qb::CircuitBreaker>(5, 2s);
spawn([breaker = breaker_, market](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
    try {
        auto r = co_await qb::ask_guarded(ctx, breaker, market, Quote{"BTC"}, 200ms);
        use(r.response);
    } catch (const qb::circuit_open_error &) {
        // breaker is open — fall back without touching the failing market
    } catch (const qb::io::async::timeout_error &) {
        // counted as a breaker failure
    }
});
```

**Pitfalls.**

- A success **closes** the breaker, a timeout (or other non-cancellation error) is a **failure**; a kill
  (`cancelled_error`) is *not* counted — it is a controlled shutdown, not a responder fault.
- Capture the breaker `shared_ptr` **by value** — never a reference to an actor member.
- Compose the two by retrying *around* a guarded ask (each guarded attempt also feeds the breaker).

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
    bool onInit() override { registerEvent<Job>(*this); return true; }
    void on(Job &j) { if (!process(j)) stop(); }   // failure -> notify supervisor + kill
};

class Pool : public qb::Supervisor {
public:
    Pool() : qb::Supervisor(qb::restart_strategy::one_for_one, /*children*/ 4, /*max_restarts*/ 8) {}
protected:
    qb::ActorId spawn_child(std::size_t slot, std::uint64_t gen) override {
        return addRefActor<Worker>(id(), slot, gen)->id();   // same-core referenced child
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

## Recipe: broadcast fan-out

**Task.** Deliver one event to many actors at once.

`broadcast<E>(args…)` sends a freshly constructed event to every actor on every `VirtualCore`. To
target a single core, `push<E>` to a `qb::BroadcastId(coreId)`.

```cpp
// src: derived from qb/source/core/tests/system/test-actor-event.cpp (BroadcastId)
#include <qb/actor.h>
#include <qb/main.h>
#include <qb/io.h>

struct Announce : qb::Event {
    qb::string<64> text;
    explicit Announce(const char *t) : text(t) {}
};

class Listener : public qb::Actor {
public:
    bool onInit() override {
        registerEvent<Announce>(*this);
        // qb::KillEvent is already registered by the default constructor; its
        // inherited handler calls kill(), so broadcast<KillEvent>() stops this actor.
        return true;
    }
    void on(const Announce &e) {
        qb::io::cout() << "listener " << id() << ": " << e.text << '\n';
    }
};

class Publisher : public qb::Actor {
public:
    bool onInit() override {
        broadcast<Announce>("system online");  // every actor on every core
        broadcast<qb::KillEvent>();            // then shut the system down
        kill();
        return true;
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
// src: derived from qb/source/core/tests/system/test-actor-event.cpp (to().push chaining)
#include <qb/actor.h>
#include <qb/main.h>
#include <qb/io.h>

struct RawItem    : qb::Event { int value; explicit RawItem(int v) : value(v) {} };
struct ParsedItem : qb::Event { int value; explicit ParsedItem(int v) : value(v) {} };
struct DoneItem   : qb::Event { int value; explicit DoneItem(int v) : value(v) {} };

class StageSink : public qb::Actor {
public:
    bool onInit() override {
        registerEvent<DoneItem>(*this);
        return true;
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
    bool onInit() override {
        registerEvent<ParsedItem>(*this);
        return true;
    }
    void on(const ParsedItem &e) {
        push<DoneItem>(_sink, e.value * 10);  // hand off to the next stage
    }
};

class StageParse : public qb::Actor {
    qb::ActorId _transform;
public:
    explicit StageParse(qb::ActorId transform) : _transform(transform) {}
    bool onInit() override {
        registerEvent<RawItem>(*this);
        push<RawItem>(id(), 42);              // feed the pipeline
        return true;
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
//      qb/include/qb/core/Main.h (registerSignal)
#include <qb/actor.h>
#include <qb/main.h>
#include <qb/io.h>
#include <csignal>

class Worker : public qb::Actor {
public:
    bool onInit() override {
        registerEvent<qb::KillEvent>(*this);   // route KillEvent to the custom handler below
        return true;
    }

    void on(const qb::KillEvent &) {           // overrides the default kill()-only handler
        qb::io::cout() << "worker " << id() << ": releasing resources\n";
        // ... close files, flush state ...
        kill();                                // confirm termination
    }
};

class Supervisor : public qb::Actor {
public:
    bool onInit() override {
        registerEvent<qb::KillEvent>(*this);
        return true;
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
- Signal handling is `SIGINT`-centric by default. `start()` installs a handler for `SIGINT` only;
  when it fires, the engine broadcasts a `qb::SignalEvent`, and the default
  `qb::Actor::on(qb::SignalEvent const &)` handler calls `kill()` **only when `event.signum ==
  SIGINT`**. `Main::registerSignal(int)` makes another signal (for example `SIGTERM` or `SIGHUP`)
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
