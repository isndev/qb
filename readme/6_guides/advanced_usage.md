# Advanced usage

> **Audience:** Adopter · **Status:** stable · **Verified-against:** qb 2.0.0 (c++23)

Five techniques for non-trivial systems: defining a custom wire protocol, scaling actors across cores, running coroutines safely inside actors, structuring shared logic as service actors, and composing the qbm modules with your own actors.

**Prerequisites:** [Writing actors with `qb::Actor`](../4_qb_core/actor.md), [The engine: `qb::Main` and `VirtualCore`](../4_qb_core/engine.md), [Building network actors](../5_core_io_integration/network_actors.md) — **See also:** [Actor patterns](../4_qb_core/patterns.md), [C++20 coroutines](../3_qb_io/coroutines.md), [Framing messages with protocols](../3_qb_io/protocols.md), [Performance tuning](./performance_tuning.md)

## Summary

Each technique below builds on a primitive that another page owns. This guide does not re-teach those primitives; it shows how to combine them in production-shaped designs and points to the page that defines each one. Every code sample is grounded in a header under `qb/include` or an example under `examples/`, cited with a `<!-- src: -->` comment. Where the framework has a sharp edge — a `noexcept` boundary that calls `std::terminate`, a pointer that dangles after `kill()`, a coroutine that must not touch actor state — the pitfall is called out inline rather than left for you to discover at runtime.

The five sections are independent. Read the one you need.

## Custom wire protocols

When an actor speaks a binary or text format that none of the built-in protocols frame, you implement the framing yourself. A protocol is a class deriving from `qb::io::async::AProtocol<IO_>` that decides where one message ends and hands each complete message to its I/O component. The base contract — `getMessageSize()`, `onMessage(std::size_t)`, `reset()`, and the `_io` reference — is owned by [Framing messages with protocols](../3_qb_io/protocols.md); this section shows the end-to-end shape of a custom binary protocol with a header, including the serialization half that the protocols page does not cover.

### The two halves: framing and serialization

A custom protocol has two independent pieces:

- **Framing (inbound).** A subclass of `AProtocol<IO_>` that scans the input buffer (`this->_io.in()`), returns the size of the next complete message from `getMessageSize()` (or `0` when more bytes are needed), and on a complete message reconstructs the application type and dispatches it via `this->_io.on(parsed_message)`.
- **Serialization (outbound).** A specialization of `qb::allocator::pipe<char>::put<T>` that writes your application type `T` into the output buffer in the same wire format. This is what makes `*this << my_message;` produce the right bytes.

The example below frames an 8-byte header (magic, version, type, payload length) followed by a variable-length payload.

```cpp
// <!-- src: examples/core_io/chat_tcp/shared/Protocol.h -->
#include <qb/io/async.h>
#include <qb/system/allocator/pipe.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace chat {

struct MessageHeader {        // 8 bytes on the wire
    uint16_t magic;           // 'QC' (0x5143)
    uint8_t  version;         // protocol version
    uint8_t  type;            // MessageType
    uint32_t length;          // payload size in bytes
};

constexpr uint16_t PROTOCOL_MAGIC   = 0x5143;
constexpr uint8_t  PROTOCOL_VERSION = 0x01;

enum class MessageType : uint8_t { AUTH_REQUEST = 1, CHAT_MESSAGE, USER_LIST };

struct Message {              // the application-level type
    MessageType type{};
    std::string payload;
};

// Inbound framing: subclass AProtocol<IO_>.
template <typename IO_>
class ChatProtocol : public qb::io::async::AProtocol<IO_> {
    static constexpr std::size_t HEADER_SIZE = sizeof(MessageHeader);
    bool              _reading_header = true;
    MessageHeader     _header{};
    std::vector<char> _payload;

public:
    using message = Message;  // type alias the I/O component reads back

    explicit ChatProtocol(IO_ &io) noexcept
        : qb::io::async::AProtocol<IO_>(io) {}

    // Return the size of the next complete message, or 0 if more data is needed.
    std::size_t getMessageSize() noexcept override {
        auto &buffer = this->_io.in();
        if (buffer.empty())
            return 0;

        if (_reading_header) {
            if (buffer.size() < HEADER_SIZE)
                return 0;                       // header not fully buffered yet
            std::memcpy(&_header, buffer.cbegin(), HEADER_SIZE);
            if (_header.magic != PROTOCOL_MAGIC || _header.version != PROTOCOL_VERSION) {
                reset();                        // unrecognized stream; resynchronize
                return 0;
            }
            _reading_header = false;
            _payload.resize(_header.length);
        }
        return HEADER_SIZE + _header.length;    // wait until header + payload buffered
    }

    // Called once a full message is buffered; reconstruct and dispatch it.
    void onMessage(std::size_t) noexcept override {
        auto &buffer = this->_io.in();
        Message msg;
        msg.type = static_cast<MessageType>(_header.type);
        if (_header.length > 0)
            msg.payload.assign(buffer.cbegin() + HEADER_SIZE, _header.length);
        this->_io.on(msg);                      // hand the message to the I/O component
        _reading_header = true;                 // ready for the next frame
    }

    void reset() noexcept override {
        _reading_header = true;
        _payload.clear();
    }
};

} // namespace chat
```

The outbound half is a single specialization in a `.cpp`:

```cpp
// <!-- src: examples/core_io/chat_tcp/shared/Protocol.cpp -->
namespace qb::allocator {

template <>
pipe<char> &pipe<char>::put<chat::Message>(const chat::Message &msg) {
    chat::MessageHeader header{
        chat::PROTOCOL_MAGIC,
        chat::PROTOCOL_VERSION,
        static_cast<uint8_t>(msg.type),
        static_cast<uint32_t>(msg.payload.size())};

    this->put(reinterpret_cast<const char *>(&header), sizeof(header));
    if (!msg.payload.empty())
        this->put(msg.payload.data(), msg.payload.size());
    return *this;
}

} // namespace qb::allocator
```

### Attaching the protocol to a session

A network actor (or a session it owns) selects its protocol with `switch_protocol<Protocol>(*this)`, declared in `qb/io/async/io.h`. The session declares the protocol type and activates it once, typically in its constructor or `on(event::connected)`:

```cpp
// <!-- src: examples/core_io/chat_tcp/server/ChatSession.h -->
class ChatSession : public qb::io::use<ChatSession>::tcp::client<ServerActor> {
public:
    using Protocol = chat::ChatProtocol<ChatSession>;

    explicit ChatSession(ServerActor &server)
        : qb::io::use<ChatSession>::tcp::client<ServerActor>(server) {
        this->template switch_protocol<Protocol>(*this);   // activate the framer
    }

    // Parsed messages arrive here, in the actor's single-threaded context.
    void on(const chat::Message &msg) { /* dispatch by msg.type */ }
};
```

Once attached, the parsed `chat::Message` arrives at `on(const chat::Message&)`, and sending is `*this << chat::Message{...}`. The `use<>` mixin that turns the class into a TCP endpoint is owned by [Building network actors](../5_core_io_integration/network_actors.md).

**Pitfalls.**

- `getMessageSize()` must be a pure query that returns the size *including* any delimiter (or `0`); the base text protocols keep a resumable scan offset so re-invocation does not rescan from the start. The terminator-based templates in `qb/io/protocol/base.h` provide `shiftSize()` to convert that total back to the payload length.
- `onMessage()` may call `switch_protocol()` or `clear_protocols()` (for example, an HTTP-to-WebSocket upgrade). The I/O component snapshots the old protocol's `should_flush()` policy *before* invoking `onMessage()`, because the old protocol may be deleted during the call. If you switch protocols inside `onMessage()`, do not touch `this->_io.in()` afterward through the old protocol's state.
- On an unrecoverable framing error, signal `this->not_ok()` (inherited from `AProtocol`) so the I/O component closes the connection; returning `0` forever silently stalls the stream instead.

## Scaling actors across cores

`qb::Main` launches one `VirtualCore` worker thread per configured core, and an actor is pinned to the core it was created on for its whole life — it never migrates. You place actors on cores explicitly; the engine moves messages between cores over lock-free queues. The engine API is owned by [The engine: `qb::Main` and `VirtualCore`](../4_qb_core/engine.md); this section covers the placement and distribution patterns.

### Placement

Place an actor on a specific core with `Main::addActor<T>(core_id, args...)`, or configure a core through `Main::core(core_id)` and use its `addActor`/`builder()` API. All placement happens *before* `start()`.

```cpp
// <!-- src: examples/core/example3_multicore.cpp -->
#include <qb/actor.h>
#include <qb/main.h>
#include <thread>

int main() {
    qb::Main engine;

    const auto n_cores = std::thread::hardware_concurrency();

    // One DispatcherActor on core 0; a WorkerActor on every other core.
    auto dispatcher = engine.addActor<DispatcherActor>(0);
    for (qb::CoreId core = 1; core < n_cores; ++core)
        engine.addActor<WorkerActor>(core, dispatcher);

    engine.start();   // spawns one worker thread per used core
    engine.join();
    return 0;
}
```

To add several actors to one core fluently, use the builder:

```cpp
// <!-- src: qb/include/qb/core/Main.h (CoreInitializer::builder) -->
engine.core(0)
    .builder()
    .addActor<Logger>()
    .addActor<MetricsCollector>()
    .addActor<HealthMonitor>();
```

### Pinning and idle latency

Two per-core knobs affect placement performance, both set on the `CoreInitializer` returned by `Main::core(core_id)` before `start()`:

- `setAffinity(CoreIdSet)` pins the worker thread to one or more physical CPU cores so the OS scheduler does not migrate it. `qb::NoAffinity` opts out.
- `setLatency(qb::duration)` sets how long an idle core waits before re-checking its mailbox. `qb::duration::zero()` (the default) is the busy, lowest-latency mode; a non-zero value trades latency for lower CPU usage when the core is idle. `Main::setLatency(...)` sets a default for every core that has no explicit value.

```cpp
// <!-- src: qb/include/qb/core/Main.h (CoreInitializer::setAffinity / setLatency) -->
using namespace std::chrono_literals;

engine.core(0).setLatency(qb::duration::zero());   // hot path: no idle wait
engine.core(1).setLatency(100us);                  // background work: yield when idle
engine.core(1).setAffinity(qb::CoreIdSet{2});      // pin core-1 worker to CPU 2
```

`setLatency` takes a `qb::duration` (a `std::chrono::nanoseconds` span); pass any `std::chrono` duration. See [time utilities](../3_qb_io/utilities.md) for the canonical time vocabulary.

### Distribution patterns

- **Round-robin dispatch.** A dispatcher actor on one core holds the `ActorId`s of workers on other cores and sends each task to the next worker in rotation — a rotating index plus `push<Event>(workers[index], args...)`. This is the pattern in `example3_multicore.cpp`, where a `qb::ICallback` callback generates work and round-robins it across the worker vector.
- **Zero-copy re-dispatch.** When a dispatcher receives an event and wants to hand it on without rebuilding it, `forward(dest, event)` reuses the received event object. The handler must take the event by non-const reference, because `forward()` consumes it.
- **Broadcast.** `broadcast<Event>(args...)` sends an event to every actor on every core; `push<Event>(qb::BroadcastId(core_id), args...)` targets every actor on a single core. Broadcast events cannot be `reply()`-ed to or `forward()`-ed.
- **Runtime discovery.** Instead of hard-coding worker `ActorId`s, an actor can call `require<WorkerA, WorkerB>()`; live actors of those types answer with a `RequireEvent` carrying their `ActorStatus`. The requester must `registerEvent<qb::RequireEvent>(*this)` and identify responses with `is<WorkerA>(event)`. This decouples placement from wiring and is detailed in [Actor patterns](../4_qb_core/patterns.md).

**Pitfalls.**

- Actors must be constructed from inside a `VirtualCore` worker thread, not from the main thread or an arbitrary user thread. Use `Main::addActor<T>(...)` (before `start()`) or `addRefActor<T>()` (from within a handler); both satisfy this. Constructing an actor directly is a debug-assert failure.
- `push`, `send`, and `broadcast` are `noexcept`. If growing the pipe buffer or running an event constructor throws across that boundary, the process calls `std::terminate()`. Keep event constructors small and allocation-light. This is intentional, not a bug to work around.
- `kill()` only flags the actor (`_alive = false`); it may still drain already-queued events, and `~Actor()` runs later under `VirtualCore` control. Do not assume teardown is synchronous with the `kill()` call.

## Coroutine patterns inside actors

`spawn_async()` is the only supported way to run a C++20 coroutine inside an actor. It launches the coroutine in an *isolated context* on the actor's own `VirtualCore` and returns immediately; the actor keeps processing other events while the coroutine is suspended at a `co_await`. The coroutine layer itself — `task<T>`, awaiters, `sleep`, channels, scopes — is owned by [C++20 coroutines](../3_qb_io/coroutines.md). This section covers the actor-safety contract, which is specific to `spawn_async`.

### The isolation rule

A spawned coroutine **must not touch actor member variables after any `co_await`.** The actor may be destroyed while the coroutine is suspended; dereferencing it then is undefined behavior. The rule in practice:

1. Capture everything you need **by value** before the first `co_await`. Never capture `this` or capture by reference.
2. After suspension, communicate back to the actor **only** through the `CoroContext` (`ctx`) the coroutine receives: `ctx.push<Event>(...)` (to self), `ctx.push_to<Event>(dest, ...)`, `ctx.id()`, and `ctx.time()`. Events sent to a dead actor are dropped, so this is always safe.

```cpp
// <!-- src: examples/coroutine/actor_example.cpp -->
#include <qb/actor.h>
#include <qb/io/async/coroutine.h>

void on(StartProcessing &req) {
    // Snapshot everything BEFORE the coroutine — capture by value only.
    int         req_id = req.request_id;
    std::string data   = req.data;
    uint64_t    start  = time();           // VirtualCore time, nanoseconds
    qb::ActorId sender = req.getSource();

    spawn_async([req_id, data, start, sender](auto ctx)
                    -> qb::io::async::task<void> {
        // Isolated context: NO access to actor members here.
        std::string result = co_await AsyncService::process_data(data);
        uint64_t    elapsed = ctx.time() - start;

        // Only way back into the actor: an event via ctx.
        ctx.template push<ProcessingComplete>(req_id, result, elapsed);
    });
    // Returns immediately; the result lands later in on(ProcessingComplete&).
}
```

The result arrives as an ordinary event handler — `on(ProcessingComplete&)` — running back in the safe, single-threaded actor context with exclusive access to the actor's state.

### Lifecycle

- `spawn_async()` must be called from the actor's `VirtualCore` worker thread; it debug-asserts that a coroutine scheduler exists on the calling thread.
- Each call creates two coroutine frames by design (a lifetime-tracking wrapper plus your body). The wrapper increments a `shared_ptr<atomic>` counter that outlives the actor, so a suspended coroutine cannot use-after-free its owner.
- `has_active_coroutines()` reports whether any spawned coroutine is still in flight — useful when deciding whether it is safe to `kill()`.

**Pitfalls.**

- Capturing `this` or any reference into the spawned lambda is a dangling-pointer / data-race bug that sanitizers may not catch, because the corruption happens only when the actor dies mid-suspension. Capture by value.
- Never call a blocking `run_sync()` on a coroutine from inside an actor handler — it blocks the entire `VirtualCore` thread, stalling every actor on that core. Drive coroutines through `spawn_async` only.

## Service actors for shared logic

A `qb::ServiceActor<Tag>` is a singleton-style actor: exactly one instance per `VirtualCore` per `Tag`. Other actors on the same core reach it directly with `getService<T>()`, which returns a raw pointer (or `nullptr` if no such service exists on that core) — no `ActorId` lookup, no event round-trip. This is the idiomatic home for per-core shared infrastructure: a connection pool, a config cache, a metrics sink, a logger.

```cpp
// <!-- src: qb/source/core/tests/system/test-actor-add.cpp -->
#include <qb/actor.h>

struct ConfigTag {};   // unique tag identifies the service type

class ConfigService : public qb::ServiceActor<ConfigTag> {
    qb::unordered_map<std::string, std::string> _settings;
public:
    bool onInit() override { return true; }
    std::string get(const std::string &key) const {
        auto it = _settings.find(key);
        return it != _settings.end() ? it->second : std::string{};
    }
};

class WorkerActor : public qb::Actor {
public:
    bool onInit() override {
        // Direct, same-core access — no event needed.
        if (auto *cfg = getService<ConfigService>())
            _timeout = cfg->get("timeout");
        return true;
    }
private:
    std::string _timeout;
};
```

Register the service on each core before the workers that depend on it (placement order within a core is creation order):

```cpp
// <!-- src: qb/include/qb/core/Main.h (CoreInitializer::builder) -->
engine.core(0)
    .builder()
    .addActor<ConfigService>()   // service first
    .addActor<WorkerActor>()     // then dependents that call getService<ConfigService>()
    .addActor<WorkerActor>();
```

**Properties and pitfalls.**

- One instance **per core**: `getService<T>()` only resolves a service that exists on the *calling* actor's core. For cross-core access, send an event to the service's `ActorId` instead of holding a pointer. The returned pointer must never be cached and dereferenced from another core's thread.
- A `ServiceActor`'s `ServiceIndex` is allocated once per `Tag` at static initialization and reserved for the process lifetime; the id is never recycled, so service ids stay stable even as ordinary actors come and go.
- The service is still an actor: it has a mailbox and can `registerEvent`/`on(...)` like any other actor. `getService<T>()` is a fast path for same-core callers, not a replacement for the event system across cores.

## Composing qbm modules

The qbm modules — qbm-http (HTTP/1.1 always; HTTP/2, WebSocket, and JWT on SSL-enabled builds; optional HTTP/3 when QUIC and nghttp3 are present), qbm-pgsql (PostgreSQL), and qbm-redis (Redis) — are application protocols built on the same `qb-io` foundation your actors use. WebSocket is a capability of qbm-http (`qb::http::ws`, under `<http/ws.h>`), not a separate module. Composing a module means treating its client as ordinary asynchronous work owned by an actor: the actor issues a request, the result returns as a callback or coroutine resumption on the actor's own `VirtualCore`, and the actor forwards it onward as an event. Because the module client lives on the actor's core, it inherits the same single-threaded, no-locks discipline as everything else on that core.

### Build-time composition

Modules are separate repositories added as git submodules under `qbm/` and discovered by CMake. The repo root calls `qb_load_modules("${CMAKE_CURRENT_SOURCE_DIR}/qbm")`, which `add_subdirectory()`s every subdirectory that contains a `CMakeLists.txt` and exposes a `qbm::<name>` alias target for each registered module (`http`, `pgsql`, `redis`). Link the module you need:

```cmake
# <!-- src: qbm/http/README.md (target_link_libraries ... qbm::http); qb-dev/CMakeLists.txt (qb_load_modules) -->
target_link_libraries(my_app
    PRIVATE
        qb::core      # actor engine
        qb::io        # async runtime
        qbm::http     # HTTP/1.1, HTTP/2, WebSocket module
        qbm::redis    # Redis client module
)
```

Header-only qbm modules require the selected qb standard on the consumer; this is enforced automatically through `target_compile_features(... cxx_std_${QB_CXX_STANDARD})` on the module's `INTERFACE`.

### Runtime composition pattern

A service-facing actor typically:

1. Owns a module client (HTTP, Redis, PostgreSQL) constructed on its `VirtualCore`.
2. On an inbound request event, captures the request data by value and issues the module call.
3. Returns the module's result to the requester as an event — keeping the boundary between "this actor talks to Redis" and "that actor handles business logic" clean.

The example applications under `examples/qbm/` (HTTP servers with routing and middleware, the WebSocket chat, Redis pub/sub and streams, PostgreSQL transactions) demonstrate full wiring. Two composition styles recur:

- **Callback style.** The module client takes a completion callback that fires on the actor's core when the operation finishes; inside it the actor pushes a result event. Suitable for fire-and-forget and simple request/response.
- **Coroutine style.** The actor `spawn_async`-es a coroutine that `co_await`s the module operation, then returns the result through `ctx.push<Event>(...)`. This reads as straight-line code and composes naturally with the isolation rule above — capture the request by value, await, push back. Suitable for multi-step flows (read from Redis, then query PostgreSQL, then reply).

**Pitfalls.**

- A module client is thread-affine like every `qb-io` object: it must not be shared across cores. Create one per actor (or per core), not one global client.
- The coroutine-style composition is still bound by the [isolation rule](#coroutine-patterns-inside-actors): do not touch the actor's members after the first `co_await` on a module operation. Capture the request, await, and reply through `ctx`.

## See also

- [Framing messages with protocols](../3_qb_io/protocols.md) — the `AProtocol` contract and the built-in framers.
- [The engine: `qb::Main` and `VirtualCore`](../4_qb_core/engine.md) — placement, affinity, latency, and shutdown in depth.
- [C++20 coroutines](../3_qb_io/coroutines.md) — `task<T>`, awaiters, channels, scopes, and combinators.
- [Actor patterns](../4_qb_core/patterns.md) — service registries, supervision, request/response, and `require<>` discovery.
- [Building network actors](../5_core_io_integration/network_actors.md) — the `qb::io::use<>` mixins that attach transports and protocols to actors.
- [Performance tuning](./performance_tuning.md) and [Resource management](./resource_management.md) — when to apply these techniques and how to measure their effect.
