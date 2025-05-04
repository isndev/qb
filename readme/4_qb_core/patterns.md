# QB-Core: Actor Patterns & Utilities

This section describes common patterns and specific features available to `qb::Actor` beyond basic event handling.

## Service Actors (`qb::ServiceActor<Tag>`)

*   **Header:** `qb/include/qb/core/Actor.h`
*   **Purpose:** Implement singleton actors **per VirtualCore**. Useful for core-local services or resource managers.
*   **How to Define:**
    1.  Create a unique empty tag struct: `struct MyServiceTag {};`
    2.  Inherit: `class MyService : public qb::ServiceActor<MyServiceTag> { ... };`
*   **How to Add:** Use `main.addActor<MyService>(core_id, ...)` like any other actor. The framework enforces the singleton-per-core rule during startup.
*   **How to Access:**
    *   From *any* actor: `qb::ActorId svc_id = qb::Actor::getServiceId<MyServiceTag>(target_core_id);` Then use `push`/`send` to `svc_id`.
    *   From actors *on the same core*: `MyService* svc_ptr = getService<MyService>();` Returns `nullptr` if not found on the current core. Allows direct method calls (use cautiously) or sending events via `svc_ptr->id()`.

```cpp
// --- Service Definition ---
struct CoreLoggerTag {};
class CoreLogger : public qb::ServiceActor<CoreLoggerTag> {
public:
    bool onInit() override { registerEvent<LogRequest>(*this); return true; }
    void on(const LogRequest& event) { /* Log event.message */ }
};

// --- Accessing Actor ---
class Worker : public qb::Actor {
private: qb::ActorId _logger_id;
public:
    bool onInit() override {
        _logger_id = getServiceId<CoreLoggerTag>(getIndex()); // Get logger on my core
        return _logger_id.is_valid();
    }
    void doSomething() {
        if (_logger_id.is_valid()) {
            push<LogRequest>(_logger_id, "Worker task started");
        }
    }
};
```
*   **(Ref:** `test-actor-add.cpp`, `test-actor-service-event.cpp`**)

## Periodic Callbacks (`qb::ICallback`)

*   **Header:** `qb/include/qb/core/ICallback.h`
*   **Purpose:** Allow an actor to execute code on every iteration of its `VirtualCore`'s event loop, *after* event processing.
*   **How to Use:**
    1.  Inherit: `class MyActor : public qb::Actor, public qb::ICallback { ... };`
    2.  Implement: `virtual void onCallback() override { /* your logic */ }`
    3.  Register: `registerCallback(*this);` (usually in `onInit`).
    4.  Unregister (Optional): `unregisterCallback(*this);` or `unregisterCallback();` (can be called from `onCallback` itself or an event handler).
*   **Caution:** `onCallback` logic **must be fast and non-blocking** as it runs directly in the core's main loop.
*   **Use Cases:** Polling external state, periodic checks, simulations, game loops.

```cpp
class Ticker : public qb::Actor, public qb::ICallback {
private: int _ticks = 0;
public:
    bool onInit() override { registerCallback(*this); return true; }
    void onCallback() override {
        _ticks++;
        if (_ticks % 100 == 0) broadcast<TickEvent>(_ticks);
        if (_ticks >= 1000) unregisterCallback(); // Stop after 1000 ticks
    }
};
```
*   **(Ref:** `test-actor-callback.cpp`, `example1_basic_actors.cpp`, `example3_lifecycle.cpp`, `example10_distributed_computing.cpp`**)

## Referenced Actors (`addRefActor`)

*   **Header:** `qb/include/qb/core/Actor.h`
*   **Purpose:** Allow an actor (parent) to create and hold a direct pointer to another actor (child) that resides **on the same core**.
*   **How to Create:** `ChildActor* child_ptr = addRefActor<ChildActor>(arg1, arg2);` (Call from within the parent actor).
*   **Return:** Returns `ChildActor*` on successful creation and initialization (`onInit` returned true), otherwise `nullptr`.
*   **Lifecycle:** The parent **does not own** the child actor. The child manages its own lifecycle and must call `kill()` to terminate. The parent needs to handle the possibility of the child pointer becoming dangling if the child terminates independently.
*   **Communication:**
    *   Parent -> Child: Can use `push`/`send` to `child_ptr->id()` (recommended) OR call public methods directly on `child_ptr` (use with **extreme caution**, bypasses mailbox).
    *   Child -> Parent: Needs the parent's `ActorId` (e.g., passed via constructor).
*   **Use Cases:** Tightly coupled helper actors, worker pools managed by a same-core supervisor where direct pointer access offers significant *measured* performance benefit over events.
*   **Risks:** Direct method calls break the standard actor message-passing guarantees and require careful consideration of state consistency.

**(Ref:** `test-actor-add.cpp::TestRefActor`, `test-actor-state-persistence.cpp`**)

## Actor Dependency Resolution (`require<T>`, `RequireEvent`)

*   **Headers:** `qb/include/qb/core/Actor.h`, `qb/include/qb/core/Event.h`
*   **Purpose:** Allow an actor to discover the `ActorId`s of running actors of a specific *type* within the system, often used to find `ServiceActor`s.
*   **How to Use:**
    1.  **Register:** Actor registers interest by calling `require<TargetActorType>();` (usually in `onInit`).
    2.  **Handle:** Implement `on(const qb::RequireEvent& event)`. Inside, check `if (is<TargetActorType>(event))` and if `event.status == qb::ActorStatus::Alive`, then `event.getSource()` is the `ActorId` of a live instance.
*   **Mechanism:** `require<T>` broadcasts an internal `PingEvent`. Live actors of type `T` respond, triggering a `RequireEvent` back to the original requester for each live instance found.

```cpp
// Client actor needing a LoggerService
class MyClient : public qb::Actor {
private: qb::ActorId _logger;
public:
    bool onInit() override {
        registerEvent<qb::RequireEvent>(*this);
        require<LoggerService>(); // Ask framework to find LoggerService instances
        return true;
    }
    void on(const qb::RequireEvent& event) {
        if (is<LoggerService>(event) && event.status == qb::ActorStatus::Alive) {
            _logger = event.getSource();
            std::cout << "Found logger service: " << _logger << std::endl;
            // Can now send log messages
            push<LogEvent>(_logger, "Client initialized");
            // Optional: Unsubscribe if only one needed?
            // unregisterEvent<qb::RequireEvent>(*this);
        }
    }
};
```
*   **(Ref:** `test-actor-dependency.cpp`**) 