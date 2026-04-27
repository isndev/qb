@page qb_core_actor_md QB-Core: Mastering `qb::Actor`
@brief A comprehensive guide to defining, initializing, and managing the lifecycle of actors using `qb::Actor`.

# QB-Core: Mastering `qb::Actor`

(`qb/core/Actor.h`)

`qb::Actor` is the cornerstone of every application built with the QB Actor Framework. All your custom actors inherit from it, gaining encapsulated state, message-driven behaviour, controlled lifecycle management, and optionally C++23 coroutine support. This guide covers the complete actor API.

---

## 1. Defining Your Actor

### 1.1 Minimal Actor

```cpp
#include <qb/actor.h>

class MyWorker : public qb::Actor {
public:
    bool onInit() override {
        registerEvent<WorkEvent>(*this);
        registerEvent<qb::KillEvent>(*this);
        return true;
    }

    void on(WorkEvent const& ev) { /* handle */ }
    void on(qb::KillEvent const&) { kill(); }
};
```

### 1.2 State Encapsulation

Actor member variables are private to the actor. The QB runtime guarantees sequential execution of handlers — no locks needed for self-state:

```cpp
class StatefulActor : public qb::Actor {
private:
    int                _counter = 0;
    std::string        _name;
    std::vector<float> _history;
public:
    // ...
};
```

### 1.3 Constructor Parameters

Pass configuration at actor creation time. Constructor args flow via `Main::addActor<A>(core, ...)`:

```cpp
class ConfiguredActor : public qb::Actor {
    const std::string _config_path;
    int               _initial_value;
public:
    ConfiguredActor(std::string path, int val)
        : _config_path(std::move(path)), _initial_value(val) {}
    // ...
};

// In main():
engine.addActor<ConfiguredActor>(0, "/etc/app.cfg", 42);
```

### 1.4 Lightweight Actors — `no_default_events`

By default every actor automatically subscribes to four system events: `KillEvent`, `SignalEvent`, `PingEvent`, and `UnregisterCallbackEvent`. For actors that never handle those events (e.g. a pool of short-lived compute workers), passing `qb::no_default_events` to the base constructor skips those subscriptions and reduces setup cost:

```cpp
class ComputeTask : public qb::Actor {
public:
    ComputeTask() : qb::Actor(qb::no_default_events) {}

    bool onInit() override {
        registerEvent<InputEvent>(*this);
        return true;
    }

    void on(InputEvent const& ev) {
        // Process and push result, then self-terminate
        push<ResultEvent>(ev.getSource(), compute(ev.data));
        kill();
    }
};
```

> ⚠️ When using `no_default_events`, the actor will **not** respond to `KillEvent` or system signals unless you explicitly subscribe to them.

---

## 2. Actor Lifecycle

```
addActor<A>(core, ...)
      │
      ▼
 Constructor (A::A(...))
      │
      ▼
 onInit()  ←── register events, acquire resources
      │
 ┌────┴──── return false ──► destructor (A::~A()) — not started
 │
 return true
      │
      ▼
 [Running: on(Event&) / onCallback()]
      │
 kill() called
      │
      ▼
 Pending events drained (current handler finishes)
      │
      ▼
 Destructor (A::~A()) ← RAII cleanup here
```

### 2.1 `onInit()` — Initialization Checkpoint

`onInit()` runs **after** the actor has been assigned its unique `ActorId` but **before** it processes any messages. It is the only safe place to call `registerEvent<T>()`.

```cpp
bool onInit() override {
    // Must register ALL event types the actor will handle
    registerEvent<DataEvent>(*this);
    registerEvent<QueryEvent>(*this);
    registerEvent<qb::KillEvent>(*this);

    // Acquire resources, load configs, find service actors
    auto logger_id = getServiceId<LoggerTag>(getIndex());
    if (!logger_id.is_valid()) {
        return false;  // fail startup — actor will be destroyed
    }
    _logger_id = logger_id;
    return true;
}
```

### 2.2 Event Handlers

For each registered event type, implement a matching public `on()` method:

```cpp
// Read-only handler — const reference
void on(DataEvent const& ev) {
    process(ev.payload);
}

// Mutable handler — required for reply() and forward()
void on(QueryEvent& ev) {
    ev.result = lookup(ev.key);
    reply(ev);  // sends ev back to its source
}
```

### 2.3 Graceful Shutdown — `on(KillEvent)`

```cpp
void on(qb::KillEvent const& /*ev*/) {
    // Optional: notify peers, flush state
    push<ShutdownNotice>(_manager_id, id());
    kill();  // mandatory — marks actor for removal
}
```

The base `Actor::on(KillEvent)` already calls `kill()`. Override only if you need custom cleanup.

### 2.4 Destructor

The destructor is called **after** `kill()` has taken full effect and the actor is removed from the `VirtualCore`:

```cpp
~MyActor() override {
    // RAII members (_file, _conn, etc.) auto-cleaned here
    // DO NOT send events from the destructor
}
```

---

## 3. Key Accessor Methods

| Method | Returns | Description |
|--------|---------|-------------|
| `id()` | `qb::ActorId` | Unique system-wide actor ID |
| `getIndex()` | `qb::CoreId` | VirtualCore this actor runs on |
| `getName()` | `std::string_view` | Demangled class name |
| `getCoreSet()` | `const CoreIdSet&` | All cores in the engine |
| `time()` | `uint64_t` | Cached nanosecond timestamp (per loop tick) |
| `is_alive()` | `bool` | True until `kill()` takes effect |

---

## 4. Sending Events

### 4.1 `push<E>(dest, args...)` — Primary Method

Ordered delivery; handles non-trivially destructible events:

```cpp
push<DataEvent>(target_id, value, label);
auto& ev = push<BatchEvent>(target_id);
ev.items.push_back(item1);   // modify after construction
```

### 4.2 `send<E>(dest, args...)` — Unordered, Trivial Events Only

Lower latency for same-core signals, but **no ordering guarantee**:

```cpp
// FireForgetSignal must be trivially destructible
send<FireForgetSignal>(monitor_id);
```

### 4.3 `broadcast<E>(args...)` — System-Wide

Sends to every actor on every core:

```cpp
broadcast<SystemAlertEvent>("disk full");
```

### 4.4 `reply(event)` / `forward(dest, event)`

Efficiently reuse the received event object:

```cpp
void on(RequestEvent& req) {
    req.result = compute(req.input);
    reply(req);               // back to sender
}

void on(WorkOrder& order) {
    forward(_worker_id, order);  // redirect, preserve source
}
```

### 4.5 `to(dest).push<E>(...)` — EventBuilder (Chained)

Avoids repeated pipe lookups when sending multiple events to the same destination:

```cpp
to(stats_id)
    .push<CountEvent>("logins")
    .push<TimerEvent>("session");
```

### 4.6 `getPipe(dest)` / `pipe.allocated_push<E>(hint, ...)` — Low Level

Pre-size the buffer when carrying large payloads:

```cpp
auto blob = std::make_shared<std::vector<char>>(256 * 1024);
qb::Pipe pipe = getPipe(processor_id);
pipe.allocated_push<BlobEvent>(sizeof(BlobEvent) + blob->size(), blob);
```

---

## 5. Periodic Callbacks — `qb::ICallback`

Actors that need to run code on every loop iteration inherit from `qb::ICallback`:

```cpp
class PollingActor : public qb::Actor, public qb::ICallback {
    int _poll_count = 0;
public:
    bool onInit() override {
        registerCallback(*this);   // activate
        return true;
    }

    void onCallback() override {
        ++_poll_count;
        if (pollExternalSystem())
            push<DataReadyEvent>(id());
        if (_poll_count > 1000) {
            unregisterCallback();  // deactivate
            kill();
        }
    }
};
```

> ⚠️ `onCallback()` runs on the VirtualCore event-loop thread. **It must be fast and non-blocking.**

---

## 6. Referenced Actors & `RefActorHandle<T>`

### 6.1 Raw `addRefActor<T>()` — Use with Caution

Creates a child actor on the **same VirtualCore** and returns a raw pointer:

```cpp
HelperActor* _helper = addRefActor<HelperActor>(id());
if (!_helper) { return false; }  // onInit() failed
```

**Problem:** if `_helper` calls `kill()`, the pointer becomes dangling. Any subsequent dereference is Undefined Behaviour.

### 6.2 Safe `addRefHandle<T>()` — Recommended

Wraps the raw pointer in a `RefActorHandle<T>` that performs an O(1) liveness check on every dereference:

```cpp
class ParentActor : public qb::Actor {
    qb::RefActorHandle<HelperActor> _helper;

public:
    bool onInit() override {
        _helper = addRefHandle<HelperActor>(id());
        if (!_helper) return false;       // creation failed

        registerEvent<ResultEvent>(*this);
        return true;
    }

    void dispatch(int x) {
        if (_helper) {                    // liveness check — O(1)
            push<TaskEvent>(_helper->id(), x);  // safe actor-model send
        }
    }

    void on(ResultEvent const& ev) {
        qb::io::cout() << "result = " << ev.value << "\n";
    }
};
```

`RefActorHandle<T>` is copyable and default-constructible (empty handle). Its `operator->()` and `operator*()` assert non-null in debug builds.

---

## 7. C++23 Coroutines — `spawn_async`

### 7.1 Overview

`spawn_async()` is the **only** safe way to use coroutines inside an actor. It launches a `qb::io::async::task<void>` that runs concurrently with the actor's normal event processing. While the coroutine is suspended (at any `co_await` point), the actor continues to receive and process events normally.

### 7.2 Safety Contract

> **Rule 1 — No actor state after `co_await`**
> The actor may be destroyed while the coroutine is suspended. Accessing `this->_member` after a `co_await` is **Undefined Behaviour**.
>
> **Rule 2 — Copy everything needed before spawn**
> Capture all required data by value before the first `co_await`.
>
> **Rule 3 — Use only `CoroContext` after suspension**
> `ctx.push<E>(...)` and `ctx.push_to<E>(dest, ...)` are safe regardless of the actor's lifetime.

### 7.3 ✅ Safe Pattern

```cpp
void on(FetchRequest& req) {
    // --- Copy ALL needed data BEFORE spawning ---
    std::string url    = req.url;
    ActorId     sender = req.getSource();
    ActorId     me     = id();

    spawn_async([url, sender, me](auto ctx) -> qb::io::async::task<void> {
        // After this co_await, 'this' may be gone — only ctx is safe
        auto body = co_await http_get(url);

        ctx.template push_to<FetchResult>(sender, me, body);
    });
}
```

### 7.4 ❌ Dangerous Anti-pattern

```cpp
void on(FetchRequest& req) {
    spawn_async([this](auto ctx) -> qb::io::async::task<void> {
        co_await sleep(100ms);
        this->_result = compute();  // ❌ 'this' may be dangling — CRASH
    });
}
```

### 7.5 Coroutine Introspection

```cpp
bool    has_active_coroutines()  const; // any pending co_await?
std::size_t active_coroutine_count() const; // how many?
```

---

## 8. Actor Discovery — `require<T>()`

Broadcast a ping to discover running actors of a specific type:

```cpp
bool onInit() override {
    registerEvent<qb::RequireEvent>(*this);
    require<LoggerService>();   // will trigger on(RequireEvent&) for each live instance
    return true;
}

void on(qb::RequireEvent const& ev) {
    if (is<LoggerService>(ev) && ev.status == qb::ActorStatus::Alive) {
        _logger_id = ev.getSource();
    }
}
```

---

## 9. Quick Reference

| Category | Method | Notes |
|----------|--------|-------|
| **Identity** | `id()`, `getIndex()`, `getName()` | Read-only accessors |
| **Lifecycle** | `kill()`, `is_alive()` | Non-blocking |
| **Events** | `registerEvent<E>(*this)`, `unregisterEvent<E>(*this)` | Call from `onInit()` |
| **Send** | `push<E>(dest, ...)`, `send<E>(dest, ...)`, `broadcast<E>(...)` | Choose based on ordering need |
| **Reuse** | `reply(ev)`, `forward(dest, ev)` | Non-const event& required |
| **Fluent** | `to(dest).push<E>(...)` | Multiple events to same dest |
| **Low level** | `getPipe(dest).push<E>(...)` / `allocated_push<E>(hint, ...)` | Bulk / large payloads |
| **Callback** | `registerCallback(*this)`, `unregisterCallback()` | Needs `ICallback` mixin |
| **Child** | `addRefHandle<A>(...)` / `addRefActor<A>(...)` | Same core only |
| **Discovery** | `require<A...>()`, `getService<A>()`, `getServiceId<Tag>(core)` | Actor lookup |
| **Coroutine** | `spawn_async(func)`, `has_active_coroutines()` | C++23 only |
| **Time** | `time()` | Cached ns timestamp — stable within one tick |

**(Next:** [QB-Core: Event Messaging](./messaging.md) — deep dive into the event system.)**
**(See also:** [QB-Core: Actor Patterns & Utilities](./patterns.md), [Core Concepts: The Actor Model](./../2_core_concepts/actor_model.md))**
