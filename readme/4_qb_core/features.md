@page qb_core_features_md QB-Core: Key Features & Capabilities
@brief A summary of the core features provided by the QB Actor Engine for concurrent C++ development.

# QB-Core: Key Features & Capabilities

`qb-core` equips developers with a comprehensive, production-ready toolkit to build robust and scalable actor-based applications on top of the `qb-io` asynchronous foundation. Here is a detailed rundown of its main capabilities.

---

## I. Actor Lifecycle & Management

*   **Core Actor Abstraction (`qb::Actor`):** The fundamental base class for all user-defined actors, encapsulating state and behaviour in a thread-safe, isolated unit.

*   **Flexible Actor Creation:**

    | Method | Description |
    |--------|-------------|
    | `qb::Main::addActor<A>(core_id, ...)` | Add a single actor to a specific core before the engine starts |
    | `main.core(id).builder().addActor<A>(...)` | Fluent builder for adding multiple actors to the same core |
    | `Actor::addRefActor<A>(...)` | Create a child actor on the **same core**; returns raw pointer |
    | `Actor::addRefHandle<A>(...)` | Create a child actor and wrap it in a `RefActorHandle<A>` (safe) |

*   **Unique Actor Identification (`qb::ActorId`):** Each actor receives a system-unique ID (composed of `CoreId` + `ServiceId`) used for all event addressing.

*   **Controlled Initialization (`virtual bool onInit()`):** Called after construction and ID assignment — the designated place for:
    *   Registering event handlers (`registerEvent<MyEvent>(*this)`).
    *   Acquiring resources, connecting to services.
    *   Returning `false` aborts startup and triggers immediate destruction.

*   **Graceful Termination (`kill()`):** Marks the actor for removal. The `VirtualCore` completes any in-flight handler before destroying the actor.

*   **RAII Destruction (`virtual ~Actor()`):** Called only after the actor has been fully removed from the engine — RAII-managed members are safely destroyed.

*   **Liveness Check (`is_alive()`):** Query whether `kill()` has been called and taken effect.

*   **Lightweight Actors (`qb::no_default_events`):** Pass `qb::no_default_events` to the `Actor` constructor to skip the four automatic system-event subscriptions (`KillEvent`, `SignalEvent`, `PingEvent`, `UnregisterCallbackEvent`). Useful for high-throughput pools of short-lived actors where those handlers are not needed.

---

## II. Event System & Asynchronous Messaging

*   **Event Definition (`qb::Event`):** The base class for all inter-actor messages. Events are primarily typed data carriers.

*   **Collision-Free Type IDs:** Each distinct event type receives a unique, dense `TypeId` via `qb::type_id<T>()` — a monotonic atomic counter (no ASLR-based collisions possible).

*   **Type-Safe Dispatch:** Events are routed to the correct `on(EventType&)` handler via the internal `router::memh` table keyed on `TypeId`.

*   **Event Subscription:**
    *   `registerEvent<MyEvent>(*this)` — subscribe inside `onInit()`.
    *   `unregisterEvent<MyEvent>(*this)` — dynamically unsubscribe at any point.

*   **Quality of Service (QoS) Levels:**

    | Base Type | Priority | Notes |
    |-----------|----------|-------|
    | `qb::Event` / `qb::EventQOS2` | High | Default; ordered delivery via `push()` |
    | `qb::EventQOS1` | Medium | Ordered; suitable for latency-sensitive but non-critical events |
    | `qb::EventQOS0` | Low / best-effort | Unordered; must be trivially destructible |

*   **Versatile Message Sending:**

    | Method | Ordering | Constraint | Use case |
    |--------|----------|------------|----------|
    | `push<E>(dest, ...)` | FIFO per pair | None | **Default, recommended** |
    | `send<E>(dest, ...)` | Unordered | Trivially destructible | Low-latency fire-and-forget |
    | `broadcast<E>(...)` | N/A | None | System-wide announcements |
    | `push<E>(BroadcastId(core), ...)` | FIFO | None | Core-scoped broadcast |
    | `reply(event)` | N/A | Non-const `event&` | Efficient request/response |
    | `forward(dest, event)` | N/A | Non-const `event&` | Efficient delegation/routing |

*   **Optimised Sending Utilities:**
    *   `to(dest).push<E>(...)` — `EventBuilder` for chained multi-event sends; avoids repeated pipe lookups.
    *   `getPipe(dest)` — direct `qb::Pipe` access for performance-critical paths.
    *   `pipe.allocated_push<E>(size_hint, ...)` — pre-size the pipe buffer for large-payload events to avoid internal reallocation.

*   **Event Reuse Patterns:** `reply()` and `forward()` reuse the event object in-place, avoiding allocation and copy for the response path.

---

## III. Concurrency, Parallelism & Scheduling

*   **Engine Controller (`qb::Main`):** Manages `VirtualCore` threads, startup, stop-token-based cancellation, signal handling, and error aggregation.

*   **Worker Threads (`qb::VirtualCore`):** Each core runs an independent event loop backed by a `qb::io::async::listener`. Actors are thread-affinized — they never migrate between cores.

*   **Multi-Core Execution:** True parallel processing; actors are statically assigned to cores at creation time.

*   **Lock-Free Inter-Core Messaging:** Events between cores travel through MPSC (`lockfree::mpsc::ringbuffer`) mailboxes — no mutex contention on the hot path.

*   **Configuration (`CoreInitializer` via `qb::Main::core(id)`):**
    *   `setAffinity(CoreIdSet)` — pin the worker thread to specific physical CPUs (Linux `pthread_setaffinity_np` / Windows `SetThreadAffinityMask`).
    *   `setLatency(ns)` — `0` = busy-spin (lowest latency, 100% CPU); `>0` = sleep when idle (trades latency for CPU efficiency).

*   **Stop-Token Cancellation:** `qb::Main` uses `std::stop_source` / `std::stop_token` (C++20) for clean, signal-free shutdown that works on all platforms.

---

## IV. C++23 Coroutine Integration

*   **`Actor::spawn_async(func)`:** Launch a coroutine (`qb::io::async::task<void>`) from any event handler. The actor continues processing other events while the coroutine is suspended at `co_await` points.

*   **`qb::CoroContext`:** Passed by value to the coroutine. Captures the actor's `ActorId` at spawn time — safe to use after `co_await` even if the parent actor has been destroyed.

*   **Critical safety contract:**
    1. **Never** access actor member variables (`this->_member`) after any `co_await`.
    2. **Copy** all needed state by value **before** the first `co_await`.
    3. **Only** use `ctx.push<E>(...)` / `ctx.push_to<E>(dest, ...)` after suspension.

    ```cpp
    void on(FetchRequest& req) {
        std::string url = req.url;       // copy BEFORE spawn
        ActorId     me  = id();

        spawn_async([url, me](auto ctx) -> qb::io::async::task<void> {
            auto data = co_await http_get(url);  // actor may die here
            ctx.template push_to<ResultEvent>(me, data); // safe
        });
    }
    ```

*   **Active Coroutine Tracking:** `has_active_coroutines()` / `active_coroutine_count()` let you inspect pending asynchronous work before destroying an actor.

---

## V. Actor Patterns & Utilities

*   **Periodic Tasks (`qb::ICallback`):** Actors that also inherit from `ICallback` can call `registerCallback(*this)` to receive an `onCallback()` invocation on every loop iteration. The method must be fast and non-blocking.

*   **Service Actors (`qb::ServiceActor<Tag>`):** Singleton actors per `VirtualCore`, identified by a unique `Tag` struct. Discovered via:
    *   `getService<MyService>()` — same-core direct pointer access.
    *   `getServiceId<MyServiceTag>(core_id)` — cross-core `ActorId` lookup.

*   **Dependency Discovery (`require<TargetActor>()`):** Broadcasts a `PingEvent`; live actors of the target type respond with `qb::RequireEvent`. Handle in `on(qb::RequireEvent&)` using `is<TargetActor>(event)`.

*   **Safe Child-Actor References (`RefActorHandle<T>`):** Wraps the raw pointer returned by `addRefActor<T>()` with an O(1) liveness check on every dereference, preventing dangling-pointer UB if the child actor terminates independently.

    ```cpp
    _helper = addRefHandle<HelperActor>(id());
    if (_helper) {               // checks is_alive internally
        push<Task>(_helper->id(), data);
    }
    ```

These features provide a comprehensive toolkit for building complex, concurrent, and high-performance applications using the Actor Model in modern C++.

**(Next:** Explore [QB-Core: Mastering qb::Actor](./actor.md) for a deep dive into defining actors.)**
