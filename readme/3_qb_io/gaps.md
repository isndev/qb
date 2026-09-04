# What has no coroutine form, and why

> **Audience:** Adopter · **Status:** stable · **Verified-against:** qb 3.1.0 (C++20 default, C++23 supported) — f1d8cca6

`qb-io` has a full coroutine layer, and most of it is not connected to the network stack. Four capabilities have **no** `co_await` spelling — accept, QUIC, signals, and file I/O — and three more are synchronous in a way a coroutine cannot hide: hostname resolution, key derivation, and compression. This page names each one, gives the structural reason, and says what to do instead. Read it before you go looking for an awaiter that is not there.

**Prerequisites:** [C++20 coroutines](./coroutines.md) · [The async runtime](./async_system.md) — **See also:** [Transports](./transports.md) · [QUIC](./quic_transport.md) · [qb-io utilities](./utilities.md)

## What *does* have a coroutine form

The complete list, and it is short. Everything else in `qb-io` reaches coroutines only through one of these.

| Spelling | Parks on | Declared |
|---|---|---|
| `co_await sleep(qb::duration)` | a `ev_timer`; a non-positive duration is a cooperative yield with no timer at all | `coroutine/utils.h:101` |
| `co_await wait_readable(fd)` / `wait_writable(fd)` / `wait_for_io(fd, events)` | a `ev_io` watcher on a **raw descriptor** | `coroutine/utils.h:127`, `:158`, `:181` |
| `co_await tcp::connect<Transport>(uri, timeout, verify_peer)` | the callback connector's completion | `async/tcp/connector.h:759` |
| `co_await tcp::starttls_connect<Transport, Negotiator>(uri, timeout, verify_peer)` | the same, plus an in-band TLS upgrade | `async/tcp/connector.h:944` |
| `co_await async_awaiter<T>(start_op)` | whatever callback you hand it | `coroutine/awaiter.h:619` |

Everything above `wait_readable` in the stack — sessions, servers, acceptors, the QUIC endpoint — is **callback-driven by construction**, and the bridge back into a coroutine is `async_awaiter<T>` or a hand-rolled awaiter of the same shape. That is not an accident of implementation: a session's bytes belong to a protocol, and a protocol's `onMessage()` is a `void` function called from inside the read loop. There is no point in that chain where the framework could suspend on your behalf without deciding *which* message you were waiting for.

## Accept has no awaiter

There is no `co_await tcp::accept(listener)`. The acceptor is a full CRTP component — `input<acceptor<Derived, Prot>>` composed with the accept protocol (`src/qb/io/async/tcp/acceptor.h:50-52`) — whose product is a `tcp::socket` delivered to `on(Protocol::message&&)`.

The structural reason is that "accept" is not a suspension point with one answer. A listener produces an unbounded stream of sockets; a coroutine that awaited one would have to own the listener between calls, and the ownership question ("who holds the listener while nobody is awaiting?") is exactly what the acceptor component already answers. Nothing in the tree needs the other shape, so it does not exist.

If you want it in a `main()` or a test, `wait_readable` on the listening descriptor gives it to you in five lines:

```cpp
// src: derived from qb/tests/io/unit/coroutine/awaiter-protocol.cpp:764-782 (wait_readable on a raw fd)
#include <qb/io/async.h>
#include <qb/io/tcp/listener.h>

using namespace qb::io::async;

task<void> accept_loop(qb::io::tcp::listener &lst) {
    lst.set_nonblocking(true);
    for (;;) {
        co_await wait_readable(lst.native_handle());
        qb::io::tcp::socket sock = lst.accept();   // not open when the accept failed
        if (sock.is_open())
            handle(std::move(sock));
    }
}
```

Two caveats that the acceptor component handles for you and this loop does not: a transient `ECONNABORTED`, `EPROTO` or fd-table exhaustion is a normal condition and must be retried rather than treated as a listener failure (`src/qb/io/transport/accept.h:90-104` is where the component remaps them), and the accepted socket's handle has to be released into whatever owns the session next (`src/qb/io/transport/accept.h:116-118`). Inside the runtime, use `qb::io::use<...>::tcp::acceptor` — see [Transports](./transports.md#acceptance-transport-qbiotransportaccept).

## A session has no `co_await read()`

Nothing lets you write `auto msg = co_await session.next_message()`. Bytes arrive at the `io` base's `on(event::io const &event)` handler (`src/qb/io/async/io.h:2744`), are framed by the active protocol in `process_messages()` (`:2597`), and are delivered synchronously to your `on(Protocol::message&&)`. The read loop drains every complete frame in the buffer before returning.

The bridge in the other direction is the one qbm's three modules use, and it is worth naming because it is the pattern: a request is written, its completion callback is stored, and an awaiter parks the coroutine until that callback fires. `async_awaiter<T>` does this generically (`src/qb/io/async/coroutine/awaiter.h:619-698`), and the modules hand-roll the same shape when they need a richer result type. See [C++20 coroutines](./coroutines.md#bridging-a-callback-api) for the mechanics and the lifetime rules.

## QUIC has no coroutine surface at all

Not one `co_await`, `co_return`, `task<` or awaiter appears in any of the eight QUIC sources — `src/qb/io/quic.h`, `quic.cpp`, `quic/types.h`, `quic/backend.h`, `async/quic.h` and the five headers under `async/quic/`. The whole surface is callback and virtual dispatch: `endpoint::dispatch(event::…)` overloads (`src/qb/io/async/quic/endpoint.h:56-71`) forwarded to your class by `requires`-guarded detection.

Three properties of the design make an awaiter genuinely hard here, and they are visible in the code rather than being a matter of taste:

- **There is no per-stream descriptor.** One UDP socket feeds `_backend->on_udp_datagram(...)` for every connection and every stream (`src/qb/io/async/quic/endpoint.h:609`). `wait_readable` has nothing to park on.
- **`event::stream_data::payload` is a borrowed view.** The backend batches every connection's events into one vector that the endpoint drains in a single loop, and the payload is a `std::string_view` into that vector (`src/qb/io/async/quic/endpoint.h:271`). It cannot survive a suspension, so an awaiter would have to copy every frame before parking — which is exactly what the protocol layer already lets you decide for yourself.
- **Resuming inside the drain is the bug the endpoint guards against.** `drain_backend_events` refuses to re-enter, setting `_drain_events_again` and returning instead (`src/qb/io/async/quic/endpoint.h:209-243`); the header calls the alternative "the root of a whole class of UAF / buffer-underflow bugs". A coroutine resumed mid-drain is precisely that re-entrancy.

Use the callback surface, and if you want coroutine ergonomics on top of it, park on your own `async_awaiter<T>` that a `dispatch(event::stream_data)` handler completes — after copying the payload.

## Signals are a watcher, not an awaitable

`event::signal<Sig>` wraps a libev `ev::sig` watcher (`src/qb/io/async/event/signal.h:82`) and is delivered like any other event: register it, implement `on(event::signal<SIGINT>&)`. There is no `co_await wait_signal(SIGINT)`.

Under `qb-core` you do not use it directly at all: `qb::Main` installs the process-level handler and turns a raw signal into a `SignalEvent` broadcast, coalesced to the most recent signum, which reaches actors through `onSignal` / `kill()` (`src/qb/core/VirtualCore.cpp:729-745`). That is the supported path, and it is an actor-tier concern.

## File I/O is polled metadata plus a blocking read

This one is a **capability gap, not a documentation gap**, and it is the one most likely to bite on a `VirtualCore`.

`async::file<Derived>` is `file_watcher<Derived>` composed with `transport::file` (`src/qb/io/async/file.h:44-46`). Two things follow:

- **The notification is polled.** `file_watcher::start(path, interval)` arms a libev `ev::stat` watcher, which `stat()`s the path on a timer — the default cadence is 100 ms (`src/qb/io/async/io.h:581`). It is not inotify, not FSEvents, not `kqueue`'s `EVFILT_VNODE`. Shorter intervals cost CPU proportionally.
- **The read is synchronous.** When the watcher reports growth, `read_all()` loops `Derived.read()` until the file is drained (`src/qb/io/async/io.h:621-644`), and that read is `qb::io::sys::file::read`, an ordinary blocking descriptor read (`src/qb/io/system/file.h:166`). `transport::file::write()` is a placeholder that returns `0` and writes nothing (`src/qb/io/transport/file.h:52-55`).

On a page-cached local file the read returns without ever blocking and none of this is observable. On a cold file, a network filesystem, or a slow device, the `VirtualCore` thread stops inside `read()` — every actor on that core with it, and with no diagnostic, exactly as described for [`run_sync`](./async_system.md#run_sync-and-run_for-block-the-calling-thread).

What to do about it, in order of preference: read the file **before** `qb::Main::start()`, where the thread is yours; or read it on a thread you own and deliver the contents to the actor as an event; or accept the stall knowingly for a small, warm, local file. `qb::io::sys::file_to_pipe` (`src/qb/io/system/file.h`) is the one-call form for the first two.

## Hostname resolution is synchronous — including inside `co_await tcp::connect`

`qb::io::socket::resolve` and its `_v4` / `_v6` / `_v4to6` siblings call `getaddrinfo` directly (`src/qb/io/system/sys__socket.h:1437-1465`). `getaddrinfo` is a blocking call: on a cache miss it does network I/O and can take as long as the resolver takes.

That matters more than it first appears, because the resolution is on the **coroutine** connect path too. `co_await tcp::connect(uri)` calls `await_suspend`, which runs the callback `connect` overload synchronously — that overload builds a `connector` and calls `run()` before returning (`src/qb/io/async/tcp/connector.h:568-572`). `run()` reaches `socket_.n_connect(remote_)` (`src/qb/io/async/tcp/connector.h:337`); and for a hostname URI that reaches `n_connect_in`, which resolves through `resolve_i` before the first non-blocking `connect` syscall (`src/qb/io/tcp/socket.cpp:169-181`, `:204-212`). **The DNS lookup therefore happens before the coroutine ever parks**, on the loop thread.

There is no asynchronous resolver in the tree, so the honest options are:

- Connect to an **endpoint** rather than a hostname where you can. `n_connect(endpoint const&)` performs no lookup (`src/qb/io/tcp/socket.cpp:186`), and `qb::io::endpoint::as_in(host, port)` accepts a numeric address.
- Resolve once, at startup, on a thread you own, and cache the `endpoint`.
- Accept the stall where the thread is yours to block — a `main()`, a CLI, a test fixture.

## Cryptography and compression are CPU work on the calling thread

Neither is asynchronous and neither can be, because neither is waiting for anything — they are computation. The point is only that the computation is charged to whichever thread runs it, and on a `VirtualCore` that is every actor on the core.

Key derivation is the case worth stating explicitly: PBKDF2, HKDF and Argon2 are *deliberately* slow, and their cost is a parameter the caller chooses. An iteration count sized for a login endpoint is milliseconds of wall time on the core, per call. Compression and decompression scale with payload size the same way.

The remedy is the same shape as for file I/O: do it before the engine starts, do it on a thread you own and hand the result back through the actor mailbox, or size the work so the stall is acceptable. See [qb-io utilities](./utilities.md) for the surfaces themselves.

## The shape of the rule

Everything on this page reduces to one question, and it is the same question [`run_sync`](./async_system.md#the-rule) asks: **whose thread is this?**

Outside the engine — a `main()`, a test, a CLI, a setup step — the thread is yours, blocking it is honest, and the absence of an awaiter costs you nothing but syntax. Inside an actor the thread is a `VirtualCore` shared with every actor on it, and each of these gaps becomes a latency source that no test will show you, because the loop keeps turning and the socket keeps answering while your actors do not.

## See also

- [C++20 coroutines](./coroutines.md) — the awaitables that *do* exist, and what each one does on cancellation.
- [The async runtime](./async_system.md) — the loop turn, and why blocking the calling thread inside an actor costs what it costs.
- [Transports](./transports.md) — the callback components that cover accept, sessions and servers.
- [Native QUIC and HTTP/3 transport](./quic_transport.md) — the callback surface QUIC does have.
- [qb-io utilities](./utilities.md) — crypto, compression, URI parsing and the synchronous file helpers.
