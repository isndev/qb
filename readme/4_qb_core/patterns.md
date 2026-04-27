@page qb_core_patterns_md QB-Core: Common Actor Patterns & Utilities
@brief Discover and implement common actor design patterns and utilities within the QB Framework for robust application structure.

# QB-Core: Common Actor Patterns & Utilities

Beyond the fundamental actor and event mechanisms, `qb-core` supports and simplifies several design patterns that are highly useful in building robust and maintainable actor-based systems. This guide explores key patterns and utilities available to `qb::Actor` implementations.

---

## 1. Finite State Machine (FSM) with Actors

Actors model naturally as FSMs: their member variables represent state, and event handlers implement transitions.

**Implementation recipe:**

1. Define states with `enum class`.
2. Store current state as a member variable.
3. Use `switch`/`if` on state inside event handlers.
4. Timed transitions: use `qb::io::async::callback` or `spawn_async` for delays.

```cpp
#include <qb/actor.h>
#include <qb/io/async.h>
#include <qb/string.h>

enum class OrderState { PENDING_PAYMENT, PROCESSING, SHIPPED, CANCELLED };

struct PlaceOrderEvent    : qb::Event { qb::string<128> details; };
struct PaymentReceived    : qb::Event { qb::string<64>  ref; };
struct ShipOrderEvent     : qb::Event {};

class OrderActor : public qb::Actor {
    OrderState      _state = OrderState::PENDING_PAYMENT;
    qb::string<128> _data;
public:
    bool onInit() override {
        registerEvent<PlaceOrderEvent>(*this);
        registerEvent<PaymentReceived>(*this);
        registerEvent<ShipOrderEvent>(*this);
        registerEvent<qb::KillEvent>(*this);
        return true;
    }

    void on(PlaceOrderEvent const& ev) {
        _data  = ev.details;
        _state = OrderState::PENDING_PAYMENT;
        qb::io::cout() << "Order placed: " << _data.c_str() << "\n";
    }

    void on(PaymentReceived const& ev) {
        if (_state == OrderState::PENDING_PAYMENT) {
            _state = OrderState::PROCESSING;
            qb::io::async::callback([this]() {
                if (is_alive()) push<ShipOrderEvent>(id());
            }, 2.0);
        }
    }

    void on(ShipOrderEvent const& /*ev*/) {
        if (_state == OrderState::PROCESSING)
            _state = OrderState::SHIPPED;
    }

    void on(qb::KillEvent const& /*ev*/) { kill(); }
};
```

**(Full example:** `example/core/example8_state_machine.cpp` — coffee machine FSM)**

---

## 2. Service Actors — Core-Local Singletons

`qb::ServiceActor<Tag>` ensures at most **one instance per `VirtualCore`** for a given tag type, making it ideal for per-core resources (loggers, metrics, caches).

### Defining a Service Actor

```cpp
struct LoggerTag {};    // unique, empty tag struct

struct LogEvent : qb::Event {
    qb::string<128> message;
    explicit LogEvent(const char* msg) : message(msg) {}
};

class CoreLogger : public qb::ServiceActor<LoggerTag> {
public:
    bool onInit() override {
        registerEvent<LogEvent>(*this);
        registerEvent<qb::KillEvent>(*this);
        return true;
    }

    void on(LogEvent const& ev) {
        qb::io::cout() << "[Core " << getIndex() << "] " << ev.message.c_str() << "\n";
    }

    void on(qb::KillEvent const& /*ev*/) { kill(); }
};
```

### Adding & Accessing

```cpp
// Setup — at most one CoreLogger per core
engine.addActor<CoreLogger>(0);
engine.addActor<CoreLogger>(1);

// Cross-core: look up ActorId and push event
qb::ActorId logger_id = qb::Actor::getServiceId<LoggerTag>(target_core);
if (logger_id.is_valid())
    push<LogEvent>(logger_id, "hello from another actor");

// Same-core: direct pointer (avoids mailbox — use with care)
if (auto* logger = getService<CoreLogger>())
    push<LogEvent>(logger->id(), "same-core log");
```

**(Reference:** `test-actor-add.cpp`, `test-actor-service-event.cpp`)**

---

## 3. Periodic Callbacks — `qb::ICallback`

For polling, heartbeats, or any logic that must run on every loop iteration.

```cpp
#include <qb/actor.h>
#include <qb/icallback.h>

struct TickEvent : qb::Event { int count; };

class HeartbeatActor : public qb::Actor, public qb::ICallback {
    int _count = 0;
public:
    bool onInit() override {
        registerEvent<qb::KillEvent>(*this);
        registerCallback(*this);   // activate periodic callback
        return true;
    }

    void onCallback() override {
        ++_count;
        if (_count % 2 == 0)
            broadcast<TickEvent>(_count);

        if (_count >= 10) {
            unregisterCallback();  // deactivate
            kill();
        }
    }

    void on(qb::KillEvent const& /*ev*/) { kill(); }
};
```

> **Never** block inside `onCallback()` — it stalls the entire VirtualCore.

**(Reference:** `test-actor-callback.cpp`, `example1_basic_actors.cpp`)**

---

## 4. Referenced Actors — `addRefActor` / `addRefHandle`

### 4.1 Raw `addRefActor` — Requires Care

Creates a child actor on the **same core** and returns a raw pointer. The pointer becomes dangling when the child terminates:

```cpp
ChildHelperActor* _raw = addRefActor<ChildHelperActor>(id());
// _raw may become dangling at any time!
```

### 4.2 `RefActorHandle<T>` — Recommended Safe Wrapper

`addRefHandle<T>()` wraps the pointer in a `RefActorHandle<T>` that performs an O(1) liveness check on every dereference:

```cpp
struct HelperTask   : qb::Event { int a, b; };
struct HelperResult : qb::Event { int result; };

class ChildHelperActor : public qb::Actor {
    qb::ActorId _parent;
public:
    explicit ChildHelperActor(qb::ActorId parent) : _parent(parent) {}
    bool onInit() override {
        registerEvent<HelperTask>(*this);
        registerEvent<qb::KillEvent>(*this);
        return true;
    }
    void on(HelperTask const& ev) { push<HelperResult>(_parent, ev.a + ev.b); }
    void on(qb::KillEvent const& /*ev*/) { kill(); }
};

class ParentActor : public qb::Actor {
    qb::RefActorHandle<ChildHelperActor> _helper;
public:
    bool onInit() override {
        _helper = addRefHandle<ChildHelperActor>(id());
        if (!_helper) return false;
        registerEvent<HelperResult>(*this);
        registerEvent<qb::KillEvent>(*this);
        return true;
    }
    void doWork(int a, int b) {
        if (_helper)                          // O(1) liveness check — safe
            push<HelperTask>(_helper->id(), a, b);
    }
    void on(HelperResult const& ev) {
        qb::io::cout() << "result = " << ev.result << "\n";
    }
    void on(qb::KillEvent const& /*ev*/) {
        if (_helper) push<qb::KillEvent>(_helper->id());
        kill();
    }
};
```

**`RefActorHandle<T>` API:**

| Method | Description |
|--------|-------------|
| `valid()` / `operator bool()` | True if constructed with a non-null actor |
| `id()` | `ActorId` of the referenced actor |
| `get()` | Live pointer or `nullptr` if terminated |
| `operator->()` | `get()` with debug-mode assertion |
| `operator*()` | Dereference (UB if `get() == nullptr`) |

**(Reference:** `test-actor-add.cpp::TestRefActor`)**

---

## 5. Actor Dependency Resolution — `require<T>`

Dynamically discover running actor instances without knowing their `ActorId` at startup.

```cpp
class ClientActor : public qb::Actor {
    qb::ActorId _logger;
    bool        _logger_found = false;
public:
    bool onInit() override {
        registerEvent<qb::RequireEvent>(*this);
        registerEvent<qb::KillEvent>(*this);
        require<CoreLogger>();    // broadcasts a discovery ping
        return true;
    }

    void on(qb::RequireEvent const& ev) {
        if (is<CoreLogger>(ev) && ev.status == qb::ActorStatus::Alive) {
            _logger       = ev.getSource();
            _logger_found = true;
            push<LogEvent>(_logger, "Client connected!");
        }
    }

    void on(qb::KillEvent const& /*ev*/) { kill(); }
};
```

**Notes:**
- Multiple `RequireEvent` responses arrive if several instances exist across cores.
- `require<A, B, C>()` discovers multiple types at once.
- `is<T>(ev)` checks the type tag embedded in the event.

**(Reference:** `test-actor-dependency.cpp`)**

---

## 6. C++23 Coroutine Pattern — `spawn_async`

Use `spawn_async` to perform non-blocking async I/O while the actor stays responsive.

```cpp
void on(ApiRequest& req) {
    // 1. Copy ALL required state BEFORE spawn — 'this' may be gone after co_await
    std::string url    = req.url;
    ActorId     sender = req.getSource();

    spawn_async([url, sender](auto ctx) -> qb::io::async::task<void> {
        auto response = co_await http_client::get(url);
        ctx.template push_to<ApiResponse>(sender, response.body);
    });
}
```

For graceful shutdown with pending coroutines, defer termination:

```cpp
void on(qb::KillEvent const& /*ev*/) {
    if (has_active_coroutines()) {
        qb::io::async::callback([this]() {
            if (is_alive()) push<qb::KillEvent>(id());
        }, 0.1);
    } else {
        kill();
    }
}
```

**(See also:** [QB-Core: Mastering qb::Actor](./actor.md))**

---

These patterns provide flexible and powerful ways to structure actor-based applications, promoting separation of concerns, safe resource management, and performant concurrent behaviour.

**(Next:** Review [Developer Guides](./../6_guides/README.md) for higher-level application patterns and best practices.)**
