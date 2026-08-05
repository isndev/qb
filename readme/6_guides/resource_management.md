# Resource management

> **Audience:** Adopter · **Status:** stable · **Verified-against:** qb 3.0.0 (C++20 default, C++23 supported)

How RAII, actor ownership, and the actor lifecycle combine to release memory, descriptors, sockets, and TLS contexts deterministically, and where the framework hands ownership back to you.

**Prerequisites:** [The actor model](../2_core_concepts/actor_model.md), [Getting started](./getting_started.md) — **See also:** [Error handling](./error_handling.md), [Advanced usage](./advanced_usage.md)

## Summary

The qb runtime owns the *thread of control*: it constructs an actor on a `VirtualCore` worker thread, runs `onInit()`, dispatches events, and calls `~Actor()` after termination — all on that one thread. You own the *resources*. Wrap each resource (heap memory, a file descriptor, a socket, an `SSL_CTX`, a mutex) in a C++ object whose destructor releases it, declare that object as an actor member, and the runtime's deterministic destruction order releases everything for you. This page covers the ownership rules you must know to make that work: when `~Actor()` actually runs, which qb types own their handles and which hand ownership back to you, and where a reference must outlive an asynchronous operation.

## Concepts

### RAII is the contract, not a suggestion

Resource Acquisition Is Initialization (RAII) ties a resource's lifetime to an object's lifetime: acquire in the constructor, release in the destructor. The framework does not run a garbage collector, scan for leaks, or close handles you forgot about. Everything below rests on one guarantee — **a destructor that runs releases its resource exactly once** — so your job is to ensure the owning object is destroyed exactly once, on the right thread, in the right order.

The standard library already provides the building blocks:

| Resource | RAII owner |
| --- | --- |
| Exclusive heap object / array | `std::unique_ptr<T>`, `std::unique_ptr<T[]>` |
| Shared heap object | `std::shared_ptr<T>` |
| Dynamic buffers, strings, maps | `std::vector`, `std::string`, `std::map`, `qb::string<N>`, `qb::allocator::pipe<char>` |
| File streams | `std::fstream`, `std::ifstream`, `std::ofstream` |
| Scoped lock | `std::lock_guard`, `std::unique_lock`, `std::scoped_lock` |

qb adds owners for the resources it introduces. The key invariant for every one of them is the same: they are **move-only** — copy construction and copy assignment are deleted, and the move transfers the native handle, leaving the moved-from object in a closed/empty state so the destructor never double-frees.

| qb resource | Owner type | Header | Ownership note |
| --- | --- | --- | --- |
| Native file descriptor | `qb::io::sys::file` | `src/qb/io/system/file.h` | Move-only; destructor closes the handle. Copy was deliberately deleted to avoid a double-close. |
| Native socket descriptor | `qb::io::socket` (the base `inet::socket`, aliased into `qb::io`) | `src/qb/io/system/sys__socket.h` | Move-only; destructor closes the descriptor. |
| TCP socket | `qb::io::tcp::socket` | `src/qb/io/tcp/socket.h` | Move-only; closes the descriptor on destruction. |
| UDP socket | `qb::io::udp::socket` | `src/qb/io/udp/socket.h` | Move-only. |
| TLS socket | `qb::io::tcp::ssl::socket` | `src/qb/io/tcp/ssl/socket.h` | Move-only; owns and frees its `SSL` object (held in a `std::unique_ptr`) on destruction. |
| TLS listener | `qb::io::tcp::ssl::listener` | `src/qb/io/tcp/ssl/listener.h` | Move-only; **takes ownership of the `SSL_CTX`** you pass to `init()` and frees it via `std::unique_ptr`. |

<!-- src: src/qb/io/system/file.h:77 -->
<!-- src: src/qb/io/system/sys__socket.h:850,858,872,878 (copy deleted / move kept), 893 (~socket) -->
<!-- src: src/qb/io/tcp/socket.h:91 -->
<!-- src: src/qb/io/tcp/ssl/socket.h:338 (_ssl_handle unique_ptr); src/qb/io/tcp/ssl/listener.h:44 (listener _ctx unique_ptr) -->

### The actor destruction guarantee

For an actor, RAII works because the runtime gives you a precise destruction contract. Three points define it:

- **Construction is thread-affine.** An actor is constructed on the `VirtualCore` worker thread that will host it, never on the main thread. Use `Main::core(idx).addActor<T>(...)` or `addRefActor<T>(...)`; constructing an actor from an arbitrary thread asserts (the constructor checks `VirtualCore::_handler != nullptr`). (`src/qb/core/Actor.cpp:115`)
- **`onInit()` runs once, before any business event.** It runs after construction and ID assignment and is an async coroutine (`qb::io::async::task<bool>`) that may `co_await`; while suspended the actor is *Activating* (inbound unicast stashed + replayed FIFO once active, bounded by the activation deadline). `co_return false` (or an uncaught exception) aborts registration and **immediately destroys the actor** — so any resource you acquired in the constructor is released right away. What you observe depends on the creation path: a runtime `addRefActor<T>()`/`addRefHandle<T>()` hands you back an **empty handle** (`!valid()`), whereas a pre-start `addActor<T>()` whose `onInit()` fails *synchronously* at startup flags the core `BadActorInit` and the core fails to start (`Main::hasError()` is true). See [Error handling](./error_handling.md) for the full failure table. (`src/qb/core/Actor.h:317`, `src/qb/core/VirtualCore.cpp:469`, `src/qb/core/Main.cpp:210`)
- **`kill()` flags, it does not destroy.** `kill()` sets `_alive = false` and asks the `VirtualCore` to retire the actor. The actor stops receiving *new* events but may still drain events already queued; **`~Actor()` runs later, under `VirtualCore` control**, on the same worker thread. (`src/qb/core/Actor.cpp:282-290`)

Because destruction is single-threaded and deterministic, member subobjects are destroyed in reverse declaration order after your `~MyActor()` body returns. Declare your RAII members and let the compiler-generated cleanup do the rest.

```cpp
// Deterministic cleanup driven entirely by member RAII.
#include <qb/actor.h>
#include <qb/io.h>
#include <fstream>
#include <memory>
#include <vector>

class ReportWriter : public qb::Actor {
    std::unique_ptr<int[]>        _scratch;   // delete[] on destruction
    std::ofstream                 _out;       // closed on destruction
    std::vector<std::string>      _rows;      // frees its storage

public:
    explicit ReportWriter(std::string path)
        : _scratch(std::make_unique<int[]>(256))
        , _out(std::move(path), std::ios::out) {}

    qb::io::async::task<bool> onInit() override {
        if (!_out.is_open()) {
            qb::io::cout() << "ReportWriter[" << id() << "]: cannot open output\n";
            co_return false; // actor is destroyed here; _scratch + _out already released
        }
        _rows.reserve(64);
        co_return true;
    }

    // No explicit destructor needed: _rows, _out, _scratch are released
    // in reverse declaration order after ~ReportWriter() runs.
};
```

Write an explicit `~MyActor()` only when you need an *action* (flush a buffer, emit a final log line), not to release members — those are released for you.

### Cleanup ordering: when work must happen before the destructor

RAII releases resources, but it cannot perform side effects that must reach *other* actors or external systems while they are still operational. Sending an event, unregistering from a manager, or notifying a peer must happen during the shutdown sequence — before `~Actor()`. The hook for that is `on(const qb::KillEvent&)`.

Every actor is, by default, subscribed to `KillEvent` (along with `SignalEvent`, `UnregisterCallbackEvent`, `PingEvent`, and `RequireEvent`) at construction. (`src/qb/core/Actor.cpp:120-124`) Constructing with `qb::no_default_events` skips all five, in which case the derived class must register at least `KillEvent` in `onInit()` to shut down gracefully. (`src/qb/core/Actor.h:75`)

Override `on(const qb::KillEvent&)`, perform the side effects, then **call `kill()`** to let the runtime proceed to destruction:

```cpp
// src: derived from examples/all/auction_house patterns
#include <qb/actor.h>
#include <qb/io.h>

struct UnsubscribeEvent : qb::Event { qb::ActorId who; };

class Subscriber : public qb::Actor {
    qb::ActorId _manager;
public:
    explicit Subscriber(qb::ActorId manager) : _manager(manager) {}

    qb::io::async::task<bool> onInit() override { co_return true; }

    void on(const qb::KillEvent &) {
        // Side effect that must reach the manager while it is still alive:
        if (_manager.is_valid())
            push<UnsubscribeEvent>(_manager).who = id();

        kill(); // hand control back to the runtime; ~Subscriber() runs later
    }
};
```

Two rules follow from the lifecycle:

- **Always end the handler with `kill()`.** If you override `on(KillEvent&)` and forget it, the actor never terminates.
- **Do not send events from `~Actor()`.** A destructor that pushes events relies on the destination actor still being operational, which is not guaranteed during teardown. Put inter-actor side effects in the `KillEvent` handler, not the destructor.

### Referenced actors are not owned

`addRefActor<T>(...)` creates a child actor on the **same** `VirtualCore` and returns a phase-aware `qb::ActorHandle<T>` (alias `RefActorHandle<T>`). The parent does **not** own the child: the parent's destructor will not delete it, and the child manages its own lifecycle through its own `kill()`. The handle **never dangles** — it resolves the live pointer on demand, so `get()` / `operator->` return `nullptr` once the child calls `kill()` (and while it is still Activating, or after a failed init). Send to `handle.id()` at any time; gate direct calls on `handle.ready()`. (`src/qb/core/Actor.h`)

Two safe patterns:

- **Send events, not pointer calls.** Capture the child's `id()` and `push<Event>(child_id)`. Events to a dead actor are dropped by the router, so this never dereferences freed memory. (`src/qb/core/Actor.h:798`)
- **Hold the phase-aware handle.** `addRefActor<T>()` (and its alias `addRefHandle<T>()`) returns a `qb::ActorHandle<T>` you can keep across event-handler boundaries. Its `get()` re-queries the owning `VirtualCore` (via `findActor`) and returns `nullptr` if the child is still Activating, failed init, or has died — never a dangling pointer; `operator->()` / `operator*()` call `get()` and assert non-null in debug builds. (`src/qb/core/VirtualCore.h:916`)

```cpp
// src: src/qb/core/Actor.h (addRefHandle / RefActorHandle)
#include <qb/actor.h>

class Worker : public qb::Actor {
public:
    qb::io::async::task<bool> onInit() override { co_return true; }
    void do_step() { /* ... */ }
};

class Coordinator : public qb::Actor {
    qb::RefActorHandle<Worker> _worker;
public:
    qb::io::async::task<bool> onInit() override {
        _worker = addRefHandle<Worker>(); // empty if creation failed
        co_return _worker.valid();
    }

    void tick() {
        if (auto *w = _worker.get()) // nullptr if the Worker has been killed
            w->do_step();
    }
};
```

A `RefActorHandle` may only be dereferenced from the owning `VirtualCore`'s worker thread — the same thread that created the child. `get()` resolves the actor against the calling thread's `VirtualCore` (`VirtualCore::_handler` is thread-local), so an off-thread dereference does not find the child and returns `nullptr`; it never reaches across threads to a live actor. Treat the handle as single-thread state.

If the parent needs its referenced children gone when it stops, it must send each child a `KillEvent` explicitly (typically from the parent's own `on(KillEvent&)` handler). The runtime does not cascade kills down a reference tree.

### TLS contexts: who frees the `SSL_CTX`

`SSL_CTX` is the one place where ownership splits, so it deserves explicit attention.

- **Helper-created contexts are caller-owned.** `qb::io::ssl::create_client_context(method)` and `qb::io::ssl::create_server_context(method, cert_path, key_path)` each return a raw `SSL_CTX*` (or `nullptr` on failure) that **you must free with `SSL_CTX_free()`** unless you hand the context off (see below). (`src/qb/io/tcp/ssl/socket.h:81`, `:92`)
- **A listener you hand it to takes ownership.** `qb::io::tcp::ssl::listener::init(SSL_CTX*)` stores the context in a `std::unique_ptr` and frees it on destruction. Once you call `init()`, do **not** call `SSL_CTX_free()` yourself — that is a double-free. Call `init()` before `listen()`. (`src/qb/io/tcp/ssl/listener.h:90`)

The transport-based server pattern below is the common case: the freshly created context is passed straight into the transport's listener, which then owns it for the transport's lifetime.

```cpp
// src: qb/tests/io/system/session/text-session-loopback.cpp:283
#include <qb/io/tcp/ssl/socket.h>
#include <qb/io/tcp/ssl/listener.h>

using namespace qb::io;

// Server: create_server_context returns an owned SSL_CTX*; the listener's
// init() takes ownership and frees it on destruction. No SSL_CTX_free here.
server.transport().init(ssl::create_server_context(
    TLS_server_method(), "cert.pem", "key.pem"));

// Client: secure by default. qb-io loads the system trust store, enables
// SSL_VERIFY_PEER, and verifies the certificate against the target host.
// set_insecure() opts out and disables MITM protection — call before connect.
client.transport().set_insecure();
```

Two further facts shape correct TLS lifetime management:

- **TLS is secure by default.** When qb-io builds the client `SSL_CTX` itself, it loads the system trust store, enables `SSL_VERIFY_PEER`, and verifies the server certificate against the hostname or IP. `set_insecure()` must be called *before* `connect()`/`n_connect()` to opt out, and disables MITM protection. When you supply your own `SSL*` via `init(SSL*)`, qb-io does not change your verification policy. (`src/qb/io/tcp/ssl/socket.h:782`)
- **A TLS session you extract is yours to free.** A `qb::io::ssl::Session` obtained from `socket::get_session()` must be released with `qb::io::ssl::free_session()` when no longer needed. (`src/qb/io/tcp/ssl/socket.h:701`)

### `qb::io::use<>` ties transport lifetime to the actor

When an actor inherits from a `qb::io::use<>` base (for example `qb::io::use<MyClient>::tcp::client<>`), the networking transport — which owns the socket — is a subobject of that base. Its lifetime is therefore the actor's lifetime: when the actor is destroyed, the base subobject is destroyed, the transport's socket destructor runs, and the descriptor is closed. You do not manage the socket directly. If you need the connection torn down *before* the rest of teardown (for instance, to flush an application-level goodbye), call `this->disconnect()` — the method the `tcp::client` base exposes — from your `on(KillEvent&)` handler; RAII still handles the final close either way. (`src/qb/io/async.h:77`, `src/qb/io/async/io.h:1238`)

See [Networking with qb-io](../3_qb_io/README.md) for the transport hierarchy.

### References that must outlive an asynchronous operation

The hardest lifetime bugs in an event-driven system are not leaks — they are use-after-free, where a buffer or object is destroyed while an in-flight operation still holds a reference into it. The actor model removes the *thread-safety* part of this problem (one writer per core), but not the *temporal* part: a reference handed to an asynchronous operation must remain valid until that operation completes.

Three concrete cases from the framework:

- **Zero-copy broadcast.** In the message-broker pattern, a payload is stored once in a `broker::MessageContainer` and shared across many events via an internal `shared_ptr`; the events carry `std::string_view`s into that container. The container's lifetime must outlive event delivery — drop it too early and every view dangles. (`examples/core_io/message_broker/README.md:74`)
- **Accepted-socket ownership transfer.** In a TCP accept handler, the accepted socket must be **moved** out of the `on(accepted_socket_type&&)` parameter into the event immediately: `evt.socket = std::move(sock);`. This transfers descriptor ownership to the worker that will service the connection, on its core. Copying is not an option — the socket is move-only — and leaving the fd in the parameter would close it when the handler returns. (`examples/all/auction_house/include/auction_house/actors/tcp_listener.h:59`)
- **Coroutine awaiters.** An awaiter must remain alive until `await_resume()`. Never create a temporary awaiter that goes out of scope before resumption — its watcher is stopped in `await_resume()`/the destructor specifically to avoid use-after-free. (`src/qb/io/async/coroutine/awaiter.h:30`)

For ad-hoc cleanup that is not naturally a class member — a temporary handle from a C API, a rollback on an early return — use the framework's lightweight guards instead of hand-rolled `try`/`catch`:

```cpp
// src: src/qb/system/cpu.h
#include <qb/system/cpu.h>

void with_external_handle() {
    void *h = acquire_external();
    auto guard = qb::scope_guard([&] { release_external(h); });
    // ... use h; release_external runs on every exit path ...
    // guard.dismiss(); // call to keep the resource on the success path
}
```

`qb::scope_guard` runs its callable on destruction unless `dismiss()` was called; it is `[[nodiscard]]` and move-constructible (copy construction and both assignment operators are deleted), so it cannot be copied or reassigned, only moved at construction. `qb::resource(handle, cleaner)` wraps a `void*` in a `std::unique_ptr<void, TCleaner>` for the same purpose. (`src/qb/system/cpu.h:60`, `:73`)

## Pitfalls

- **Overriding `on(KillEvent&)` without calling `kill()`.** The actor never terminates and its destructor never runs. Always end the handler with `kill()`.
- **Sending events from a destructor.** Teardown order does not guarantee the destination is still operational. Move inter-actor side effects into the `KillEvent` handler.
- **Calling `handle->method()` on a child that may not be active.** `addRefActor<T>()` returns a `qb::ActorHandle<T>` whose `get()` is `nullptr` while the child is Activating, after a failed init, or once it died. Gate direct calls on `handle.ready()` (check `get()` for `nullptr`), or interact via `push<Event>(handle.id())`.
- **Calling `SSL_CTX_free()` on a context you already passed to `ssl::listener::init()`.** The listener owns it now; this is a double-free. Free only contexts you never handed off.
- **Forgetting `set_insecure()` is a downgrade, not a tweak.** It disables peer verification and MITM protection. It exists for test and trusted-network scenarios; do not reach for it to silence a certificate error in production.
- **Copying a move-only qb resource.** Sockets and `sys::file` delete copy precisely so a stray copy cannot cause a double-close. Use `std::move`, or `std::shared_ptr<file>` when shared ownership is genuinely required.
- **Letting a referenced buffer die before an async operation completes.** Keep the owner alive (a member, a `shared_ptr`, or a moved-in payload) for the full duration of the operation, including zero-copy `string_view` broadcasts and in-flight coroutine awaiters.

## See also

- [The actor model](../2_core_concepts/actor_model.md) — construction, `onInit()`, `kill()`, and destruction ordering in depth.
- [Error handling](./error_handling.md) — failure paths and how cleanup interacts with fault tolerance.
- [Networking with qb-io](../3_qb_io/README.md) — sockets, transports, and the `qb::io::use<>` hierarchy.
- [Advanced usage](./advanced_usage.md) — referenced actors, inner components, and shared state patterns.
