# Patterns cookbook

> **Audience:** Adopter · **Status:** stable · **Verified-against:** qb 2.0.0 (c++23)

Task-oriented recipes for the interactions you reach for most often: one-shot and periodic timers, request/reply, broadcast fan-out, multi-stage pipelines, and graceful shutdown — each a complete, compilable snippet.

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
