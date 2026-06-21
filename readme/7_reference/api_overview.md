# Public API overview

> **Audience:** Adopter · **Status:** stable · **Verified-against:** qb 2.0.0 (C++20 default, C++23 supported)

A reference map of the public API: the namespaces, key types, and signatures of `qb-core` and
`qb-io`, each linked to the header that owns it.

**Prerequisites:** [Core concepts](../2_core_concepts/) — **See also:** [Glossary](./glossary.md),
[Core invariants](./core_invariants.md), [qb-io invariants](./io_invariants.md),
[Lock-free primitives](./lockfree_primitives.md)

## Summary

The framework ships two libraries:

- **`qb-core`** — the actor engine: `Main`, `Actor`, `Event`, `ActorId`, `ServiceActor`, `ICallback`.
- **`qb-io`** — the asynchronous runtime: the event loop, transports, coroutines, protocols, crypto,
  and shared utilities.

This page is a directory, not a tutorial. Each entry states where a type lives and what its key public
methods are, then links to the page that explains it in depth. Signatures here are the public contract;
the cited header is ground truth. Public types are reached through umbrella headers — `qb/main.h`,
`qb/actor.h`, `qb/event.h`, `qb/io.h` — while this page cites the defining header under
`qb/include/qb/` so you can confirm any signature directly.

A note on naming and time: identifiers use their exact source spelling (`onInit`, `addActor`,
`getService`). Every timeout, delay, latency, and interval in the public API is a
[`qb::duration`](#time) — an alias for `std::chrono::nanoseconds`. The pre-2.0 framework-specific
time classes were removed in favor of these `std::chrono` aliases — the current vocabulary is
`qb::duration`, `qb::mono_time`, and `qb::wall_time`; see the
[migration guide](../6_guides/migration_guide.md) for the old-to-new mapping.

## Namespace map

| Namespace | Library | Contents |
|---|---|---|
| `qb` | both | `Actor`, `Main`, `Event`, `ActorId`, `ServiceActor`, `ICallback`, `string`, `json`, `uuid`, time vocabulary, containers |
| `qb::io` | qb-io | `socket`, `endpoint`, `uri`, `cout`/`cerr`, the `use<>` helper, stream and transport types |
| `qb::io::async` | qb-io | event loop (`listener`), `callback`, `with_timeout`, I/O CRTP bases, coroutines, event types |
| `qb::io::tcp`, `qb::io::udp` | qb-io | TCP and UDP sockets and listeners |
| `qb::io::tcp::ssl`, `qb::io::ssl` | qb-io | TLS sockets, listeners, and context helpers (requires `QB_WITH_SSL`) |
| `qb::io::transport` | qb-io | concrete stream specializations (`tcp`, `udp`, `stcp`, `file`, …) |
| `qb::io::protocol`, `qb::protocol::*` | qb-io | message-framing protocols |
| `qb::io::sys` | qb-io | synchronous native-file wrappers |
| `qb::crypto`, `qb::jwt`, `qb::compression` | qb-io | cryptography, JWTs, compression |
| `qb::lockfree`, `qb::allocator`, `qb::endian` | both | lock-free primitives, buffers, byte-order helpers |

---

## `qb-core`: the actor engine

The actor runtime: lightweight actors, a type-safe event system with ordered delivery, multicore
scheduling, and lock-free inter-core message passing. See [qb-core](../4_qb_core/) for the full treatment.

### `qb::Main`

- **Header:** `qb/core/Main.h` (include `qb/main.h`)
- **Role:** the engine controller. It owns one `VirtualCore` worker thread per configured core, wires the
  inter-core mailboxes, and manages start, stop, join, and signal handling.

| Member | Signature | Notes |
|---|---|---|
| Constructor | `Main() noexcept` | Initializes the engine structure. |
| `addActor` | `template<class _Actor, class... _Args> ActorId addActor(CoreId index, _Args&&... args)` | Adds an actor to core `index` before start; returns its `ActorId`, or `ActorId::NotFound` on failure. Shorthand for `core(index).addActor<_Actor>(...)`. |
| `core` | `CoreInitializer& core(CoreId index)` | Pre-start, per-core configuration handle. |
| `setLatency` | `void setLatency(qb::duration latency = qb::duration::zero())` | Default idle latency for cores without a per-core override. `zero()` means busy-spin. |
| `usedCoreSet` | `qb::CoreIdSet usedCoreSet() const` | Cores the engine will use. |
| `start` | `void start(bool async = true) noexcept` | Starts the worker threads. With `async == false`, the calling thread becomes a worker and the call blocks until shutdown. |
| `join` | `void join()` | Blocks until all workers terminate; pair with `start(true)`. |
| `stop` | `static void stop() noexcept` | Requests graceful shutdown from any thread, including a signal handler. |
| `hasError` | `bool hasError() const noexcept` | Whether a core terminated on an unhandled error; check after `join()`. |
| `registerSignal` | `static void registerSignal(int signum) noexcept` | Routes an OS signal to `stop()`. `SIGINT` and `SIGTERM` are registered by default. |
| `unregisterSignal` | `static void unregisterSignal(int signum) noexcept` | Removes a signal handler. |
| `ignoreSignal` | `static void ignoreSignal(int signum) noexcept` | Sets a signal to be ignored. |

See [The engine](../4_qb_core/engine.md).

### `qb::CoreInitializer`

- **Header:** `qb/core/Main.h`
- **Role:** pre-start configuration for one `VirtualCore`, obtained from `Main::core(index)`.

| Member | Signature |
|---|---|
| `addActor` | `template<class _Actor, class... _Args> ActorId addActor(_Args&&... args) noexcept` |
| `builder` | `ActorBuilder builder() noexcept` |
| `setAffinity` | `CoreInitializer& setAffinity(CoreIdSet const& cores = {}) noexcept` |
| `setLatency` | `CoreInitializer& setLatency(qb::duration latency = qb::duration::zero()) noexcept` |
| `clear` | `void clear() noexcept` |

`ActorBuilder` (from `builder()`) chains `addActor<T>(...)` calls and collects their ids via `idList()`;
`valid()` reports whether all preceding additions succeeded.

### `qb::Actor`

- **Header:** `qb/core/Actor.h` (include `qb/actor.h`)
- **Role:** the base class for every user-defined actor. An actor owns its state, runs on one
  `VirtualCore`, and communicates only by events. Derive from it and override `on(...)` handlers.

**Lifecycle**

| Member | Signature | Notes |
|---|---|---|
| Constructor | `Actor() noexcept` | Subscribes to the four default system events (`KillEvent`, `SignalEvent`, `UnregisterCallbackEvent`, `PingEvent`). |
| Tagged constructor | `explicit Actor(no_default_events_t) noexcept` | Skips the default subscriptions; the derived class registers what it needs in `onInit()`. |
| `onInit` | `virtual bool onInit()` | Called once after construction, before event processing. Register events and acquire resources here. Return `false` to abort the launch. |
| Destructor | `virtual ~Actor() noexcept` | |
| `kill` | `void kill() const noexcept` | Marks the actor for termination. |
| `is_alive` | `bool is_alive() const noexcept` | False once `kill()` has taken effect. |

**Identity and context**

| Member | Signature |
|---|---|
| `id` | `ActorId id() const noexcept` |
| `getIndex` | `CoreId getIndex() const noexcept` |
| `getName` | `std::string_view getName() const noexcept` |
| `getCoreSet` | `const CoreIdSet& getCoreSet() const noexcept` |
| `time` | `uint64_t time() const noexcept` |

`time()` returns the core's cached wall-clock timestamp (UNIX-epoch nanoseconds), refreshed once per loop iteration; for a live value use `qb::unix_nanos(qb::wall_now())`.

**Event subscription**

| Member | Signature |
|---|---|
| `registerEvent` | `template<class _Actor> void registerEvent(_Actor& actor) const noexcept` |
| `unregisterEvent` | `template<class _Actor> void unregisterEvent(_Actor& actor) const noexcept` |
| `unregisterEvent` | `template<class _Event> void unregisterEvent() const noexcept` |

Handlers are public member functions named `on`: `void on(const EventType&)` for a read-only handler,
`void on(EventType&)` when the handler may `reply` or `forward` the event.

**Sending events**

| Member | Signature | Delivery |
|---|---|---|
| `push` | `template<class _Event, class... _Args> _Event& push(ActorId const& dest, _Args&&... args) const noexcept` | Ordered; returns a mutable reference to the queued event; supports non-trivial members. |
| `send` | `template<class _Event, class... _Args> void send(ActorId const& dest, _Args&&... args) const noexcept` | Unordered; the event type must be trivially destructible. |
| `broadcast` | `template<class _Event, class... _Args> void broadcast(_Args&&... args) const noexcept` | To every actor on every core. |
| `reply` | `void reply(Event& event) const noexcept` | Returns a received event to its source by swapping destination and source. |
| `forward` | `void forward(ActorId dest, Event& event) const noexcept` | Re-routes a received event to `dest`, preserving its original source. |
| `to` | `EventBuilder to(ActorId dest) const noexcept` | Fluent helper that chains ordered `push<>()` over one destination. |
| `getPipe` | `Pipe getPipe(ActorId dest) const noexcept` | Direct access to the source-to-destination channel, e.g. for `allocated_push<Event>(size, ...)`. |

`push<>()` is the primary send. `reply` and `forward` reuse the received event object, so the event must
not be touched after either call. See [Messaging](../4_qb_core/messaging.md).

**Referenced actors and services**

| Member | Signature | Notes |
|---|---|---|
| `addRefActor` | `template<class _Actor, class... _Args> _Actor* addRefActor(_Args&&... args) const` | Creates a child actor on the same core; the parent holds a raw, non-owning pointer. |
| `addRefHandle` | `template<class _Actor, class... _Args> RefActorHandle<_Actor> addRefHandle(_Args&&... args) const` | Liveness-checked wrapper around `addRefActor`. |
| `getService` | `template<class _ServiceActor> _ServiceActor* getService() const noexcept` | The `ServiceActor` instance on this core, or `nullptr`. |
| `getServiceId` | `template<class T> static ActorId getServiceId(CoreId index) noexcept` | The `ActorId` of service `T` on core `index`. |

**Discovery and periodic work**

| Member | Signature | Notes |
|---|---|---|
| `require` | `template<class... _Actors> bool require() const noexcept` | Broadcasts a discovery ping; live actors answer with `RequireEvent`. |
| `is` | `template<class _Type> bool is(RequireEvent const&) const noexcept` (and a `uint32_t` overload) | Tests whether a `RequireEvent` (or type id) pertains to `_Type`. |
| `registerCallback` | `template<class _Actor> void registerCallback(_Actor& actor) const noexcept` | Enrolls an actor that also inherits `ICallback`. |
| `unregisterCallback` | `template<class _Actor> void unregisterCallback(_Actor& actor) const noexcept` | |
| `spawn` | `template<class Func> void spawn(Func&& func) const` | **Recommended.** Launches a *scoped* coroutine cancelled when the actor is killed; communicates back via [`ScopedCoroContext`](#coroutines) (cancellation-aware ops + `ask()`). |
| `spawn_detached` | `template<class Func> void spawn_detached(Func&& func) const` | Launches a *detached* coroutine that outlives the actor; communicates back via [`CoroContext`](#coroutines). |
| `resolve_ask` | `template<class E> bool resolve_ask(E& e) const noexcept` | Routes an `AskEvent`-derived reply to the awaiting `spawn` coroutine; call first in the asker's `on(E&)`. |
| `has_active_coroutines` | `bool has_active_coroutines() const` | |

`reply` and `forward` reuse the received event object, so the event must not be touched afterward. See
[Patterns](../4_qb_core/patterns.md) for `require`, services, and referenced actors.

### `qb::ServiceActor<Tag>`

- **Header:** `qb/core/Actor.h`
- **Role:** a per-core singleton actor. Each `Tag` (an empty struct) yields one instance per `VirtualCore`.
  Create it with `Main::addActor` and reach it with `Actor::getService<T>()` or
  `getServiceId<Tag>(core)`.

```cpp
struct CacheTag {};
class CacheService : public qb::ServiceActor<CacheTag> { /* ... */ };
```

### `qb::Event`

- **Header:** `qb/core/Event.h` (include `qb/event.h`)
- **Role:** the cache-line-aligned base for every message. Derive from it and add data members for the
  payload.

| Accessor | Signature |
|---|---|
| `getSource` | `ActorId getSource() const noexcept` |
| `getDestination` | `ActorId getDestination() const noexcept` |
| `getID` | `id_type getID() const noexcept` |
| `getQOS` | `uint8_t getQOS() const noexcept` |
| `getSize` | `std::size_t getSize() const noexcept` |
| `is_alive` | `bool is_alive() const noexcept` |

`Event::id_type` is `qb::EventId` in release builds (`NDEBUG`) and `const char *` in debug builds, so
route on the event's runtime type rather than storing a raw `getID()` value across build configurations.

Quality-of-service variants are `EventQOS2`/`EventQOS1` (both aliases of `Event`, higher value processed
first) and `EventQOS0`, a distinct lowest-priority, unordered event. System events include `KillEvent`,
`SignalEvent`, `PingEvent`, `RequireEvent`, and `UnregisterCallbackEvent`. For payloads, prefer
[`qb::string<N>`](#strings-and-containers) for inline strings and a smart pointer for large or
dynamically sized data. See [The event system](../2_core_concepts/event_system.md).

### `qb::ICallback`

- **Header:** `qb/core/ICallback.h` (include `qb/icallback.h`)
- **Role:** a mixin that grants an actor an `onCallback()` tick once per `VirtualCore` loop iteration.
  Multiply-inherit it alongside `qb::Actor` and enroll with `registerCallback(*this)`.

```cpp
// src: include/qb/core/ICallback.h (HeartbeatActor pattern)
class PollerActor : public qb::Actor, public qb::ICallback {
public:
    bool onInit() final { registerCallback(*this); return true; }
    void onCallback() final { /* runs every loop iteration; must not block */ }
};
```

`onCallback()` runs on the event-loop thread, so blocking work inside it stalls the whole core.

### Identifiers

- **Header:** `qb/core/ActorId.h` (include `qb/actorid.h`)

| Type | Definition | Notes |
|---|---|---|
| `qb::CoreId` | `uint16_t` | Logical core id. `qb::MaxCores == 256`. |
| `qb::ServiceId` | `uint16_t` | Actor slot within a core. |
| `qb::TypeId` / `qb::EventId` | `uint16_t` | Dense per-type identifier. |
| `qb::ActorId` | compound `{ServiceId, CoreId}` | `sid()`, `index()`, `is_valid()`, `is_broadcast()`, `operator uint32_t()`. `ActorId::NotFound == 0`. |
| `qb::BroadcastId` | `ActorId` subclass | `explicit BroadcastId(uint32_t core_id)` targets every actor on one core. |
| `qb::CoreIdSet` | alias of `CoreIdBitSet` | Affinity and reachability set over `[0, MaxCores)`. |
| `qb::NoAffinity` | `constexpr CoreId` sentinel | `std::numeric_limits<CoreId>::max()` — let the OS schedule the thread. |

See [The actor model](../2_core_concepts/actor_model.md) and [Core invariants](./core_invariants.md).

---

## `qb-io`: the asynchronous runtime

The non-blocking I/O foundation: a libev-based event loop, transports, coroutines, protocols, crypto,
and utilities. See [qb-io](../3_qb_io/) for the full treatment.

### The `use<>` helper

- **Header:** `qb/io/async.h`
- **Role:** a CRTP helper that exposes ready-made async bases as nested type aliases, so a session or
  client class declares its transport and protocol by inheritance.

```cpp
// src: examples/io (use<>::tcp pattern)
template <typename _Derived> struct qb::io::use {
    template <typename _Protocol = void> using io = async::io<_Derived>;
    struct tcp {
        using acceptor = /* ... */;
        template <typename _Client> using server     = /* ... */;
        template <typename _Server = void> using client = /* ... */;
        struct ssl { /* secure variants of the above */ };
    };
    struct udp { using server = /* ... */; using client = /* ... */; };
    using timeout = async::with_timeout<_Derived>;
    using file    = async::file<_Derived>;
};
```

See [Transports](../3_qb_io/transports.md) and [Network actors](../5_core_io_integration/network_actors.md).

### Event loop

- **Header:** `qb/io/async/listener.h`

`qb::io::async::listener` manages the thread-local libev loop, reached through the thread-local
`listener::current`.

| Member | Signature |
|---|---|
| `registerEvent` | `template<class _Actor, class... _Args> auto& registerEvent(_Actor& actor, _Args&&... args)` |
| `unregisterEvent` | `void unregisterEvent(IRegisteredKernelEvent* kevent)` |
| `run` | `void run(int flag = 0)` |
| `break_one` | `void break_one()` |
| `loop` | `ev::loop_ref loop() const` |
| `coro_scheduler` | `CoroutineScheduler& coro_scheduler()` |

Free functions in `qb::io::async` (same header) drive the loop for standalone use:

| Function | Signature | Effect |
|---|---|---|
| `init` | `void init() noexcept` | No-op; `listener::current` self-initializes per thread. |
| `run` | `std::size_t run(int flag = 0)` | Runs the loop with a libev flag. |
| `run_once` | `std::size_t run_once()` | One `EVRUN_ONCE` iteration. |
| `run_until` | `std::size_t run_until(bool const& status)` | Pumps `EVRUN_NOWAIT` while `status` is true. |
| `break_parent` | `void break_parent() noexcept` | Breaks the current thread's loop. |

`qb::Main` drives the loop on each `VirtualCore` automatically; the free functions are for standalone
`qb-io` use. See [The async system](../3_qb_io/async_system.md).

### Scheduled callbacks and timeouts

- **Header:** `qb/io/async/io.h`

| API | Signature | Notes |
|---|---|---|
| `callback` | `template<class _Func> void callback(_Func&& func)` | Invokes `func()` immediately, in-line. |
| `callback` | `template<class _Func, class Rep, class Period> void callback(_Func&& func, std::chrono::duration<Rep, Period> timeout)` | Schedules `func` on a one-shot timer; runs after `timeout`. When `timeout <= 0` it runs `func()` inline immediately (not on the next loop turn). |
| `scoped_callback` | `template<class _Func> auto scoped_callback(_Func&& func)` | Returns a cancellable `unique_ptr<ScopedTimeout<...>>`. |
| `with_timeout<_Derived>` | CRTP base | Derived implements `on(qb::io::async::event::timer const&)`; reset with `updateTimeout()`. |

`with_timeout::setTimeout` takes a `qb::duration`:

```cpp
class Idle : public qb::io::async::with_timeout<Idle> {
public:
    Idle() : with_timeout(std::chrono::seconds(30)) {}
    void on(qb::io::async::event::timer const&) { /* inactivity elapsed */ }
};
```

### Async event types

- **Header:** `qb/io/async/event/all.h` (one struct per header in `qb/io/async/event/`)

`qb::io::async::event::io`, `::timer`, `::signal`, `::file`, `::disconnected`, `::pending_read`,
`::pending_write`, `::eos`, `::dispose`, `::extracted`, and `::handshake`. A component receives these
through matching `on(event::T&)` handlers. The end-of-input event is `event::input_drained` (aliased
`event::eof`).

### Sockets and addresses

- **Header:** `qb/io/system/sys__socket.h`

`qb::io::socket` is the cross-platform low-level wrapper: `bind`, `listen`, `accept`, `connect`,
`connect_n` (non-blocking), `send`/`recv`, `sendto`/`recvfrom`, `close`, `set_nonblocking`, `local_endpoint`,
`peer_endpoint`, and the static `resolve`. `qb::io::endpoint` represents an IPv4, IPv6, or AF_UNIX
address (`af()`, `ip()`, `port()`, `to_string()`, `as_in()`, `as_un()`).

- **Header:** `qb/io/uri.h`

`qb::io::uri` parses RFC 3986 URIs: `scheme()`, `host()`, `port()`, `u_port()`, `path()`, `query("key")`,
`queries()`, `fragment()`, plus static `parse`, `encode`, and `decode`.

#### TCP, UDP, and TLS

| Type | Header | Key methods |
|---|---|---|
| `qb::io::tcp::socket` | `qb/io/tcp/socket.h` | `init(af)`, `connect(endpoint\|uri[, qb::duration])`, `n_connect(...)`, `read`, `write`, `disconnect` |
| `qb::io::tcp::listener` | `qb/io/tcp/listener.h` | `listen(endpoint\|uri)`, `accept() -> tcp::socket`, `disconnect` |
| `qb::io::udp::socket` | `qb/io/udp/socket.h` | `init(af)`, `bind(endpoint\|uri)`, `read`, `write`, multicast and broadcast options |
| `qb::io::tcp::ssl::socket` | `qb/io/tcp/ssl/socket.h` | `init(SSL* = nullptr)`, `connect`, `n_connect`, `handshake_status`, `read`, `write`, `disconnect` |
| `qb::io::tcp::ssl::listener` | `qb/io/tcp/ssl/listener.h` | `init(SSL_CTX*)`, `accept() -> ssl::socket`, `ssl_handle` |

TLS types require `QB_WITH_SSL` (the default when OpenSSL is present). Context helpers
`qb::io::ssl::create_client_context` and `create_server_context` live in `qb/io/tcp/ssl/socket.h`. A
client context loads the system trust store and verifies the peer by default; `set_insecure()` opts out.
Every socket and transport exposes a `constexpr static bool is_secure()`. See
[SSL transport](../3_qb_io/ssl_transport.md) and [Transports](../3_qb_io/transports.md).

### Streams, transports, and protocols

- **Streams** (`qb/io/stream.h`): `qb::io::istream<_IO_>`, `ostream<_IO_>`, and `stream<_IO_>` add input
  and output buffering over a transport `_IO_`. Key members: `transport()`, `in()`, `out()`, `read`,
  `write`, `publish`. A read or write that would exceed the configured buffer limit returns
  `ErrBufferLimitExceeded` (`-2`).
- **Transports** (`qb/io/transport/`): concrete specializations — `transport::tcp` is `stream<tcp::socket>`;
  likewise `transport::udp`, `transport::stcp` (TLS), `transport::file`, and the acceptors `transport::accept`
  and `transport::saccept`.
- **Protocol interface** (`qb/io/async/protocol.h`): `qb::io::async::AProtocol<_IO_>` is the CRTP base for
  message framing. Override `std::size_t getMessageSize() noexcept`, `void onMessage(std::size_t) noexcept`,
  and `void reset() noexcept`, and define `using message = ...;`. `not_ok()` signals an unrecoverable stream
  so the component closes the connection.
- **Built-in protocols** (`qb/io/protocol/`): in `qb::protocol::text` — `command` (newline-terminated),
  `string` (NUL-terminated), `command_view`/`string_view`, and `binary8`/`binary16`/`binary32`
  (size-prefixed); in `qb::protocol` — `json` and `json_packed`. In `qb::io::protocol`, `accept` hands an
  accepted socket to the I/O component and `handshake` drives a TLS or transport handshake to completion.

See [Protocols](../3_qb_io/protocols.md).

### Coroutines

- **Headers:** `qb/io/async/coroutine.h` and the files under `qb/io/async/coroutine/`

`qb::io::async::task<T>` is the move-only return type for a coroutine that uses `co_await`/`co_return`.
Entry points in `qb::io::async`:

| API | Signature | Header |
|---|---|---|
| `sleep` | `timer_awaiter sleep(qb::duration duration)` | `coroutine/utils.h` |
| `wait_readable` / `wait_writable` | `socket_awaiter wait_readable(int fd)` (and `uintptr_t`) | `coroutine/utils.h` |
| `coro_scheduler` | `CoroutineScheduler& coro_scheduler()` | `coroutine/utils.h` |
| `run_for` | `void run_for(qb::duration duration)` | `coroutine/utils.h` |

`CoroutineScheduler::spawn(std::move(task))` runs a coroutine; the scheduler owns the frame. The wider
surface includes combinators (`when_all`, `when_any`, `race`), channels (`channel<T>`), cooperative sync
(`async_mutex`, `semaphore`, `barrier`), retry, structured-concurrency scopes, generators, and
`shared_task<T>`. Within an actor, launch a coroutine through `Actor::spawn` (recommended — scoped, cancelled on kill) or
`Actor::spawn_detached` (detached) and return results via `qb::CoroContext` (which holds the actor's
`ActorId` by value, not a pointer to the actor). See
[Coroutines](../3_qb_io/coroutines.md) and [Async in actors](../5_core_io_integration/async_in_actors.md).

### Cryptography, JWT, and compression

| Area | Header | Surface |
|---|---|---|
| `qb::crypto` | `qb/io/crypto.h` | Hashing (SHA-256/512, …), HMAC, AEAD encryption, key derivation (Argon2, HKDF, PBKDF2), ECIES, envelope encryption, signatures (RSA, EC, Ed25519), random generation, Base64. Requires OpenSSL. |
| `qb::jwt` | `qb/io/crypto_jwt.h` | RFC 7519 JSON Web Tokens: `create`, `verify`, `decode` for HMAC, RSA, and ECDSA algorithms. |
| `qb::compression` | `qb/io/compression.h` | Gzip and deflate via `compress`/`uncompress`; convenience namespaces `qb::gzip` and `qb::deflate`. Requires zlib. The decompressor enforces a max-output budget against decompression bombs. |

See [Utilities](../3_qb_io/utilities.md).

### File system

- `qb::io::sys::file` (`qb/io/system/file.h`): synchronous native file descriptor wrapper (`open`, `read`,
  `write`, `close`), plus `sys::file_to_pipe` and `sys::pipe_to_file` for bulk transfer to and from a
  `qb::allocator::pipe`.
- `qb::io::async::file_watcher` / `directory_watcher` (`qb/io/async/io.h`): CRTP bases that monitor file or
  directory changes and deliver `on(event::file&)`.

---

## Shared utilities

Used by both libraries.

### Time

- **Header:** `qb/system/timestamp.h`

| Type | Definition |
|---|---|
| `qb::duration` | `std::chrono::nanoseconds` |
| `qb::mono_time` | `std::chrono::steady_clock::time_point` |
| `qb::wall_time` | `std::chrono::system_clock::time_point` |

Helpers: `qb::mono_now()`, `qb::wall_now()`, `qb::unix_seconds()`/`unix_millis()`, `qb::to_iso8601()`,
`qb::from_iso8601()`, `qb::format_utc()`, `qb::parse_utc()`, and the `qb::time_literals` namespace. Use
`mono_time` for deadlines and timers; use `wall_time` for dates, expiry, and wire formats.

### Strings and containers

| Type | Header | Notes |
|---|---|---|
| `qb::string<N>` | `qb/string.h` | Fixed-capacity, heap-free string; truncates past `N`. |
| `qb::allocator::pipe<T>` | `qb/system/allocator/pipe.h` | Growable front-and-back buffer; backs I/O buffers. `pipe<char>` is the byte specialization. |
| `qb::unordered_map` / `unordered_set` | `qb/system/container/unordered_map.h`, `unordered_set.h` | Flat hash tables (ska); `icase_unordered_map` is the case-insensitive variant. |
| `qb::ring_buffer<T, N, Overwrite>` | `qb/system/container/ring_buffer.h` | Fixed-capacity circular FIFO. |
| `qb::json` / `qb::jsonb` | `qb/json.h` | nlohmann/json aliases; `jsonb` is a distinct wrapper type. |
| `qb::uuid` | `qb/uuid.h` | RFC 4122 UUID; `qb::generate_random_uuid()` for v4. |

### Lock-free primitives and system info

- **Lock-free** (`qb/system/lockfree/`): `qb::lockfree::SpinLock` (TTAS), `spsc::ringbuffer`,
  `mpsc::ringbuffer`, and `mpsc_unbounded_queue`. These are correctness-critical concurrency primitives;
  see [Lock-free primitives](./lockfree_primitives.md).
- **System** : `qb::CPU` (`qb/system/cpu.h`) for core counts and affinity; `qb::endian` (`qb/system/endian.h`)
  for byte-order detection and `byteswap`.

### Logging

- **Header:** `qb/io.h`

`qb::io::cout()` and `qb::io::cerr()` are thread-safe stream objects; `LOG_DEBUG`/`LOG_INFO`/`LOG_WARN`/
`LOG_CRIT` wrap them. Logging support is gated by `QB_WITH_LOGGING` (on by default).

---

## See also

- [qb-core](../4_qb_core/) — the actor engine in depth
- [qb-io](../3_qb_io/) — the asynchronous runtime in depth
- [Core invariants](./core_invariants.md) and [qb-io invariants](./io_invariants.md) — guarantees the API relies on
- [Glossary](./glossary.md) — definitions of the terms used above
- [FAQ](./faq.md) — common questions
