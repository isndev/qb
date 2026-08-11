<!-- Verified-against: qb 3.0.0 (C++20 default, C++23 supported). Source of truth: the headers and sources under qb/src. -->
# qb — concepts for writing correct code

This file teaches an LLM the mental model and rules needed to emit **compilable, correct** qb code.
It is not a tutorial. Prefer the verified signatures and invariants below over anything remembered.
qb is a **C++20-first** actor framework with optional C++23 support: share-nothing actors + non-blocking async I/O (libev) + native
C++20 coroutines. Two libraries: **`qb-io`** (runtime: event loop, sockets, protocols, coroutines,
time/crypto/compression) and **`qb-core`** (actor engine on top of `qb-io`). `qb-io` is usable
standalone.

<!-- llms-txt:lead -->
> qb — the qb Actor Framework, **QBAF** — is a C++20-first (optional C++23) actor framework
> for concurrent and distributed C++: share-nothing actors over a non-blocking asynchronous
> I/O runtime with native C++20 coroutines. Two libraries compose — **qb-io** (event loop, TCP/UDP/TLS/QUIC transports,
> protocols, coroutines, time, crypto, compression) and **qb-core** (the actor engine on top
> of it); qb-io is usable standalone. Optional **qbm** modules add HTTP/1.1·2·3 + WebSocket,
> PostgreSQL and Redis. Apache-2.0, CMake ≥ 3.24, Linux · macOS · Windows, x86-64 · ARM64.

Five rules decide whether generated qb code is correct; everything else is detail.

1. **Time is `std::chrono`.** Every timeout / TTL / interval / delay / latency is a
   `qb::duration` (= `std::chrono::nanoseconds`); it accepts finer-or-equal chrono literals
   and rejects bare integers at compile time. `qb::mono_time` is the steady clock,
   `qb::wall_time` the system clock, and subtracting one from the other does not compile.
   Never emit `qb::Timestamp`, `qb::Duration`, `qb::TimePoint`, `to_timestamp(`,
   `to_time_point(` or the header `<qb/system/timestamp.h>`: 3.0 removed all six.
2. **Actors share nothing.** An actor is thread-affine to one `VirtualCore`, processes one
   event at a time, and talks only by events — no mutexes, no shared mutable state, and no
   actor ever constructed outside a worker thread (use `Main::core(i).addActor<T>(...)`,
   `addActor<T>(core, ...)` or `addRefActor<T>(...)`).
3. **`push` / `send` / `broadcast` are `noexcept`.** A throwing event constructor, or an OOM
   growing the pipe, calls `std::terminate()`. Events are relocated with raw `memcpy`, so no
   member may point into its own storage: a by-value `std::string` is never a valid event
   member — use `qb::string<N>` or box it behind a smart pointer. `send<T>()` additionally
   requires a trivially destructible event and is unordered; `push<T>()` is ordered.
4. **A coroutine captures by value before its first `co_await`.** The actor can be destroyed
   while the coroutine is suspended, so after any suspension the only legal channel back is
   the context (`ctx.push_to<E>(...)`, `ctx.id()`, `ctx.time()`). Prefer `Actor::spawn(...)`
   — bound to the actor's cancellation scope — over `Actor::spawn_detached(...)`, and pass
   the lambda **without** a trailing `()`, which would destroy its closure before the
   coroutine starts.
5. **Register every event you handle.** `registerEvent<T>(*this)` inside `onInit()`, which is
   itself a coroutine returning `qb::io::async::task<bool>`; `co_return false` or a throw
   fails creation and yields an invalid `ActorId`.
<!-- /llms-txt:lead -->

## Mental model

**Actor** — an isolated object (derives `qb::Actor`) with a unique `qb::ActorId`. It owns private
state, communicates **only** by sending events, and processes its mailbox **one event at a time** on a
single worker thread. No mutexes: thread-safety comes from isolation, not locks.

**VirtualCore** — a worker thread (`std::jthread`) that owns a set of actors and runs one event loop
dispatching their events, callbacks, async I/O, and inter-core message flushing. An actor is strictly
**thread-affine**: it never migrates cores. Per loop iteration: drain inter-core mailbox → drain local
event queue → run `ICallback::on(qb::LoopEvent const&)` ticks → flush outgoing pipes → idle.

**Main (`qb::Main`, alias `qb::engine`)** — the top-level controller. Configure cores/actors **before**
`start()`, then `start()`/`join()`. Spawns one VirtualCore thread per used core. Inter-core messaging
uses lock-free MPSC ring buffers; same-source→same-dest delivery is FIFO-ordered.

**Event (`qb::Event`)** — the only inter-actor message. Subclass it, add data members, register a
handler in `onInit()`, implement `on(const EventType&)`. Sending is non-blocking.

**Async I/O (`qb-io`)** — non-blocking event loop (`qb::io::async::listener`), thread-local via
`listener::current`. Network/timer/file readiness dispatches to `on(...)` handlers. In `qb-core` the
VirtualCore drives the loop automatically; standalone you call `qb::io::async::init()` then `run(...)`.

**Coroutines** — native C++20 `co_await`, strictly **single-thread cooperative** per scheduler. Inside
an actor you spawn them via `Actor::spawn(...)`, which binds them to the actor's cancellation scope so
a killed actor cannot leave one parked; `Actor::spawn_detached(...)` is the low-level form that
deliberately outlives the actor. Either way the coroutine MUST NOT touch actor members after a
`co_await` (the actor may have been destroyed) — capture by value and talk back only via the context
(`qb::ScopedCoroContext` for `spawn`, `qb::CoroContext` for `spawn_detached`).

## Core APIs (verified signatures)

### Actor

```cpp
#include <qb/actor.h>

class MyActor : public qb::Actor {
public:
    qb::io::async::task<bool> onInit() override {              // runs once, after ID assignment, before any event
        registerEvent<MyEvent>(*this);    // REQUIRED: subscribe to every event you handle
        co_return true;                      // false aborts creation -> id().is_valid()==false
    }
    void on(const MyEvent &e) { /* read-only handler */ }
    void on(MyEvent &e) { reply(e); }     // non-const handler: needed for reply()/forward()
};
```

Key members (all called from inside the actor): `id()`, `getIndex()`, `is_alive()`, `kill()`,
`time()`, `registerEvent<T>(*this)` / `unregisterEvent<T>(*this)`,
`push<T>(dest, args...)`, `send<T>(dest, args...)`, `broadcast<T>(args...)`, `reply(e)`,
`forward(dest, e)`, `to(dest).push<T>(...)` (EventBuilder), `getPipe(dest)`,
`addRefActor<T>(args...)`, `addRefHandle<T>(args...)`, `getService<T>()`, `require<T>()`,
`is<T>(event)`, `spawn(...)` / `spawn_detached(...)`, `context()`, `resolve_ask(e)`, `resolve_require(e)`,
`has_active_coroutines()`.

Lifecycle: by default every actor auto-subscribes to `KillEvent`, `SignalEvent`,
`UnregisterCallbackEvent`, `PingEvent`, `RequireEvent` (the last routes `co_await qb::ping`/`require`
replies). For graceful cleanup override `on(const qb::KillEvent&)` and
**call `kill()` at the end**. The virtual destructor runs later under VirtualCore control (RAII members
clean up there). To opt out of the five default subscriptions construct with the `qb::no_default_events`
tag — then you must register at least `KillEvent` yourself in `onInit()`.

### Events

```cpp
#include <qb/event.h>

struct MyEvent : qb::Event {
    int code{};
    qb::string<64> name;                  // fixed-capacity string: ABI-safe in events
    std::shared_ptr<Payload> big;         // large/non-trivial data -> pass via smart pointer
    MyEvent(int c, const char *n) : code(c), name(n) {}
};
```

Rules: PODs and `qb::string<N>` are fine as direct members. Large/owning data goes through
`std::shared_ptr`/`std::unique_ptr`. **A by-value `std::string` is never a valid event member, on
any path** — the runtime relocates events with raw `memcpy` (the source pipe moves what it already
holds when it grows, `reply`/`forward` byte-recycle it, a cross-core hop copies it twice) and a
*short* `std::string` points into its own inline buffer on libstdc++, so it dangles after the move;
libc++ recomputes `data()` from `this`, which is why this corrupts on Linux and passes every macOS
test. Use `qb::string<N>` or box it. Events sent via **`send<T>()` must additionally be trivially
destructible**; `push<T>()` carries no destructibility requirement.
`push` = ordered, returns the event by mutable reference for
in-place population — that reference dies at the **next** event queued to the same destination core
(the pipe reallocates or compacts), so populate it fully before sending anything else;
`send` = unordered fire-and-forget. `qb::EventQOS0` is the lowest-priority,
unordered, droppable-under-backpressure variant.

### Engine

```cpp
#include <qb/main.h>
int main() {
    qb::Main engine;
    engine.core(0).setLatency(qb::duration::zero());  // 0 = busy-spin (lowest latency, 100% CPU)
    auto id = engine.addActor<MyActor>(0, ctor_args...);   // returns ActorId; pick core
    engine.core(1).setAffinity({2, 3});        // pin VirtualCore 1 to CPUs 2,3 (best-effort)
    engine.start();                            // async=true (default): returns when cores ready
    engine.join();                             // block until all actors stop
    if (engine.hasError()) return 1;           // surfaces VirtualCore failures post-join
}
```

`start(false)` makes the calling thread the last worker and blocks until shutdown.
`Main::core(idx)` throws once the engine is running, and `std::range_error` for `idx >= qb::MaxCores`
(256). A core started with **0 actors fails startup** (`Error::NoActor`). `setLatency` takes a
`qb::duration`: `qb::duration::zero()` = busy-spin; `>0` parks on a condition variable up to that span
when idle.

`setAffinity` is best-effort, and on **Apple Silicon it does nothing at all** — silently. macOS has no
`pthread_setaffinity_np`, so qb emulates one with `thread_policy_set(THREAD_AFFINITY_POLICY)`, a flavor
arm64 macOS does not implement: it answers `KERN_NOT_SUPPORTED` for every core (measured on an Apple M4
Pro, Darwin 25.6.0: `ret == 46`). qb's shim reports that as success on purpose — failing would warn once
per core on every run — so no caller and no log learns the pin never happened. Even on Intel macOS the
affinity tag is a scheduler *hint* that groups threads onto a shared L2, not a pin to CPU N. Hard per-CPU
pinning is real only on Linux (`pthread_setaffinity_np`) and Windows/MSVC (`SetThreadAffinityMask`).
**Never assume a pin took effect: ask `qb::CPU::ThreadPinningSupported()`** (`qb/system/cpu.h`), the one
way to tell "pinned" from "silently not pinned". A refused pin never fails core init either way.

### Async I/O — standalone `qb-io`

```cpp
#include <qb/io/async.h>
qb::io::async::init();                       // per thread; self-initializing thread_local
qb::io::async::run();                         // run forever (blocking)
qb::io::async::run_once();                    // one iteration
qb::io::async::run_until(some_bool_ref);      // until flag flips
qb::io::async::break_parent();                // stop this thread's run() loop
```

### Delayed work & timeouts

```cpp
#include <qb/io/async.h>
using namespace qb;                           // brings chrono literals (5s, 100ms, ...)

// Run a lambda on the current loop after a delay (chrono/qb::duration only):
qb::io::async::callback([this] {
    if (is_alive()) push<TickEvent>(id());    // ALWAYS guard actor capture with is_alive()
}, 100ms);
// delay <= 0 (or the no-duration overload) executes the lambda inline, NOT next iteration.

// Three distinct primitives — pick by intent:
//   callback(fn, delay>0)  -> run after a REAL timed delay (timeout / deadline).
//   defer(fn)              -> run at the TAIL of the current loop turn (next tick, no delay,
//                             no timer). THE way to continue after the current handler unwinds,
//                             e.g. a handler that must destroy/replace the object it runs on.
//   callback(fn) / fn()    -> run INLINE, right now (callback(fn) is NOT deferred).
qb::io::async::defer([this] { if (is_alive()) reconnect(); }); // never runs re-entrantly

// Inactivity timeout (CRTP): implement on(event::timer&), call updateTimeout() on activity.
class Session : public qb::io::async::with_timeout<Session> {
    void on(qb::io::async::event::timer const &) { /* idle expired */ }
};
```

### Network actors via `qb::io::use<>`

Inherit a CRTP helper to get a transport, in/out buffers, and protocol wiring. Declare
`using Protocol = ...;` and implement `on(Protocol::message&&)` plus I/O events
(`on(event::disconnected&)`, etc.). Verified helper shapes (`_Derived` is your type):

- TCP: `qb::io::use<T>::tcp::client<Server=void>`, `::tcp::server<Session>`, `::tcp::acceptor`,
  `::tcp::io_handler<Session>`.
- TCP+TLS: `qb::io::use<T>::tcp::ssl::{client,server,acceptor,io_handler}` (needs `QB_HAS_SSL`).
- UDP: `qb::io::use<T>::udp::server`, `::udp::client` (datagram-oriented, no per-peer session demux).
- QUIC/HTTP3: `qb::io::use<T>::quic::{client,server,io_handler}` (needs `QB_HAS_QUIC`).

### Coroutines inside an actor

**Use `spawn()`. It is the default and the safe one.** `spawn()` binds the coroutine to a per-actor
cancellation scope: when the actor is killed or destroyed the scope is cancelled, any coroutine parked
on a *cancellation-aware* await (`ctx.sleep`, `ctx.cancellation_point`, `ctx.until_cancelled`,
`ctx.cancellable`, and everything in the patterns library — `qb::ask` and friends) wakes on the next
loop iteration, throws `qb::io::async::cancelled_error`, and unwinds cleanly (destructors and `catch`
blocks run). A killed actor therefore cannot leave a coroutine parked on a long timeout or a dead
socket. `spawn()` hands the lambda a `qb::ScopedCoroContext`. _(Actor.h:1204-1239)_

```cpp
#include <qb/actor.h>
#include <qb/io/async.h>

class Fetcher : public qb::Actor {
public:
    qb::io::async::task<bool> onInit() override {
        registerEvent<FetchEvent>(*this);
        co_await context().sleep(1ms);        // cancel-on-kill, usable inside onInit()
        co_return true;
    }

    void on(const FetchEvent &e) {
        auto key       = e.key;               // copy by VALUE, BEFORE the first co_await
        auto requester = e.getSource();       // ditto - `e` is dead after the handler returns
        spawn([key, requester](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            co_await ctx.sleep(50ms);         // cancelled if the actor is killed meanwhile
            // `this` and every actor member are off-limits from here on.
            ctx.push_to<ResultEvent>(requester, key);   // talk back only through ctx
            co_return;
        });
    }
};
```

**The lifetime rule, precisely.** The cancellation scope bounds *how long the coroutine can stay
parked*; it does **not** make member access after a suspension legal. Between two `co_await`s the
actor can be destroyed, and the coroutine frame outlives it. So:

- **Capture everything by value before the first `co_await`** — never `this`, never a reference or
  pointer to an actor member, never a reference to the event (the event's storage is recycled as
  soon as the handler returns).
- **After any `co_await`, the only legal channel back is the context.** `ctx.push<E>()` /
  `ctx.push_to<E>(dest, …)` / `ctx.broadcast<E>()` / `ctx.id()` / `ctx.time()` are safe by
  construction — the context stores the `ActorId` by value, and events addressed to a dead actor are
  dropped, not delivered into freed memory. _(Actor.h:1380-1386)_
- **`spawn()` must be called from the actor's own VirtualCore thread** (i.e. from a handler,
  `onInit()`, or `on(qb::LoopEvent const&)`).
- **Pass the lambda WITHOUT a trailing `()`.** `spawn(f())` invokes the closure to get a `task`, the
  temporary closure dies at the end of the full expression, and the coroutine — whose
  `initial_suspend` is `suspend_always`, so it has not started yet — resumes on freed captures. That
  is ASan-invisible stack corruption. `qb/scripts/check-spawn-dangling-closure.py` lints for it.

`qb::ScopedCoroContext` is a superset of `qb::CoroContext`. On top of `push`/`push_to`/`broadcast`/
`id`/`time` it adds the cancellation-aware surface: `sleep(qb::duration)`, `cancellation_point()`,
`until_cancelled()`, `cancellable(task<T>&&)`, `child_token()`, `token()`, `cancelled()`.
_(Actor.h:1674-1753)_

`Actor::context()` returns that same `ScopedCoroContext` **wherever you hold the actor** — most
importantly inside `onInit()`, which is itself a coroutine (`task<bool>`) and gets no `ctx`
parameter. It is also what you pass to the free functions of the patterns library:
`co_await qb::ask(context(), target, req, 500ms)`. _(Actor.h:1241-1257, :1762-1765)_

**When `spawn_detached()` is the right tool — and only then.** It is the low-level form: the lambda
receives a plain `qb::CoroContext` (no scope token), and the coroutine is **not** cancelled when the
actor dies — it runs to completion, orphaned. Reach for it only for fire-and-forget work that must
*intentionally outlive* the actor, e.g. flushing an audit record during shutdown. It cannot be used
with `qb::ask` or any patterns-library helper, all of which require a `ScopedCoroContext`; passing a
`CoroContext` there is a compile error, not a runtime risk. _(Actor.h:1144-1202)_

```cpp
void on(const AuditEvent &) {
    auto sink = _sink;                        // by value, before the first co_await
    spawn_detached([sink](qb::CoroContext ctx) -> qb::io::async::task<void> {
        co_await qb::io::async::sleep(10ms);  // NOT cancelled if the actor dies here
        ctx.push_to<ResultEvent>(sink, qb::string<64>{"flushed"});
        co_return;
    });
    kill();                                   // the detached flush still completes
}
```

Introspection: `has_active_coroutines()`, `active_coroutine_count()`, `has_coro_scope()` (true once
`spawn()` **or** `context()` has lazily allocated the scope — `spawn_detached()` never does).
`task<T>` is move-only.

## Key patterns

- **Send to self / schedule** — `push<TickEvent>(id())`, or `qb::io::async::callback(fn, delay)` to
  defer; the lambda re-checks `is_alive()` and pushes an event back.
- **Service (core-local singleton)** — `class Cfg : public qb::ServiceActor<CfgTag> {...};` add via
  `addActor`; reach same-core via `getService<Cfg>()`.
- **Referenced child (same core)** — `addRefActor<Child>(args...)` returns a phase-aware
  `qb::ActorHandle<T>` (alias `RefActorHandle<T>`); `get()`/`operator->` resolve the live actor on demand
  and yield `nullptr` while the child is Activating, after a failed init, or once it died — never a
  dangling pointer. Send to `handle.id()` any time; gate direct calls on `handle.ready()`.
  `addRefHandle<Child>()` is a **pure alias** of `addRefActor` returning the same handle type — not a
  stronger or separately liveness-checked one — and `RefActorHandle<T>` is a `using` alias of
  `ActorHandle<T>`, not a distinct class. Both are retained only for source compatibility.
- **Request/response with timeout** — prefer the native `co_await qb::ask(ctx, target, req, timeout)`
  (`qb::AskEvent`/`qb::Request<Resp>`, responder `qb::answer`, asker `resolve_ask`) over hand-rolled
  pending-state. `qb::ask` is a **free function** and its first parameter is a `qb::ScopedCoroContext` —
  the `ctx` of a `spawn()` body, or `context()` anywhere else. A `CoroContext` from `spawn_detached`
  does not convert and will not compile. _(patterns/request.h:100)_ Manual fallback: store pending
  state keyed by request id; schedule a self-sent timeout via `qb::io::async::callback`; clear on
  response or timeout.
- **Patterns library** (`<qb/core/patterns.h>`, header-only over the kernel — narrative home
  `qb/readme/4_qb_core/patterns_library.md`; signatures in qb.llm.api.md "Patterns"; recipes in the
  cookbook) — request/response (`ask`, `answer`, `ask_by`/`deadline`), discovery (`ping`,
  `require<T>`), idempotency (`answer_idempotent`, `dedup_map`), aggregation (`batcher`), streaming
  (`ask_stream` + `StreamRequest`/`yield_answer`/`end_stream`; chunks via `resolve_ask`),
  scatter-gather (`ask_all` incl. bounded sliding-window, `ask_any`, `ask_quorum`), resilience
  (`ask_retry`+`retry_policy.jitter`, `ask_guarded`+`CircuitBreaker`, `rate_limiter`, `bulkhead`),
  saga (`run_saga`), routing (`WorkerPool`), pub/sub (`PubSub`), supervision (`Supervisor`). All
  core-local; coroutine-side helpers are cancel-on-kill.
- **Discovery** — `require<Target>()`; handle `qb::RequireEvent` and gate on `is<Target>(event)`.
  The default `on(qb::RequireEvent&)` already routes replies to a pending `co_await qb::ping`/
  `qb::require` through `resolve_require(e)`, so the coroutine form needs no boilerplate — but if
  you **override** that handler for the fire-and-forget form, call `resolve_require(e)` first or
  every coroutine discovery on that actor hangs until its timeout. (Exactly `resolve_ask`'s rule.)
  There is **no `status` field**: presence *is* the status — a dead actor never replies. Prefer
  `co_await qb::require<Target>(context(), timeout)`, which correlates the replies for you.
  _(Event.h:619-632)_
- **reply vs forward** — both reuse the received event (require a non-const `on(Event&)` handler).
  `reply(e)` swaps dest↔source; `forward(dest, e)` keeps the original source. Neither works on
  broadcast events (dropped). After the call the event is consumed — do not touch it.
- **Blocking work** — wrap synchronous file/DB calls in `qb::io::async::callback` (blocks one callback
  turn, not the actor handler) or hand off to a dedicated worker actor.

## Invariants & gotchas (cite-backed; violating these = UB, terminate, or miscompile)

- **Actors are constructed only on a VirtualCore worker thread.** Never `new MyActor` from `main()` or
  an arbitrary thread (asserts). Use `Main::core(i).addActor<T>(...)` / `addActor<T>(core, ...)` /
  `addRefActor<T>()`. _(Actor.cpp:114-119)_
- **`onInit()` is an async coroutine (`qb::io::async::task<bool>`) that may `co_await`; it must
  `registerEvent<T>(*this)` for every handled event.** `co_return true` activates the actor; `co_return false`
  or throwing fails init and the resulting `ActorId` is invalid. While `onInit()` is suspended the actor
  is *Activating*. _(Actor.h:321-334)_
- **`push`/`send`/`broadcast` and the messaging hot path are `noexcept`.** A throw across that boundary
  (e.g. OOM growing the pipe, or a throwing event constructor) calls `std::terminate()`. Keep events
  small and allocation-light. _(Actor.h:871-877; Pipe.h:135-150)_
- **`send<T>()` requires a trivially-destructible event** and is unordered; `push<T>()` is ordered and
  carries no destructibility requirement. Use `push` unless you have a specific reason. _(Actor.h:829-834, :886-890)_
- **Every event payload must be trivially *relocatable* — on `push` as much as on `send`.** The
  runtime moves events with raw `memcpy` and abandons the source without running a destructor there,
  so no member may point into its own storage. A by-value `std::string` is exactly that shape on
  libstdc++ (short-string buffer addressed by an internal pointer) and is therefore invalid in an
  event even for a same-core `push`; `qb::string<N>`, `std::vector` and smart pointers are fine.
  _(VirtualCore.cpp `__flush_all__`; pipe.h `recycle`)_
- **`reply`/`forward` consume the event and need a non-const `on(Event&)`;** broadcast events can't be
  replied/forwarded. _(Actor.cpp:300-318)_
- **`addRefActor<T>()` returns a phase-aware `qb::ActorHandle<T>` (alias `RefActorHandle<T>`);**
  `get()`/`operator->` resolve the live actor on demand and yield `nullptr` while the child is Activating,
  after a failed init, or once it died — never a dangling pointer. Send to `handle.id()` any time; gate
  direct calls on `handle.ready()`. Cross-thread deref of a `RefActorHandle` is a logic error.
  _(Actor.h:1098-1102, :1122, :1830-1832)_
- **Coroutine after `co_await`: never read actor members** — capture by value before the first
  `co_await`, communicate back only through the context. Prefer **`spawn()`** (`ScopedCoroContext`,
  cancelled when the actor dies) over `spawn_detached()` (`CoroContext`, deliberately outlives it);
  both must be called from the actor's own worker thread. _(`spawn_detached` Actor.h:1202 / VirtualCore.h:1146; `spawn` Actor.h:1239 / VirtualCore.h:1159)_
- **`on(qb::LoopEvent const&)` (ICallback) runs every loop iteration and must be fast/non-blocking;** blocking it
  stalls the whole core and every actor on it. _(ICallback.h:16-19)_
- **Configure cores/actors before `start()`.** `Main::core()` throws once the engine is running. A core
  with 0 actors fails startup. _(Main.cpp:484-486, :341-343)_
- **`Actor::time()` is the VirtualCore's cached nanosecond timestamp,** constant within one handler /
  `on(qb::LoopEvent const&)` invocation. For a continuously-updating value use `qb::wall_now()` /
  `qb::unix_nanos(qb::wall_now())`. _(Actor.h:567-583; VirtualCore.h:648-659)_
- **One listener per thread; never share I/O objects across threads.** Construct and destroy an async
  object on the same thread whose `listener::current` it bound to. _(async/listener.h:66-78; async/io.h:62-67, :82-83, :91-95)_
- **Don't call `async::run`/`run_once`/`run_until`/`run_sync`/`run_for` from inside a coroutine or actor
  handler** already under the scheduler — throws `std::logic_error` (asserts in debug). Inside an actor,
  drive coroutines via `spawn()` (or `spawn_detached()`), never `run_sync`. _(listener.h:980-993; mixin.h:63-71)_
- **`async::init()` is a no-op** (the listener is a self-initializing `thread_local`). Do **not**
  `listener::current.clear()` to "re-init" — it destroys live objects' kernel watchers and dangles
  them. _(listener.h:965-977)_
- **`callback(fn)` and `callback(fn, delay<=0)` run `fn` inline immediately,** not next iteration — despite the name they do NOT defer. To break re-entrancy (run after the current handler unwinds) use **`qb::io::async::defer(fn)`**, never a bare `callback` or a magic tiny-delay timer. _(io.h:353-379)_ _(listener.h:1032)_
- **Coroutine lambdas with reference/loop-variable captures dangle after the first suspension.** Store
  the lambda in a variable, pass loop vars by value, and pass `spawn_detached`/`spawn` the callable
  without trailing `()` so its closure is moved into an owning frame. _(scheduler.h:406-435)_
- **Stop the event loop before destroying a coroutine scheduler;** suspended frames are intentionally
  leaked while their watchers reference them. _(scheduler.h:183-203)_
- **Time model is `std::chrono`-only on public signatures.** All timeouts/TTL/intervals/delays take
  `qb::duration` (= `std::chrono::nanoseconds`); it accepts finer-or-equal chrono literals and **rejects
  bare integers at compile time**. `qb::mono_time` (steady) is for deadlines/timers/latency, `qb::wall_time`
  (system) for dates/expiry/wire; subtracting one from the other does not compile. The retired tokens
  `qb::Timestamp`, `qb::Duration`, `qb::TimePoint`, `to_timestamp(`, `to_time_point(` no longer exist —
  never emit them. _(time.h:8-25, aliases :90, :93, :96)_
- **`qb::string<N>` silently truncates** anything past capacity `N` (only `at()` / out-of-range
  `substr` throw). _(string.h:201)_
- **DoS bounds are enforced:** read/write buffers cap at 200 MB (`QB_MAX_READ/WRITE_BUFFER_SIZE`), a
  message at 100 MB (`QB_MAX_MESSAGE_SIZE`); exceeding them marks the protocol `not_ok()` and closes the
  connection. A protocol signals unrecoverable framing errors via `not_ok()`. _(config.h:251, :265, :175; base.h:276)_
- **TLS is secure-by-default; prefer the value-semantic `qb::io::ssl::Context`.** `Context::client()` /
  `Context::server(cert,key).alpn({...})` — copy = share one `SSL_CTX`, **no user `SSL_CTX_free`**,
  fail-closed (`ok()`/`error()`). Hand it to `ssl::socket{ctx}` / `ssl::listener{ctx}` (or `connect()`
  auto-creates a secure client one). The auto/Context client verifies the chain + hostname; `set_insecure()`
  (before connect) disables MITM protection. Raw `create_client_context`/`create_server_context` (caller-owned,
  free with `SSL_CTX_free`) stay as an advanced escape hatch. _(ssl/context.h:128; ssl/socket.h:464, :871, :84, :95)_
- **Filesystem paths are `std::filesystem::path` and resource paths self-locate.** `sys::file::open`/ctor,
  `file_to_pipe`/`pipe_to_file::open`, the SSL cert/key/CA/DH helpers (`create_server_context`,
  `load_ca_certificates`/`load_ca_directory`/`configure_mtls_server_context`/`configure_client_certificate`/`configure_dh_parameters_server`),
  UDS `connect_un`/`n_connect_un`/`listen_un`/`bind_un` (tcp/udp/ssl), `file_watcher`/`directory_watcher::start`,
  and http `listen(uri, path cert={}, path key={})` / `StaticFilesOptions::root_directory` all take `std::filesystem::path`
  (implicit from `std::string`/`const char*`, so source-compatible; Windows opens via `CreateFileW`/Unicode). SSL cert/CA/DH
  paths and http static roots resolve through `qb::io::sys::resolve_resource` — a relative path is found from the cwd **or
  the running executable's own dir** (`self_path()`/`self_dir()`), so a binary shipped next to its assets/certs runs from any
  cwd; absolute paths pass through unchanged. URL/route paths, actor event path fields, and remote/wire paths (redis
  module-load, pgsql server-side COPY) deliberately stay `std::string`. _(file.h:115, :139, :368; ssl/socket.h:95)_
- **`file_watcher`/`directory_watcher` own their watched path string.** qev's `qev_stat` stores the path
  **pointer** without copying, so the watcher keeps a `std::string _watched_path` alive for its lifetime — never
  hand `ev::stat` a temporary's `c_str()`. _(io.h:581-584; qev++.h:696)_
- **Server bind is exclusive on Windows.** `socket::pserve` sets `SO_EXCLUSIVEADDRUSE` on Windows (`#ifdef _WIN32`)
  so an in-use bind fails fast with `WSAEADDRINUSE` and no other process can hijack/shadow the port; POSIX keeps
  `SO_REUSEADDR` (TIME_WAIT rebind). _(sys__socket.cpp:254-271)_
- **`compression.h` hard-`#error`s without `QB_HAS_COMPRESSION`; `crypto.h` does not — its OpenSSL gate is
  per-member.** Including `crypto.h` in a no-SSL build compiles: `qb::crypto` still declares everything that
  needs no OpenSSL (the hex codec the pgsql `bytea` wire format depends on), and the OpenSSL-only members are
  removed from the class, so misuse fails at the **call site** (`no member named 'md5' in 'qb::crypto'`) rather
  than at the `#include`. These are the compile-time flags the headers gate on; the `QB_WITH_SSL`/`QB_WITH_COMPRESSION`
  CMake options, when enabled, define the `QB_HAS_SSL`/`QB_HAS_COMPRESSION` macros. AEAD `decrypt`/`verify_token`
  return empty on auth failure — treat empty as rejection, never as "decrypted to nothing". _(crypto.h:33-44; compression.h:37-39; crypto.h:542-544, :784-790)_

## Build / integration

C++20/23, CMake ≥ 3.24. Embed with `add_subdirectory(qb)` then
`target_link_libraries(app PRIVATE qb::core qb::io)`, or `find_package(qb CONFIG REQUIRED)`. Optional
features default ON and self-disable if their system dep is missing: `QB_WITH_SSL` (OpenSSL, gates crypto/TLS),
`QB_WITH_COMPRESSION` (zlib), `QB_WITH_QUIC=AUTO` (ngtcp2, needs SSL). libev and stduuid are bundled.
qbm modules are **compiled libraries** (not header-only): HTTP/1.1 + HTTP/2 + HTTP/3 + WebSocket as `qbm::http`,
PostgreSQL as `qbm::pgsql`, Redis as `qbm::redis`. WebSocket lives inside qbm-http (`qb::http::ws`); there is no
separate ws module. Load them with `qb_load_modules("${CMAKE_CURRENT_SOURCE_DIR}/qbm")` then
`target_link_libraries(app PRIVATE qbm::http)` (HTTP/2/HTTPS/WSS/JWT need `QB_HAS_SSL`, HTTP/3 needs `QBM_HTTP_HAS_HTTP3`).
