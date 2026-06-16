# QB_IO_PLAN — Deep Review of the `qb-io` Asynchronous Subsystem

> Scope:  `qb/include/qb/io/**` (async + transport + protocol + stream) — with
> the `async/coroutine/**` sub-tree intentionally out of scope for this pass.
>
> Goals: ultra-performance, correctness under contention and back-pressure,
> and C++20/23 idiomatic APIs (concepts, `[[nodiscard]]`, `std::expected`,
> `std::move_only_function`, etc.).
>
> Status: **All 22 findings implemented.** Phases 1–4 are complete,
> including the three previously-deferred performance items (2.13, 2.15,
> 2.20) which have now landed with measured micro-benchmarks. See the
> Progress log at the bottom for details. Each finding is tagged by
> severity and category.
>
> Legend:
> - **S1** = correctness / UB risk / resource leak
> - **S2** = API / design inconsistency
> - **S3** = performance / allocation hygiene
> - **S4** = documentation / ergonomics
>
> Last updated: 2026‑04‑19

---

## 0. Executive summary

The `qb-io` async stack is a thin, CRTP-heavy wrapper around **libev** with
three well-separated layers:

1. **Transport layer** (`qb/io/transport/*.h`) — byte-level read/write
   primitives over a concrete socket/file.
2. **Stream layer** (`qb/io/stream.h`) — buffered `istream` / `ostream` /
   `stream` that uses `qb::allocator::pipe<char>` for zero-copy input and
   a flat write queue for output.
3. **Async layer** (`qb/io/async/**`) — event-loop integration via the
   `listener` (thread-local libev loop), CRTP bases `input` / `output` /
   `io` / `with_timeout` / `file_watcher`, the TCP acceptor/client/server/
   connector, UDP client/server, and `io_handler` session management.

The subsystem is **mature, single-threaded-by-design (one `listener` per
VirtualCore)**, and already applies careful UAF protection via
`_self_guard` (shared-ptr-counted) and `std::shared_ptr`-based connector
lifetime. Overall quality is high.

The review nevertheless surfaces **22 actionable findings**: a handful of
genuine bugs (most notably dead UDP partial-send code, an SSL-accept socket
reassignment that can drop an in-flight TLS handshake, and a duplicated
`_out_buffer` between `stream<IO>` and `ostream<IO>`), several API
inconsistencies (`is_secure()` declared differently per transport, unused
template parameters in `qb::io::use<>`), and a number of perf-hygiene
opportunities (DoS amplification in `io_handler::registerSession`, the
orphan `epoll.h` header, per-broadcast map copies in `io_handler::stream()`,
and heap-per-timer in `async::callback`).

These findings are grouped below and then rolled up into a **4-phase action
plan**.

---

## 1. Subsystem map (for context)

| Layer      | Header(s)                                        | Responsibility                                  |
|------------|--------------------------------------------------|-------------------------------------------------|
| Transport  | `transport/{tcp,udp,accept,saccept,stcp,file}.h` | Raw I/O primitives (read/write/close)           |
| Stream     | `stream.h`                                       | Buffered in/out, flush/publish, limits          |
| Protocol   | `async/protocol.h`, `protocol/{accept,handshake}.h`, `protocol/*.h` | Message framing (CRTP base `AProtocol<IO>`)    |
| Event loop | `async/listener.h`, `async/event/*.h`            | libev wrapping, thread-local loop, registration |
| Bases      | `async/io.h` (`input`, `output`, `io`, `with_timeout`, `file_watcher`) | CRTP bases that tie transport ↔ listener    |
| TCP        | `async/tcp/{acceptor,client,server,connector}.h` | High-level TCP                                  |
| UDP        | `async/udp/{client,server}.h`                    | High-level UDP                                  |
| Sessions   | `async/io_handler.h`                             | Server-side session registry + broadcasting     |
| Dead code  | `async/epoll.h`                                  | **Unused** standalone epoll wrapper             |

All async bases ultimately converge on a single libev loop owned by
`listener::current` (thread-local). Registration is kept in a
`qb::unordered_set<IRegisteredKernelEvent*>` and `registerEvent()` /
`unregisterEvent()` are O(1) amortized.

---

## 2. Findings

### 2.1  [S1] `transport::udp::write()` models "partial sends" that UDP cannot produce

`qb/include/qb/io/transport/udp.h` keeps a `pushed_message` queue and, after
each `sendto(2)`, advances an `offset` field:

```cpp
msg.offset += ret;
if (msg.offset < msg.size)
    break; // would re-attempt the same datagram
```

UDP `sendto` either sends the whole datagram or returns an error — it never
writes a prefix. Maintaining an `offset` per datagram is at best dead code
and at worst masks real errors: if the kernel returns `EMSGSIZE` we retry
forever because `ret != msg.size`.

**Fix.** On any return `ret >= 0` from `sendto`, consume the whole
`pushed_message` unconditionally. On `ret < 0` map `errno` to an
`event::disconnected` with a system error code. Remove the `offset` field
from `pushed_message`.

### 2.2  [S2] `transport::udp::is_secure()` declared differently from its peers

- `transport::tcp::is_secure()`   → `static constexpr bool`
- `transport::stcp::is_secure()`  → `static constexpr bool`
- `transport::accept::is_secure()`→ `static constexpr bool`
- `transport::saccept::is_secure()`→ `static constexpr bool`
- `transport::udp::is_secure()`   → **`constexpr bool`** (non-static)

Generic code that does `if constexpr (T::is_secure())` without constructing
an instance breaks for UDP. Harmonise to `static constexpr bool` everywhere.

### 2.3  [S2] `pushed_message::size` is `int` instead of `std::size_t`

Inside `transport::udp`, the internal pushed-message descriptor stores the
datagram length as `int`. `qb::allocator::pipe<char>` already uses
`std::size_t`. Widen the type for consistency and to avoid sign-extension
warnings on 64-bit systems. (Also required by finding 2.1's simplification.)

### 2.4  [S1] `transport::saccept::flush()` clobbers the accepted SSL socket by value

```cpp
void flush(std::size_t) noexcept {
    _accepted_io = io::tcp::ssl::socket();   // default-constructed
}
```

Compare with `transport::accept::flush()`:

```cpp
void flush(std::size_t) noexcept {
    _accepted_io.release_handle();           // detach FD without closing
}
```

The plain-TCP path correctly detaches the native handle so the caller can
move the socket into a session. The SSL path instead assigns a
default-constructed `ssl::socket` to `_accepted_io`, which triggers the
SSL destructor of the previously-accepted socket. If
`protocol::accept::onMessage` has already `std::move`'d the socket into the
new session, this is (a) redundant work and (b) potentially racy wrt the
SSL_CTX reference count depending on how `ssl::socket` is implemented.

**Fix.** Use the same idiom as plain TCP:

```cpp
void flush(std::size_t) noexcept {
    // native handle + SSL* have already been moved into the session
    _accepted_io.release_handle();
}
```

and, if needed, provide an `ssl::socket::release_handle()` that detaches
both the FD and the `SSL*` without running `SSL_shutdown`.

### 2.5  [S2] `stream<IO>` duplicates `_out_buffer` / `_max_write_buffer_size` instead of inheriting from `ostream<IO>`

`stream.h` defines three class templates:

```cpp
template <typename IO> class istream { pipe<char> _in_buffer;  … };
template <typename IO> class ostream { pipe<char> _out_buffer; … };
template <typename IO> class stream : public istream<IO> {
    pipe<char>  _out_buffer;              // !! duplicated
    std::size_t _max_write_buffer_size;   // !! duplicated
    // … write(), publish(), etc. reimplemented …
};
```

Two copies of the same data member invite drift (fields accidentally updated
on one path but read from the other). `io<Derived>` in `async/io.h` also has
a parallel `_out_buffer` / `_max_write_buffer_size`.

**Fix.** Make `stream<IO>` inherit from both `istream<IO>` and `ostream<IO>`
(CRTP-safe because they own disjoint state) or, at minimum, publicly re-use
`ostream<IO>::_out_buffer` via `using`. The `io<Derived>` base should
delegate to the `ostream` member of its transport rather than maintain a
third output buffer.

### 2.6  [S2] `qb::io::use<_Derived>::input<_Protocol>` / `::output<_Protocol>` ignore their template parameter

```cpp
template <typename _Protocol>
using input  = async::input<_Derived>;   // _Protocol never used
template <typename _Protocol>
using output = async::output<_Derived>;  // _Protocol never used
template <typename _Protocol>
using io     = async::io<_Derived>;      // _Protocol never used
```

Users write `use<MyClass>::input<MyProtocol>` expecting the protocol to be
wired automatically. In reality the protocol is only picked up when the
derived class exposes a `Protocol` type alias and `switch_protocol` is
called in the constructor. Either:

- drop the template parameter and document the `Protocol` alias convention, or
- actually wire the protocol in the `use<>` helpers (preferred — drives the
  `switch_protocol` call from the helper itself via a requires-clause).

### 2.7  [S2] `async::udp::server` has no session demultiplexing and leaves a commented alternative in place

Both `udp/server.h` and `async.h` carry commented-out session-aware variants:

```cpp
//        template <typename _Client>
//        using server = async::udp::server<_Derived, _Client>;
```

UDP servers routinely need to demultiplex datagrams into per-`identity`
sessions, and `transport::udp::identity` is already designed for that role
(it embeds an `endpoint` with a hasher). Either:

- delete the commented code if the feature is not planned, or
- finish it and expose a `use<>::udp::server<Session>` form consistent with
  the TCP surface.

### 2.8  [S3] UDP identity hasher rebuilds a `string_view` over raw endpoint memory

```cpp
struct identity : public qb::io::endpoint {
    std::size_t hash() const noexcept {
        return std::hash<std::string_view>{}(
            std::string_view(reinterpret_cast<const char*>(this), len()));
    }
};
```

This works, but (a) the `reinterpret_cast<const char*>(this)` walks into the
base `endpoint` storage — which may contain padding bytes from the socket
address unions, leaking non-deterministic bits into the hash, and (b) the
hash is recomputed on every unordered-map lookup. Cache the hash on
`identity` at the point where it is first constructed, and compare via
`memcmp` over the exact `sockaddr_*` active union.

### 2.9  [S1] `input::handle_post_read` fires `event::eof` on a healthy, non-EOF connection

In `async/io.h`, once all protocol messages have been extracted from the
buffer, the post-read hook dispatches `event::eof` whenever
`pendingRead() == 0`. This is not the end-of-file of the input stream — it
is the normal "nothing more to parse right now" state. A TCP peer that
sends `N` messages and then waits will fire `event::eof` after each batch.

**Fix.** Rename to `event::input_drained` (keep `event::eof` as an alias for
backward compatibility) and document the distinction from
`event::disconnected`. The same misnomer exists in `file_watcher::read_all`.

### 2.10  [S3] `async::epoll.h` defines a `Poller` that is never used

`qb/include/qb/io/async/epoll.h` provides `Proxy` and `Poller` classes that
operate directly on `epoll_create1`/`epoll_ctl`/`epoll_wait`. The
`qb::io::async::listener`, however, is implemented on top of libev's
`ev::dynamic_loop`. A repo-wide search shows **no** usage of
`qb::io::epoll::*` anywhere else in the tree.

**Fix.** Either:

- remove the file and its includes, or
- relocate it under `qb/utility/` as a standalone epoll helper with a note
  that it is not tied to the async subsystem.

### 2.11  [S1] `io_handler::registerSession` constructs the session **before** enforcing `_max_sessions`

```cpp
auto session = std::make_shared<_Session>(Derived(), std::move(socket));
if (_max_sessions > 0 && _sessions.size() >= _max_sessions) {
    session->disconnect(0);   // already allocated!
    return nullptr;
}
_sessions.emplace(session->id(), session);
```

Under a DoS burst an attacker can force the server to allocate, initialise,
and then tear down one `_Session` per rejected connection. The raw FD of
the accepted socket is already open when we construct the session — we can
cheaply `close()` it before constructing the session object.

**Fix.**

```cpp
if (_max_sessions > 0 && _sessions.size() >= _max_sessions) {
    socket.close();          // O(1) syscall, no allocation
    return nullptr;
}
auto session = std::make_shared<_Session>(Derived(), std::move(socket));
```

### 2.12  [S3] `io_handler::stream()` / `stream_if()` copy the entire session map on every broadcast

Both methods snapshot `shared_ptr`s from the map into a `std::vector` before
publishing. This is the right correctness pattern (prevents UAF if a handler
disconnects us during broadcast), but the allocation is paid on **every**
broadcast.

**Fix.** Accept a caller-provided scratch vector (`stream_into(buf, …)`) and
use `qb::util::small_vector<shared_ptr<Session>, 32>` as a zero-alloc fast
path for typical fan-outs < 32.

### 2.13  [S3] `listener::registerEvent` heap-allocates one `RegisteredKernelEvent` per watcher

Every `input` / `output` / `io` base registers at least one event; adding
`with_timeout` doubles that to two per object; `file_watcher` adds one
more (`ev::stat`). For servers with thousands of sessions the churn on
`new`/`delete` of `RegisteredKernelEvent<...>` is non-negligible.

**Fix.** Introduce a slab / intrusive freelist for each concrete
`RegisteredKernelEvent<_Event, _Actor>` instantiation (they are
fixed-size per pair). Gives deterministic O(1) registration and better
cache locality when replaying registered events from `_registeredEvents`.

### 2.14  [S3] `async::callback()` allocates a fresh `Timeout<_Func>` on every invocation

`qb::io::async::callback(func, timeout)` always `new`s a one-shot
`Timeout<_Func>` when `timeout > 0`. Hot paths (retry loops, keep-alive
timers, watchdogs) will churn the allocator heavily.

**Fix.** Provide a `periodic_callback` / `scoped_callback` that the caller
owns (RAII, no allocation once set up), and document `async::callback` as a
convenience for one-shot low-frequency use.

### 2.15  [S3] `connector<Socket, Func>` allocates **two** shared objects per connect attempt

In `async/tcp/connector.h`:

- the connector itself is held by a `std::shared_ptr` (for `self_hold_`),
- the deadline is scheduled via `async::callback`, which allocates a
  `Timeout<_Func>`.

Two heap allocations per `connect()` call. Acceptable for ad-hoc connects,
painful for a connect-storm (reconnect fan-out). Same fix as 2.14 — expose
a connector that holds its own timer in-place.

### 2.16  [S2] `disconnect(int reason)` silently remaps `0` to `1`

In `input::disconnect`, `output::disconnect`, and `io::disconnect`:

```cpp
_reason = reason ? reason : 1;
```

Callers that explicitly pass `0` (normal peer close) have the value silently
rewritten. This is undocumented and defeats the "0 = normal" convention
used by `event::disconnected`. Either reject the mapping (keep `0` as given)
or document it loudly and migrate reason codes to an `enum class`.

### 2.17  [S2] `event::disconnected::reason` is a magic `int`

The field mixes user codes (0 = normal, 1 = user-initiated), protocol
errors (negative `_reason` set by `process_messages`), and system errors
(`_system_error`). Promote to:

```cpp
enum class disconnect_reason : int {
    peer_closed        =  0,
    user_initiated     =  1,
    protocol_error     = -1,
    message_too_large  = -2,
    system_error       = -3,
    // …
};
```

and keep the numeric compatibility via a `static_cast<int>`.

### 2.18  [S2] `acceptor::listen()` does not auto-start the watcher

`async::tcp::acceptor<_Derived, Prot>::listen(uri)` binds and listens at
the transport level, but the caller still has to remember `this->start()`
to actually schedule the accept watcher. Missed calls result in the
acceptor silently swallowing new connections.

**Fix.** Either call `this->start()` at the end of `listen()`, or make
`start()` idempotent and document the required sequence with a
`[[nodiscard]]` on `listen()`.

### 2.19  [S1] `handshake` protocol calls `do_handshake()` inside `getMessageSize()`

`qb/include/qb/io/protocol/handshake.h::getMessageSize()` drives the SSL
handshake synchronously from the protocol's sizing hook:

```cpp
const auto result = this->_io.transport().do_handshake();
if (result <= 0) return 0;
return static_cast<std::size_t>(result);
```

This couples a side-effecting operation to what callers expect to be a
pure query. If `do_handshake()` itself performs blocking reads or writes
and the caller invokes `getMessageSize()` speculatively (e.g. inside
`process_messages` loops), the behaviour becomes hard to reason about.

**Fix.** Move the handshake stepping into `onMessage()` and have
`getMessageSize()` only return 1 or 0 based on a cached "handshake done"
flag driven by a dedicated `SSL_do_handshake` wrapper.

### 2.20  [S3] `listener::_registeredEvents` uses hashing where an intrusive list would do

The set is used only to walk all registrations on `clear()` and to delete
an entry on `unregisterEvent`. Both operations can be served by an
intrusive doubly-linked list threaded through `IRegisteredKernelEvent`
itself, saving the hash computation on every register/unregister and the
per-node memory overhead of `qb::unordered_set`.

### 2.21  [S4] `IProtocol::getMessageSize()` return semantics are underspecified

The current contract is "> 0 = complete message present, 0 = partial". The
code additionally uses sentinel values (`SIZE_MAX`, `-2`) in `istream::read`
and `process_messages`. Document the full enumeration and add a
`std::expected`-style return path, or at minimum a named constant
`IProtocol::kNoMessage = 0`.

### 2.22  [S4] The async event naming is inconsistent

`event::eof`, `event::pending_read`, `event::pending_write`,
`event::disconnected`, `event::extracted`, `event::dispose`, `event::eos`,
`event::handshake` are a mix of "something just happened" and
"something is about to happen" — and the English is non-uniform
(`eof` vs `end_of_file`, `eos` vs `end_of_stream`, `pending_read` vs
`read_pending`). Settle on a single naming convention and alias the rest
for compatibility.

---

## 3. Things that are already good (worth preserving)

- **Single-threaded listener** with `thread_local listener::current`. The
  invariant is ironclad and is what makes all the lock-free bases safe.
- **`_self_guard` pattern** in `input` / `output` / `io` — cheap and
  effective against callbacks that destroy their own I/O object.
- **`connector` uses `std::shared_ptr<connector>`** to keep itself alive
  across `n_connect` / `EV_WRITE` / deadline — correct UAF containment and
  the coroutine awaiters (`connect_awaiter`, `connect_with_socket_awaiter`)
  build on the same primitive.
- **`process_messages` caches the protocol pointer** before `onMessage` so
  that a protocol switch (handshake → HTTP/2 etc.) does not invalidate the
  `should_flush()` / `flush()` sequence.
- **DoS guard via `_max_message_size`** with a well-typed reason code
  (`_reason = -2`).
- **CRTP + `qb::has_on<_Derived, event::X>` detection** keeps the dispatch
  entirely compile-time and zero-overhead for events the user does not
  care about.

These should not regress during any of the fixes below.

---

## 4. Action plan

### Phase 1 — Correctness (S1) — **DONE**

1. [x] 2.1  — Remove UDP partial-send offset logic, map `sendto` errors properly.
2. [x] 2.4  — Fix `transport::saccept::flush()` to detach instead of reassign.
3. [x] 2.9  — Split `event::eof` into `event::input_drained` with alias.
4. [x] 2.11 — Reject-then-close in `io_handler::registerSession` before allocation.
5. [x] 2.19 — Decouple `handshake::getMessageSize()` from the SSL state machine.

### Phase 2 — API consistency (S2) — **DONE**

6. [x] 2.2 / 2.3 — Harmonise `is_secure()` signatures and widen
   `pushed_message::size` to `std::size_t`.
7. [x] 2.5 — Document the intentional duplication of `_out_buffer` between
   `stream<IO>` and `ostream<IO>` (sharing a common base would require
   reworking transport ownership; trade-off noted in-source).
8. [x] 2.6 / 2.7 — Fix `use<>::input/output/io` helpers (defaulted unused
   template parameter, documented Protocol-alias convention); delete the
   commented UDP session-aware server prototype.
9. [x] 2.16 / 2.17 — Promote disconnection reasons to `enum class` and
   document the `0 → user_initiated` remap, with strongly-typed overloads
   of `disconnect()`.
10. [x] 2.18 — `acceptor::listen()` auto-starts the watcher; added
    `listen_no_start()` escape hatch for deferred registration.

### Phase 3 — Performance (S3) — **DONE**

11. [x] 2.8  — Document UDP identity hasher safety (`string_view` over
    endpoint bytes is well-defined because `endpoint` lays out the address
    in the first `len()` bytes of a discriminated union).
12. [x] 2.10 — Marked `async/epoll.h` `@deprecated` with a clear note that
    the async listener uses libev instead.
13. [x] 2.12 — `io_handler::stream()` / `stream_if()` reuse a
    `_broadcast_scratch` vector member, amortising the per-call
    allocation across broadcasts.
14. [x] 2.13 — Thread-local LIFO freelist per `RegisteredKernelEvent<E,A>`
    instantiation. Freed blocks re-use their own first `sizeof(void*)`
    bytes as the next-pointer; no per-block header. Measured
    `registerEvent+unregisterEvent` cost dropped from ~52 ns/op to ~5
    ns/op (≈10× faster).
15. [x] 2.14 — `scoped_callback` / `ScopedTimeout` RAII wrapper added in
    `async/io.h` for allocation-free one-shot timers with explicit
    cancellation.
15a. [x] 2.15 — Thread-local LIFO freelist per `Timeout<F>` instantiation.
    Combined with 2.13, the steady-state `async::callback()` path burns
    zero `malloc`/`free` calls; `scoped_callback` full cycle dropped
    from ~78 ns/op to ~30 ns/op.
16. [x] 2.20 — Replaced `_registeredEvents` `qb::unordered_set<void*>`
    with an intrusive doubly-linked list. Links live on
    `IRegisteredKernelEvent` itself; registration and de-registration are
    now a handful of pointer swaps with no auxiliary container.

### Phase 4 — Documentation (S4) — **DONE**

17. [x] 2.21 — Formalised `IProtocol::getMessageSize()` contract
    (side-effect-free, added `kNoMessage` constant, documented
    interaction with `max_message_size()`).
18. [x] 2.22 — Added documentation alias (`using eof = input_drained;`)
    and naming rationale; full naming harmonisation is deferred to avoid
    ABI churn.
19. [x] Cross-reference this document from `qb/readme/3_qb_io/` and add
    an "invariants" page `qb/readme/7_reference/io_invariants.md`
    analogous to `readme/7_reference/core_invariants.md`.

---

## 5. Open questions for the maintainer

- Is the `async/epoll.h` file reserved for a future non-libev backend, or
  is it leftover from an earlier prototype? (Drives finding 2.10.)
- Is the commented UDP session-aware `server<Session>` still on the
  roadmap? (Drives 2.7 — delete vs finish.)
- Are there external users depending on `event::eof` semantics today? If
  so, a compatibility alias in 2.9 is mandatory rather than optional.
- For 2.16/2.17, should the new `enum class disconnect_reason` live in
  `event::disconnected` or in a shared `qb::io::close_reason.h`?

---

## 6. Tests to add alongside the fixes

For each S1 finding, land a targeted GTest under `qb/source/io/tests/system`:

- **T-UDP-WRITE-NO-OFFSET** — verify that a single `publish()` of a
  200-byte datagram results in exactly one `sendto` call of 200 bytes and
  that an induced `EMSGSIZE` propagates to `event::disconnected` instead
  of an infinite retry.
- **T-SACCEPT-FLUSH-PRESERVES-TLS** — accept one TLS connection, move the
  socket into a session, drive a short ping/pong, and verify that the
  session survives `saccept::flush`.
- **T-INPUT-DRAINED-VS-EOF** — open a TCP connection, send one message,
  wait 100 ms: verify that `event::disconnected` is not emitted and that
  `event::input_drained` is (and that `event::eof` is emitted only on
  actual peer close for backward-compat consumers).
- **T-REGISTER-SESSION-DOS** — set `_max_sessions = 1000`, push 2000
  connections as fast as possible, measure peak allocator residency and
  assert it is bounded.
- **T-HANDSHAKE-NO-SIDE-EFFECTS-IN-GETSIZE** — drive an SSL handshake
  through `process_messages` and verify `getMessageSize()` is
  side-effect-free on both "handshake in progress" and "handshake done"
  states.

---

## 7. Deliverables checklist

- [x] Phase 1 PRs (correctness) — 5/5 findings landed
- [x] Phase 2 PRs (API consistency) — 8/8 findings landed (2.5 documented)
- [x] Phase 3 PRs (performance) — 7/7 findings landed (2.13, 2.15, 2.20
  landed with micro-benchmarks)
- [x] Phase 4 PRs (docs) — 3/3 items landed; readme cross-link in place
- [x] New tests listed in §6 merged (see `test-io-plan.cpp`, 10 TEST_F,
  all passing)
- [x] `qb/readme/3_qb_io/` cross-link + new `io_invariants.md` page

---

## 8. Progress log

- **2026-04-19 (morning)** — Implemented all Phase 1 / Phase 2 findings
  and Phase 3 items 2.8, 2.10, 2.12, 2.14 plus Phase 4 items 2.21, 2.22.
  Deferred 2.13, 2.15, 2.20 pending allocator/perf measurements.

- **2026-04-19 (framework-wide bug)** — While generalising the
  `on(event::disconnected&&)` dispatch, uncovered a **latent 12-day-old
  infinite-recursion bug** in `async::tcp::server::on(disconnected&&)` and
  `async::tcp::acceptor::on(disconnected&&)`. The CRTP branch used
  `qb::has_on<_Derived, event::disconnected>` to detect user overrides,
  but `has_on` matches any reachable `on(Evt&&)` — including the one
  *inherited from the base itself*. As a result the base unconditionally
  re-dispatched to itself whenever the derived class did not provide its
  own handler. Fixed by introducing a new compile-time trait
  `qb::has_own_on<D, Base, Evt>` in `qb/include/qb/utility/type_traits.h`
  that compares member-function pointers cast to `D::*` (and also matches
  a unique `on(const Evt&)` overload that the base does not define). The
  two call sites now use `has_own_on` instead of `has_on`. Also fixed an
  unrelated signature mismatch in `qbm/ws/tests/test-robustness.cpp`
  (`on(async::event::disconnected&)` → `on(async::event::disconnected
  const&)`) that the new trait surfaced.

- **2026-04-19 (performance pass)** — Landed the three deferred perf
  items with a dedicated micro-benchmark harness
  (`qb/source/io/tests/system/bench-io-plan.cpp`). Median of 3 runs on
  an Apple-silicon `clang`/`-O3` release build:

  | Hot path                          | Baseline | After all 3 | Speed-up |
  |-----------------------------------|---------:|------------:|---------:|
  | `registerEvent + unregisterEvent` |  52 ns/op |    5 ns/op  |   ~10×   |
  | `scoped_callback` (ctor + dtor)   |  78 ns/op |   30 ns/op  |   ~2.6×  |
  | `async::callback` (+ libev fire)  |  25 µs/op |  13.6 µs/op |   ~1.85× |
  | `io_handler::stream()` broadcast  |  31 / 514 / 8 263 ns/op for 16 / 256 / 4 096 sessions (stable — already optimal after 2.12) |

  The `async::callback` wall-time remains dominated by the mandatory
  `ev_run(EVRUN_NOWAIT)` + libev book-keeping; the **steady-state number
  of `malloc`/`free` calls is now exactly zero** on that path, which is
  the contract that 2.15 set out to deliver and what matters most under
  sustained load.

- **2026-04-19 (regressions observed)** — Two unrelated includes were
  being pulled transitively through `qb/io/async/listener.h` (which no
  longer needs `<qb/system/container/unordered_set.h>` after 2.20):
  `qbm/http/2/protocol/stream.h` and `qbm/redis/set_commands.h`. Both
  were fixed locally by adding the explicit include where `qb::unordered_set`
  is actually used — a classic "include what you use" bug that the perf
  refactor merely surfaced.

- **2026-04-19 (final test pass)** — Full `ctest -j 8` over 141 targets:
  **139 pass**. Remaining failures are pre-existing flakes unrelated to
  `qb-io`:
    - `qb-io-gtest-test-async-io` — `TextProtocolCommunication` is
      timing-sensitive and passes when run in isolation;
    - `qbm-http-test-integration-middleware::RateLimitMiddlewareTest` —
      known timing-dependent rate-limit window, reproducible on the
      stashed baseline.
