@page reference_io_invariants QB-IO: Async, Lifecycle & Allocation Invariants
@brief Consolidated reference of the thread-ownership, lifetime and
allocation invariants upheld by `qb-io`. Read this once before you write
protocol, transport or session code.

# QB-IO — Async, Lifecycle & Allocation Invariants

This page is the **single source of truth** for the invariants the
asynchronous stack assumes. Every header under `qb/include/qb/io/**` and
every source under `qb/source/io/src/**` is written with these rules in
mind. If a change breaks one of these invariants, the whole `qb-io`
model breaks silently (races, double frees, or worse: "it works until
production").

Each rule links back to the finding number in
[`qb/QB_IO_PLAN.md`](../../QB_IO_PLAN.md) where the invariant was
formalised or hardened.

---

## 1. Thread model — one `listener` per `VirtualCore`

- Every `VirtualCore` owns **exactly one** `qb::io::async::listener`.
  The listener is reachable through the `thread_local` slot
  `qb::io::async::listener::current`, installed on the worker thread
  before any I/O object is constructed on that core.
- Every `async::input<>`, `async::output<>`, `async::io<>`,
  `async::tcp::client`, `async::tcp::server`, `async::udp::client`,
  `async::udp::server`, `Timeout<F>`, `ScopedTimeout<F>`,
  `file_watcher<>` and `directory_watcher<>` you allocate is bound at
  construction to the `listener::current` of the thread that runs the
  constructor. **It MUST be destroyed on the same thread** — the libev
  watcher it registers is not cross-thread safe.
- The listener itself is **not** locked. All hot paths assume
  single-threaded access. Anything that needs to speak to an I/O object
  from another thread goes through the actor mailbox
  (`qb::Actor::push<>` / `broadcast<>`) — never through a raw pointer.

Consequence: `qb-io` objects are **not `std::thread`-safe**. They are
`VirtualCore`-safe, which is a strictly stronger and strictly
single-threaded notion.

---

## 2. `IRegisteredKernelEvent` ownership & lifetime

- Every libev watcher created by `listener::registerEvent<Event>()` is
  backed by a freshly-allocated `RegisteredKernelEvent<Event, Actor>`.
  Those allocations live in a **thread-local LIFO freelist** per
  `(Event, Actor)` instantiation (finding 2.13). Steady-state
  `registerEvent` / `unregisterEvent` does **zero `malloc`/`free`** after
  the first handful of instances.
- The ownership chain is strict: the listener owns the
  `RegisteredKernelEvent *`, the `RegisteredKernelEvent` owns the libev
  watcher (`_event`), and the libev watcher carries a back-pointer
  (`_interface`) used by `unregisterEvent` to release storage.
- `IRegisteredKernelEvent *` is threaded on an **intrusive doubly-linked
  list** hanging off the listener (finding 2.20). Links live on the
  interface itself (`_list_prev`, `_list_next`); the listener is the
  only class allowed to touch them (friended). Registration and
  de-registration are therefore three pointer swaps with no hash
  computation.
- **Double-unregister is idempotent**: calling
  `listener::unregisterEvent(p)` on a pointer that is no longer linked
  is a no-op (guarded by a linked-state check in `_unlink`), and
  `delete`ing the pointer is skipped. This is a safety net — do **not**
  rely on it in application code.

---

## 3. `async::callback` and `Timeout<F>` allocations

- `async::callback(func, timeout > 0)` creates a self-deleting
  `Timeout<F>` on the heap (one allocation) which in turn registers a
  libev timer (one `RegisteredKernelEvent` allocation). Both
  allocations now hit **thread-local freelists** (findings 2.13 and
  2.15). Steady-state burst traffic through `async::callback` performs
  **exactly zero** `malloc`/`free` calls once the pools have warmed up.
- `Timeout<F>::on(event::timer const&)` is the only place that calls
  `delete this` on the timer. Any other code path that wants owned,
  RAII-cancellable timers **must** use `async::scoped_callback(...)` /
  `ScopedTimeout<F>` (finding 2.14): they are pure stack/`unique_ptr`
  citizens and do not participate in the self-delete dance.
- The pool lifetime is strictly "thread-local, never drained". On
  thread exit the OS reclaims the pooled blocks. Draining in a TLS
  destructor would race with late `delete this` calls from
  already-fired timers whose loop iteration outlives the listener TLS,
  so we explicitly do not attempt it.

---

## 4. CRTP dispatch & user overrides

- The dispatch between `async::tcp::server`, `async::tcp::acceptor`,
  `async::input/output/io` and the user's `_Derived` class is
  **compile-time** via CRTP + a set of trait predicates
  (`qb::has_on<T, Event>`, `qb::has_getMessageSize<T>`, etc.).
- For events that the framework dispatches as rvalues (most notably
  `event::disconnected`), **user overrides must be declared with a
  compatible signature**: `on(event::X&&)` or `on(const event::X&)`.
  A plain `on(event::X&)` (non-const lvalue reference) will fail to
  bind — the framework does not hand out lvalues for these rvalue-only
  events (finding documented alongside the trait work).
- The trait `qb::has_own_on<Derived, Base, Event>` (added alongside
  the 2026-04-19 CRTP bug-fix) is the correct way to detect whether a
  derived class **genuinely overrides** a particular `on(Event&&)`
  handler vs. merely inheriting the base's fallback. Using the
  coarser `qb::has_on` inside a CRTP base for dispatch is unsafe: it
  will match the inherited method and cause infinite recursion.

Do **not** use `qb::has_on` in a CRTP base to decide whether to
re-dispatch to the derived class. Use `qb::has_own_on` instead.

---

## 5. Session lifetime in `io_handler<Session>`

- `io_handler::registerSession` allocates a new `Session` and binds its
  socket — but only **after** the DoS guard (finding 2.11): session
  slots are checked, reserved and capped **before** allocation so that
  a flood of incoming connections cannot amplify into heap pressure.
- `io_handler::stream()` / `stream_if()` fan-out uses a persistent
  `_broadcast_scratch` vector (finding 2.12) so that broadcasting to
  the same N sessions N times costs O(N) reads, not O(N²)
  allocations.
- A session is destroyed in three situations:
  1. The peer closes → `event::disconnected` with a positive
     `disconnect_reason`.
  2. The framework detects a protocol violation or `max_message_size`
     overflow → `disconnect_reason::user_initiated` with
     `_reason = -2` (finding 2.16 / 2.17).
  3. The owner actor calls `disconnect()` explicitly.

In all three cases, the destruction happens **inside the listener
callback on the owning `VirtualCore`'s thread** — never from another
thread, never from a destructor of an unrelated object.

---

## 6. Protocol contract (`IProtocol`)

- `IProtocol::getMessageSize()` is **side-effect-free** (finding 2.21).
  It may be called multiple times per byte of input by the stream
  layer; implementations must not mutate parser state, not decrement
  a credit counter, not emit logs that have semantic meaning.
- `getMessageSize()` returns `IProtocol::kNoMessage` when more input
  is needed, or a positive frame size otherwise.
- `max_message_size()` is a hard cap enforced by the stream layer
  **before** calling the protocol's `on_message()` — a protocol that
  returns a size larger than its own cap will short-circuit to
  `disconnected` with `_reason = -2` and will not be re-invoked on the
  session.
- `event::input_drained` (finding 2.9) signals "protocol parser has
  consumed every buffered byte it could" and is distinct from EOF.
  The legacy alias `event::eof = event::input_drained` is preserved
  for backward compatibility (finding 2.22) but new code should use
  `event::input_drained`.

---

## 7. Transport-level invariants

- `transport::udp::publish()` writes **one datagram per call**,
  atomic at the kernel level. Partial-write retries have been removed
  (finding 2.1): `sendto` errors propagate up as
  `event::disconnected` rather than infinite retries.
- `transport::saccept::flush()` **detaches** the handshake socket into
  the new session instead of reassigning its descriptor (finding 2.4).
  An in-flight TLS handshake survives the detach.
- `transport::stcp` uses a cached `do_handshake` result so the
  protocol-size query (`handshake::getMessageSize()`) is cheap and
  side-effect-free (finding 2.19 + 2.21).

---

## 8. Bench harness invariants (internal)

For anyone extending `qb/source/io/tests/system/bench-io-plan.cpp`:

- Benches run against `listener::current` on the main thread — always
  call `qb::io::async::init()` first and `listener::current.clear()`
  at the end of `main` to drain any dangling watchers.
- Each bench loop must be net-zero on the listener: for every
  `registerEvent` / `new Timeout<F>` you must call the matching
  `unregisterEvent` / let the timer fire, otherwise the next bench in
  the same process will see polluted state.
- Never compare bench numbers taken on different build types or with
  different sanitisers; the freelists and intrusive list are sensitive
  to ASan / TSan quarantine behaviour.

---

## Appendix — How to check invariants when something breaks

1. **Is the crash on the listener's thread?** `std::this_thread::get_id()`
   inside the callback → compare to the owning `VirtualCore`'s id.
2. **Is a watcher leaked?** `listener::current.size()` at shutdown
   should be 0. If not, look for an I/O object destroyed on the wrong
   thread or a protocol that forgot to unregister.
3. **Is the freelist corrupted?** Temporarily remove the class-level
   `operator new`/`operator delete` in
   `RegisteredKernelEvent<E,A>` / `Timeout<F>`; if the crash goes
   away, a double-free or wrong-size delete is hitting the pool.
4. **Is `on(Event&&)` recursing?** Check with
   `qb::has_own_on<Derived, Base, Event>` that the derived class
   actually provides the override, and that the signature is
   rvalue-ref (or `const&`), never non-const lvalue.

---

*Document companion to `qb/QB_IO_PLAN.md`. Last updated: 2026-04-19.*
