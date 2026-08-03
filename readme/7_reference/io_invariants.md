# qb-io invariants: threading, lifetime, and ownership

> **Audience:** Contributor · **Status:** stable · **Verified-against:** qb 3.0.0 (C++20 default, C++23 supported)

The rules the asynchronous stack assumes — one event loop per thread, where
callbacks run, when objects may be destroyed, and who owns each socket — with
the header and line that enforces each one.

**Prerequisites:** [Core concepts](../2_core_concepts/), [Asynchronous I/O](../3_qb_io/) —
**See also:** [Public API overview](./api_overview.md), [Core invariants](./core_invariants.md),
[Glossary](./glossary.md)

## Summary

`qb-io` achieves thread safety through isolation, not locks. Every event loop,
socket, session, timer, watcher, and coroutine scheduler belongs to exactly one
thread for its entire life. There are no mutexes on the I/O hot paths because
there is never more than one thread touching a given object.

This page is the reference contract for that model. Each invariant below is a
rule a change must not break; breaking one does not usually fail to compile — it
produces a silent data race, a dangling pointer, or a double free that survives
testing and surfaces in production. Read it once before you write protocol,
transport, session, or coroutine code, and consult it when something built on
`qb-io` crashes in a way that does not reproduce under a debugger.

The invariants are grouped by subsystem. Each bullet cites the header that owns
the rule as `path:line`. Where a claim is enforced by a single enum, constant,
or function, that symbol is named so you can verify it directly.

---

## 1. Thread model — one listener per thread

- Each thread owns **exactly one** `qb::io::async::listener`, reachable through
  the `thread_local` static member `listener::current`
  (`src/qb/io/async/listener.h:91`). Under `qb-core`, every `VirtualCore`
  worker thread has its own `listener::current`; the worker installs it before
  any I/O object is constructed on that core.
- Every async object — `async::input<>`, `async::output<>`, `async::io<>`,
  the `async::tcp` / `async::udp` clients and servers, `with_timeout<>`,
  `Timeout<F>`, `ScopedTimeout<F>`, and the file and directory watchers — binds
  to `listener::current` **at construction**. `async::base` registers its libev
  watcher in its constructor and stops + unregisters it in its destructor
  (`src/qb/io/async/io.h:79`, `:88`). The object **must be constructed and
  destroyed on the same thread**; the libev watcher is not cross-thread safe.
- The listener is **never locked**. All event handlers (`on()` methods) run
  sequentially on the listener's own thread, so I/O state needs no mutex or
  atomic (`src/qb/io/async/listener.h:63`). This is a strictly stronger and
  strictly single-threaded guarantee than `std::thread`-safety: `qb-io` objects
  are *not* `std::thread`-safe, they are *thread-confined*.
- To reach an I/O object from another thread, go through the actor mailbox
  (`qb::Actor::push<>` / `broadcast<>`) — never through a raw pointer. The
  destination core dequeues and dispatches the event on its own thread.
- On Windows, the libev epoll backend is wepoll (IOCP). The loop must not run
  on one thread while another thread closes the same loop or its epoll handle
  (`src/qb/io/async/listener.h:73`). One thread per listener satisfies this
  contract; do not break it on Windows even for "read-only" access.

Moving an async object is also forbidden. `async::base` declares no move
operations of its own, and the embedded `_async_event` reference binds to the
original object's address (`src/qb/io/async/io.h:71`). Copy is blocked at the
derived `input`/`output`/`io` level; a move would corrupt the listener
registration. Hold these objects in place — by `shared_ptr`, in a session
registry, or as a member — never relocate them.

---

## 2. `async::init()` and listener teardown

- `qb::io::async::init()` is a deliberate **no-op**
  (`src/qb/io/async/listener.h:717`). `listener::current` is a
  self-initializing `thread_local`; `init()` exists only as an explicit
  "this thread uses qb-io" marker. It must **not** clear the listener: it is
  called from multi-threaded test fixtures that have already constructed objects
  in the same thread-local listener, and clearing would dangle their registered
  kernel events.
- To reset event-loop state (for example in a unit-test teardown), call
  `listener::current.clear()` directly — never via `init()`.
- `listener::clear()` (and the destructor) **detach** watchers without
  **deleting** them (`src/qb/io/async/listener.h:448`). Each `async::base`
  still holds a reference to its embedded event, so the owning object's
  destructor performs the final unregister and delete. Deleting in `clear()`
  would leave a dangling `_async_event`.
- `clear()` runs the loop four times with `EVRUN_NOWAIT`, not `EVRUN_ONCE`,
  because under a monotonic-clock + timerfd libev build the loop can pick a
  multi-million-second wait time when `timercnt == 0`, which would wedge thread
  teardown (`src/qb/io/async/listener.h:486`). This is intentional; do not
  "simplify" it to a single `EVRUN_ONCE`.

---

## 3. Re-entrancy — never pump the loop from inside a handler

- `async::run` / `run_once` / `run_until` must **not** be called from inside a
  coroutine body or an actor handler that is already executing under
  `CoroutineScheduler::run_ready()`. `ensure_not_inside_ready_drain()` asserts
  in debug builds and throws `std::logic_error` in release
  (`src/qb/io/async/listener.h:732`).
- The same applies to the synchronous coroutine bridges `run_sync()` and
  `run_for()` (`src/qb/io/async/coroutine/utils.h:285`, `:227`): they are for
  test setup/teardown and non-coroutine entry points only. Each calls
  `ensure_not_inside_ready_drain()` on entry, so a re-entrant call asserts in
  debug and throws `std::logic_error` in release
  (`src/qb/io/async/coroutine/utils.h:288`, `:228`). A second, deeper
  per-scheduler `in_run_ready_` guard inside `run_ready()` itself asserts in
  debug and returns `0` in release should a nested drain still be reached
  (`src/qb/io/async/coroutine/scheduler.h:465`).
- `input` / `io` add a second, single-thread re-entrance guard: `on(event::io)`
  returns immediately when `_on_message` is already set, preventing recursive
  message processing within the same thread
  (`src/qb/io/async/io.h:1379`). This is intra-thread re-entrance
  protection, not cross-thread synchronization.

> **`run_once()` footgun.** The bundled libev disables timerfd by default — the
> `QB_EV_USE_TIMERFD` CMake option is `OFF` (`src/qb/vendor/qev/CMakeLists.txt:69`).
> Built with `-DQB_EV_USE_TIMERFD=ON` and with only `qev_io` watchers active
> (no heap timers, `timercnt == 0`), a single `run_once()` can block for libev's
> internal maximum wait time. Drive manual pumps with `run_until(...)` or
> `run(EVRUN_NOWAIT)` instead (`src/qb/io/async/listener.h:777`).

---

## 4. Timers, `async::callback`, and ownership of timeouts

- `async::callback(func)` with no duration, or with a non-positive duration,
  runs `func()` **inline immediately** — not on the next loop iteration
  (`src/qb/io/async/io.h:360`, `:366`). `Timeout<F>` and `ScopedTimeout<F>`
  mirror this fire-immediately semantics in their constructors.
- `async::callback(func, timeout)` with a positive duration creates a
  self-deleting `Timeout<F>` on the heap, which registers a libev timer.
  `Timeout<F>` owns its own lifetime and calls `delete this` when it fires
  (`src/qb/io/async/io.h:342`). Do not store, delete, or otherwise hold a
  `Timeout<F>` — it is fire-and-forget by design.
- For a timer you can **cancel or own**, use `async::scoped_callback(...)` /
  `ScopedTimeout<F>` (`src/qb/io/async/io.h:462`). These are caller-owned
  via `unique_ptr`; cancellation is destroying or reusing the object. They do
  not participate in the `delete this` dance.
- `Timeout`, `ScopedTimeout`, and the `RegisteredKernelEvent` slab use
  per-instantiation, thread-local LIFO freelists (custom `operator new` /
  `operator delete`). A steady-state `callback()` performs **zero**
  `malloc`/`free` once the pool is warm (`src/qb/io/async/io.h:212`). The
  pools are intentionally **never drained** at thread exit — the OS reclaims the
  thread's memory, and draining from a TLS destructor would race a late
  `delete this` from an already-fired timer whose loop iteration outlives the
  listener TLS. Do not add a drain step.
- All timeout, interval, and delay parameters in this layer are
  `qb::duration` (`std::chrono::nanoseconds`); the only raw `double` is libev's
  `qev_tstamp` (seconds) at the `qb::detail::to_ev_seconds` /
  `from_ev_seconds` seam (`src/qb/io/async/io.h:118`). The retired
  pre-2.0 capitalized time identifiers appear nowhere in this layer and must
  never be reintroduced; the canonical vocabulary is `qb::duration`,
  `qb::mono_time`, and `qb::wall_time`
  (`src/qb/system/time.h`).

> `callback()` refreshes libev's cached monotonic "now" (`qev_now_update`) before
> arming a timer, so a timer scheduled after the owning thread blocked outside
> the loop does not expire far earlier than requested
> (`src/qb/io/async/io.h:380`).

---

## 5. CRTP dispatch and user `on()` overrides

- Dispatch between the framework bases (`async::tcp::server`,
  `async::tcp::acceptor`, `async::input`/`output`/`io`) and the user's
  `_Derived` class is resolved **at compile time** via CRTP plus trait
  predicates such as `qb::has_on<T, Event>`
  (`src/qb/utility/type_traits.h:802`).
- The framework dispatches most lifecycle events as **rvalues** — for example
  `event::disconnected` is delivered with `std::move`. A user handler must use a
  compatible signature: `on(event::X&&)` or `on(const event::X&)`. A plain
  `on(event::X&)` (non-const lvalue reference) will not bind and is treated as
  *no override* (`src/qb/utility/type_traits.h:843`).
- When a CRTP base needs to decide whether `_Derived` **genuinely overrides** a
  handler (rather than merely inheriting the base fallback), use
  `qb::has_own_on<Derived, Base, Event>`
  (`src/qb/utility/type_traits.h:912`), **not** `qb::has_on`. `has_on`
  evaluates `true` for the inherited overload, so using it to drive a
  `static_cast<Derived&>(*this).on(e)` re-dispatch produces silent infinite
  recursion (`src/qb/utility/type_traits.h:802`).

---

## 6. Disconnect reasons and the dispose self-guard

- Disconnect reasons are the strongly-typed enum `disconnect_reason`, with
  underlying type `int` for ABI compatibility with the historical raw-integer
  field (`src/qb/io/async/event/disconnected.h:45`):

  | Constant | Value | Meaning |
  | --- | --- | --- |
  | `peer_closed` | `0` | Normal shutdown — peer closed, or the local side closed cleanly. Generated automatically on kernel EOF. |
  | `user_initiated` | `1` | Explicit `disconnect()` from user code. |
  | `protocol_error` | `-1` | Protocol marked itself `not_ok()`. |
  | `message_too_large` | `-2` | DoS guard: message exceeded `max_message_size()`. |
  | `buffer_overflow` | `-3` | DoS guard: read or write buffer exceeded the configured cap. |

  Positive codes are application-defined; the negative codes are framework-
  reserved and set automatically by the read/write/publish DoS guards. Prefer
  the strongly-typed `disconnect(disconnect_reason)` overload over a raw integer.
- `disconnect(0)` is remapped to `user_initiated` (`1`) because internally
  `_reason == 0` is the sentinel for "no disconnect pending"; the `peer_closed`
  (`0`) code is generated automatically on kernel EOF
  (`src/qb/io/async/io.h:1239`).
- `dispose()` is **idempotent** — guarded by `_is_disposed`, it runs once
  (`src/qb/io/async/io.h:1455`). It fires `on(event::disconnected)` if the
  derived class implements it; for a server-owned object it then notifies
  `server().disconnected(id())`, otherwise it stops the watcher and fires
  `on(event::dispose)`.
- For the entire duration of `on(event::io)`, the handler holds a
  `std::shared_ptr<void> _self_guard` to itself — acquired before any branch
  that can reach `dispose()` (`src/qb/io/async/io.h:1377`). This means a user
  who releases the last external `shared_ptr` from inside `on(disconnected)`
  cannot trigger a use-after-free in the rest of `dispose()`. The guard is typed
  `shared_ptr<void>` so it works even when `_Derived` inherits
  `enable_shared_from_this<Base>` with `Base != _Derived` (the HTTP CRTP session
  case). Do not remove or narrow this guard.

> All three destruction paths — peer close, framework-detected protocol or
> size violation, and explicit `disconnect()` — run **inside the listener
> callback on the owning thread**, never from another thread and never from an
> unrelated object's destructor.

---

## 7. Sessions in `io_handler<Session>`

- `io_handler::registerSession` enforces the session cap **before** allocation
  (`src/qb/io/async/io_handler.h:211`). If `_max_sessions > 0` and the
  registry is full, it closes the incoming I/O (which already owns an open fd,
  and possibly an `SSL*`) and returns `nullptr` — no `Session` is allocated. This
  prevents a connection flood from amplifying into heap pressure.
- The cap is **opt-in**. `_max_sessions` defaults to `QB_DEFAULT_MAX_SESSIONS`,
  which is `0` — meaning **unlimited** — for backward compatibility
  (`src/qb/io/config.h:233`). Set it explicitly with `set_max_sessions(n)`,
  or define `QB_DEFAULT_MAX_SESSIONS` before including the header, on any
  network-facing server.
- `stream()` / `stream_if()` fan-out reuses a persistent `_broadcast_scratch`
  vector (`src/qb/io/async/io_handler.h:115`) so broadcasting to N sessions
  costs O(N) reads, not O(N²) allocations.

---

## 8. Protocol contract (`IProtocol`)

The protocol base class `IProtocol` / `AProtocol<_IO_>` lives in
`src/qb/io/async/protocol.h`. Every concrete protocol under
`src/qb/io/protocol/**` depends on it.

- `getMessageSize()` is expected to be a **pure query**
  (`src/qb/io/async/protocol.h:102`). The stream layer may call it many
  times per byte of input; an implementation must not mutate parser state,
  decrement a credit counter, or emit semantically meaningful side effects.
  Base protocols keep a resumable scan offset so re-invocation does not rescan
  from the start.
- It returns `IProtocol::kNoMessage` (`== 0`) when more input is needed, or the
  exact byte count of the next complete frame otherwise
  (`src/qb/io/async/protocol.h:48`, `:72`). Consumption happens when
  `onMessage(size)` is called with that size.
- A protocol signals an unrecoverable framing or parse error by calling
  `not_ok()`; the I/O component then closes the connection, and the protocol
  cannot be recovered (`src/qb/io/async/protocol.h:185`).
- `set_should_flush(bool)` controls whether the input buffer is flushed after
  each message (`src/qb/io/async/protocol.h:202`). `process_messages`
  snapshots the **old** protocol pointer and its `should_flush()` before calling
  `onMessage()`, because `onMessage()` may `switch_protocol()` (handshake or
  upgrade) and leave the old protocol dangling — the flush must use the old
  protocol's policy (`src/qb/io/async/io.h:1296`).
- The `handshake` protocol is the documented exception to the pure-query rule:
  its `getMessageSize()` calls `transport().do_handshake()` (a side effect) and
  caches the result so the handshake step is never executed twice per buffer
  cycle (`src/qb/io/protocol/handshake.h:88`).

> **`event::eof` is not end-of-stream.** The event class is
> `event::input_drained`, with `using eof = input_drained` as a back-compat
> alias (`src/qb/io/async/event/eof.h:57`). It fires whenever a successful
> read empties the input buffer — even on a perfectly healthy, still-open
> connection. For an actual connection closure, handle
> `event::disconnected`, not `event::eof`.

---

## 9. Coroutine lifetimes

The coroutine layer (`src/qb/io/async/coroutine/**`) is a separate slice; see
the coroutine documentation for full semantics. The invariants that intersect
with I/O lifetime are:

- The entire layer is **strictly mono-thread (cooperative)**. One
  `CoroutineScheduler` belongs to exactly one thread — the VirtualCore worker or
  the listener's I/O thread (`src/qb/io/async/coroutine/scheduler.h:96`).
  Resuming or pushing from another thread is undefined behavior; cross-thread
  wake-ups go through the actor mailbox.
- A `thread_local` scheduler is established automatically when a
  `qb::io::async::listener` is created on the thread. `schedule_via_current()`
  asserts in debug and silently no-ops in release if no scheduler exists,
  leaving any queued waiter permanently unresumed
  (`src/qb/io/async/coroutine/scheduler.h:815`).
- **Awaiters must remain alive until `await_resume()`**
  (`src/qb/io/async/coroutine/awaiter.h:30`). Never create a temporary
  awaiter that goes out of scope before resumption; watchers are stopped in
  `await_resume()` (or the awaiter destructor) precisely to avoid a
  use-after-free.
- `~CoroutineScheduler` destroys only the ready-queue and deferred-completed
  frames it owns. **Suspended frames are intentionally leaked**, because their
  libev watchers still reference them
  (`src/qb/io/async/coroutine/scheduler.h:155`). **Stop the event loop
  before destroying the scheduler**, or those watchers fire against freed
  frames.
- `spawn()` takes ownership of the coroutine handle and runs it to completion
  even if the original `task` object is destroyed
  (`src/qb/io/async/coroutine/scheduler.h:90`). Pass a callable to `spawn`
  **without invoking it** (`spawn(f)`, not `spawn(f())`): creating a coroutine
  from a temporary lambda with reference or loop-variable captures dangles after
  the first suspension. `spawn(Callable)` moves the closure into an owning frame
  (`src/qb/io/async/coroutine/scheduler.h:378`).

---

## 10. Socket and descriptor ownership

- `qb::io::socket` and `qb::io::sys::file` are **move-only RAII owners** of their
  native handle. Copy construction and assignment are deleted, and the
  destructor closes the handle (`src/qb/io/system/file.h:77`). File copy was
  removed deliberately to prevent two objects double-closing the same descriptor
  — a bug that on Windows raises a fast-fail and on Linux can close a descriptor
  another thread has since reopened.
- The transport sockets follow the same rule: `tcp::socket`, `udp::socket`,
  `ssl::socket`, and `ssl::listener` all delete the copy constructor and default
  the move operations; ownership of the native handle (and `SSL*`) transfers on
  move (`src/qb/io/tcp/socket.h:91`). `tcp::socket` inherits non-publicly
  from `qb::io::socket`, so only its re-exported accessors are public
  (`src/qb/io/tcp/socket.h:43`).
- A default-constructed socket is **uninitialized**; call `init()` before any
  connect/accept/read/write. The success conventions differ and cannot be
  treated uniformly (`src/qb/io/tcp/socket.h:86`,
  `src/qb/io/udp/socket.h:117`):
  - `tcp::socket::init(int af)` returns `int` (`0` = success).
  - `udp::socket::init(int af)` returns `bool` (`true` = success).
  - `ssl::socket::init(SSL*)` returns `void` and adopts the supplied handle.
- The accept transports **detach** the accepted handle rather than closing it.
  `transport::accept::flush()` / `saccept::flush()` call `release_handle()` on
  the accepted socket so it is *not* closed when the transport's internal
  `_accepted_io` is reused — the handle (and `SSL*`) has already been moved into
  the user session by the protocol layer
  (`src/qb/io/transport/accept.h:112`). An in-flight TLS handshake survives
  the detach.
- All socket timeout parameters are `qb::duration`
  (`std::chrono::nanoseconds`); a non-positive wait is clamped to "poll once"
  in the timed connect/recv/send paths (`src/qb/io/tcp/socket.h:150`).
  Timeout semantics are deliberately asymmetric: `ssl` timed connect bounds only
  the TCP phase (the TLS handshake is unbounded), and `udp::socket::read_timeout`
  returns `-ETIMEDOUT` on expiry, whereas a generic non-blocking "no data" read
  returns `0` (`src/qb/io/tcp/ssl/socket.h:469`,
  `source/io/src/udp/socket.cpp:102`).
- The `file_watcher<>` / `directory_watcher<>` **own the watched path string for
  the watcher's lifetime**. Their `start()` takes a `std::filesystem::path`, but
  qev's `qev_stat` stores the narrow `const char *` it is given **without
  copying** (`src/qb/vendor/qev/qev++.h:696`). `start()` therefore stashes
  `fpath.string()` in the watcher's own `_watched_path` member and passes
  `_watched_path.c_str()` to the watcher (`src/qb/io/async/io.h:576`, `:740`).
  Do not pass a temporary's `c_str()` straight to the underlying `ev::stat`, and
  do not reassign or shrink `_watched_path` while the watcher is armed — the
  pointer libev holds would dangle and the next stat poll would read freed memory.

---

## 11. DoS bounds

Buffer growth and message size are hard-capped to bound a malicious or buggy
peer's memory footprint (`src/qb/io/config.h:172`, `:249`, `:263`):

| Constant | Default | Enforced by |
| --- | --- | --- |
| `QB_MAX_MESSAGE_SIZE` | 100 MB | per-message cap; over-limit marks the protocol `not_ok()` and disconnects with `message_too_large` |
| `QB_MAX_READ_BUFFER_SIZE` | 200 MB | input buffer growth; over-limit returns `ErrBufferLimitExceeded` and disconnects with `buffer_overflow` |
| `QB_MAX_WRITE_BUFFER_SIZE` | 200 MB | output buffer growth; over-limit makes `publish()` return `nullptr` and disconnects with `buffer_overflow` |

Setting a cap to `SIZE_MAX` disables that limit
(`src/qb/io/stream.h:124`, `:471`); it is discouraged for any network-facing
component.

---

## Appendix — diagnosing an invariant break

1. **Is the crash on the listener's thread?** Compare
   `std::this_thread::get_id()` inside the failing callback to the owning
   `VirtualCore`'s thread id. A mismatch means an object was constructed or
   destroyed on the wrong thread (§1).
2. **Is a watcher leaked?** `listener::current.size()` should be `0` at
   shutdown. A non-zero count points to an I/O object destroyed on the wrong
   thread, or a protocol that never unregistered (§1, §2).
3. **Is the freelist corrupted?** Temporarily remove the per-instantiation
   `operator new` / `operator delete` on `RegisteredKernelEvent<E,A>` /
   `Timeout<F>`. If the crash disappears, a double-free or wrong-size delete is
   hitting the pool (§4).
4. **Is `on(Event&&)` recursing?** Confirm with
   `qb::has_own_on<Derived, Base, Event>` that the derived class actually
   provides the override, and that its signature is `&&` or `const&`, never a
   non-const lvalue reference (§5).
5. **Did a coroutine outlive its scheduler?** Verify the event loop was stopped
   before the `CoroutineScheduler` was destroyed; suspended frames are leaked,
   not destroyed, and their watchers reference freed memory if the loop runs
   again (§9).
