# The asynchronous I/O model

> **Audience:** Adopter · **Status:** stable · **Verified-against:** qb 2.0.0 (C++20 default, C++23 supported)

How qb runs network and filesystem work without blocking a thread: a single-threaded, libev-backed event loop that dispatches readiness to `on()` handlers, plus a coroutine layer for code that prefers `co_await` over callbacks.

**Prerequisites:** [The actor model](./actor_model.md), [The event system](./event_system.md) — **See also:** [qb-io: the asynchronous system](../3_qb_io/async_system.md), [qb-io: coroutines](../3_qb_io/coroutines.md), [qb-io: transports](../3_qb_io/transports.md)

## Summary

`qb-io` is a non-blocking I/O runtime. Instead of dedicating a thread to each connection and letting it wait for data, qb registers interest in an operation with the operating system, returns immediately, and resumes work only when the kernel reports readiness. One thread can therefore drive many concurrent connections.

The runtime exposes two complementary programming models over the same loop:

- **Readiness callbacks** — you derive from a CRTP base (`qb::io::async::io`, `input`, `output`, `with_timeout`, …) and implement `on(...)` handlers. The loop calls them when the watched file descriptor or timer becomes ready.
- **Coroutines** — you write linear `co_await` code (`qb::io::async::sleep`, `wait_readable`, `task<T>`, channels, …) that suspends and resumes on the same loop, with no explicit callbacks.

Both models run on the thread-local `qb::io::async::listener`. Under `qb-core`, every `VirtualCore` owns one listener and drives it as part of its actor cycle, so actor code never calls the loop directly.

## Concepts

### The non-blocking principle

A blocking read parks the calling thread until data arrives. Under concurrency this wastes threads and CPU. qb inverts the flow:

1. **Initiate.** Code requests an operation — for example, `start()` on an I/O component begins watching a socket for read readiness.
2. **Register interest.** The runtime registers a libev watcher with the thread-local listener, which in turn arms the kernel readiness primitive (epoll on Linux, kqueue on the BSDs and macOS, wepoll/IOCP on Windows — selected by libev's `EVFLAG_AUTO`).
3. **Return immediately.** The calling thread does not wait. It returns to the event loop and processes other events.
4. **Notify.** When the operation can proceed without blocking, the kernel notifies the listener.
5. **Dispatch.** The listener routes the notification to the registered handler — an `on(...)` method on your component, or the resumption of a suspended coroutine.

The unit of "work" is therefore an `on()` handler invocation or a coroutine resumption, not a thread.

### The event loop: `qb::io::async::listener`

The loop is owned by `qb::io::async::listener` (`qb/io/async/listener.h`).

- **Engine.** It wraps [libev](http://software.schmorp.de/pkg/libev.html). The loop is constructed with `EVFLAG_AUTO`, so libev picks the best backend for the platform.
- **Thread-local.** Each thread has exactly one listener, reachable as `qb::io::async::listener::current` (a `thread_local static` member). I/O objects bind to the listener of the thread that constructs them and must not be shared across threads. Under `qb-core`, each `VirtualCore` has its own listener.
- **Watcher registry.** The listener registers and unregisters libev watchers (`registerEvent` / `unregisterEvent`), tracks them in an intrusive list, and uses a per-watcher-type thread-local freelist so a steady-state workload registers and frees watchers without allocating.
- **Dispatch.** When a watcher fires, the listener updates the wrapper's triggered-flags field and calls the handler's `on(SpecificEvent&)` method. If the handler exposes `is_alive()`, the listener checks it first and skips the call on a dead object.

> **Windows note.** libev's epoll backend on Windows is wepoll (IOCP-based). Keep the whole loop lifecycle — registration, `run()`, and teardown — on a single thread per listener; do not close a loop or its handle from another thread while it is running.

### Driving the loop: who calls `run()`?

The loop only makes progress while it is being run.

- **Under `qb-core` (actors).** You do **not** call the loop manually. Each `VirtualCore` runs its listener as part of its main cycle, interleaving actor message processing with I/O dispatch. Components and coroutines created from actor code are serviced automatically.
- **Standalone `qb-io`.** When you use `qb-io` without the actor engine, you drive the loop yourself. Call `qb::io::async::init()` once per thread to make the thread-local listener available (it is a no-op safety hook; the listener self-initializes), then pump events with one of:

| Free function | Behavior |
| --- | --- |
| `run(int flag = 0)` | Runs `listener::current.run(flag)`; returns the number of events invoked. `flag` is a libev run flag. |
| `run_once()` | Equivalent to `run(EVRUN_ONCE)`: waits for and processes one event block. |
| `run_until(bool const &status)` | Repeatedly runs `run(EVRUN_NOWAIT)` while `status` is `true`. |
| `break_parent()` | Requests the current thread's loop to break out of its `run()` cycle. |

With the default `flag == 0`, libev blocks until `break_parent()` is called or no active watchers remain. `EVRUN_NOWAIT` checks once and returns; `EVRUN_ONCE` waits for and processes one block of events.

These functions throw `std::logic_error` if called from inside a coroutine or actor handler that is already executing under the scheduler's ready-drain — you must not re-enter the loop while it is dispatching. (See the pitfalls below for `run_once` and timerfd.)

> Both of these — the readiness model and the loop-driving functions — are owned by the qb-io reference. This page introduces them; [qb-io: the asynchronous system](../3_qb_io/async_system.md) is the full treatment.

### The CRTP I/O bases

The callback model is built from a small family of CRTP templates in `qb/io/async/io.h`. Each binds one libev watcher to your derived class and dispatches readiness to your `on()` methods. The shared root is:

```cpp
template <typename _Derived, typename _EV_EVENT>
class base; // registers _EV_EVENT with listener::current on construct, unregisters on destruct
```

Built on `base`:

| Class | Watcher | Role |
| --- | --- | --- |
| `qb::io::async::input<_Derived>` | `event::io` | Read side: drives non-blocking reads through a protocol. |
| `qb::io::async::output<_Derived>` | `event::io` | Write side: buffers outgoing data and flushes on write readiness. |
| `qb::io::async::io<_Derived>` | `event::io` (single) | Bidirectional: combines `input` and `output` over one watcher. |
| `qb::io::async::with_timeout<_Derived>` | `event::timer` | Inactivity / interval timer (see below). |
| `qb::io::async::file_watcher<_Derived>`, `directory_watcher<_Derived>` | `event::file` (`ev::stat`) | Filesystem attribute change watching. |

You rarely instantiate these directly. The `qb::io::use<_Derived>` helper (`qb/io/async.h`) exposes them through declarative aliases — `use<T>::tcp::client<>`, `use<T>::tcp::server<S>`, `use<T>::udp::client`, `use<T>::io<>`, and so on — which wire the correct transport and base together. The transports themselves are documented in [qb-io: transports](../3_qb_io/transports.md).

#### Lifecycle of an `io` component

`qb::io::async::io<_Derived>` is the workhorse. A derived class supplies a transport (a socket) and a protocol (the wire framing), then drives the component:

- `start()` — sets the transport non-blocking and arms the watcher for `EV_READ`; resets the disconnection reason and system-error state. Call it after `connect()` or `accept()`.
- `publish(args...)` / `operator<<` — append to the output buffer and arm `EV_WRITE`; the loop flushes when the socket is writable. `publish` enforces the configured maximum write-buffer size and disconnects with `buffer_overflow` if it is exceeded.
- `disconnect(int reason = 1)` / `disconnect(event::disconnect_reason)` — request a graceful shutdown. The reason is recorded and the loop runs the cleanup path on the next dispatch.
- `close_after_deliver()` — flush all pending output, then disconnect.
- `stop()` — pause the watcher without running disconnection cleanup, so the component can be restarted with `start()`.

When a read or write fails, or `disconnect()` is requested, the component calls `dispose()` exactly once. `dispose()` invokes `on(event::disconnected&&)` (carrying the reason and, on error, the system errno) if the derived class implements it, then either notifies the owning server (for accepted sessions) or stops the watcher and fires `on(event::dispose&&)` (for standalone clients).

### Timers: `with_timeout`

`qb::io::async::with_timeout<_Derived>` adds an `event::timer` to a class for inactivity deadlines and recurring ticks.

- **Construct.** `with_timeout(qb::duration timeout = std::chrono::seconds(3))` sets and starts the initial timer. A timeout of `0` or less leaves it disabled.
- **Handle.** Implement `on(qb::io::async::event::timer const&)`; the base calls it when the deadline elapses with no intervening activity.
- **Reset.** `updateTimeout()` records the current loop time as the last activity, deferring the deadline.
- **Reconfigure.** `setTimeout(qb::duration)` changes the interval and restarts the timer; pass `qb::duration::zero()` to disable it.
- **Inspect.** `getTimeout()` returns the configured interval as a `qb::duration`.

All durations are `qb::duration` (a `std::chrono::nanoseconds` span); the literals from `<chrono>` work directly.

```cpp
// src: qb/source/io/tests/system/test-async-io.cpp (adapted from TimerHandler)
#include <qb/io/async.h>
#include <chrono>

using namespace std::chrono_literals;

// Fires on(event::timer) once the configured interval elapses without an
// updateTimeout() call. updateTimeout() defers the deadline; setTimeout(0)
// disables it.
class InactivityWatch : public qb::io::async::with_timeout<InactivityWatch> {
public:
    explicit InactivityWatch(qb::duration timeout = 60s)
        : with_timeout(timeout) {}

    void on_activity() { updateTimeout(); }   // call on each client interaction

    void on(qb::io::async::event::timer const &) {
        // No activity within the window — react (close the session, log, …).
    }
};
```

### Scheduling a one-shot callback: `qb::io::async::callback`

`qb::io::async::callback` (`qb/io/async/io.h`) schedules a callable on the loop of the *calling thread*. There are two overloads, and their behavior differs in a way worth stating precisely:

```cpp
template <typename _Func>
void callback(_Func &&func);                               // (1) runs func() inline, now

template <typename _Func, typename Rep, typename Period>
void callback(_Func &&func, std::chrono::duration<Rep, Period> timeout); // (2)
```

- **Overload (1), no delay.** `func()` runs **immediately and synchronously**, on the calling stack. It is not deferred to a later loop iteration.
- **Overload (2), with a delay.** If `timeout <= 0`, `func()` runs immediately and synchronously, as in (1). If `timeout > 0`, the call heap-allocates a self-deleting timer that fires `func()` after the delay on the next eligible loop iteration, then deletes itself. The loop's monotonic clock is refreshed before arming, so the requested delay holds even if the calling thread was idle outside the loop.

The self-deleting timer uses a thread-local freelist, so a steady stream of delayed callbacks reuses storage rather than churning the allocator.

```cpp
// src: qb/source/io/tests/system/test-async-io.cpp (CallbackScheduledExecution)
#include <qb/io/async.h>
#include <chrono>

using namespace std::chrono_literals;

// Standalone qb-io: drive the loop yourself.
qb::io::async::init();

bool fired = false;
// timeout > 0  ->  deferred; runs after ~250 ms on a later loop iteration.
qb::io::async::callback([&fired] { fired = true; }, 250ms);

while (!fired)
    qb::io::async::run(EVRUN_ONCE); // under qb-core the VirtualCore pumps this for you
```

> **Inside an actor.** Prefer having the callback send an event back to the actor rather than mutating actor state directly, and guard against the actor having been destroyed before a *delayed* callback fires. Because the no-delay overload runs synchronously, the actor is necessarily still alive at that point; the lifetime concern applies only to the delayed overload. See [Actors and asynchronous I/O](../5_core_io_integration/README.md) for the actor-side patterns.

For a cancellable, caller-owned timer that does not self-delete, use `qb::io::async::scoped_callback`, which returns a `std::unique_ptr` to a `ScopedTimeout`; destroying or calling `cancel()` on it stops a pending callback. This is documented with the timer family in [qb-io: the asynchronous system](../3_qb_io/async_system.md).

### Coroutines over the same loop

The callback model is not the only option. `qb-io` ships a C++20/23 coroutine runtime layered on the same listener (`qb/io/async/coroutine/`). Coroutine code reads top-to-bottom and suspends at `co_await` points instead of registering callbacks:

```cpp
// src: qb/include/qb/io/async/coroutine/utils.h (doc example)
#include <qb/io/async.h>
#include <chrono>

using namespace std::chrono_literals;

qb::io::async::task<void> poll_once(int fd) {
    co_await qb::io::async::wait_readable(fd); // suspend until fd has data
    // ... read here ...
    co_await qb::io::async::sleep(100ms);      // suspend without burning the thread
    co_return;
}
```

Key entry points (`qb::io::async`):

- `sleep(qb::duration)` — suspend the coroutine for a duration. A non-positive duration is a cooperative yield (re-enqueued at the back of the ready queue, no kernel timer).
- `wait_readable(fd)` / `wait_writable(fd)` / `wait_for_io(fd, events)` — suspend until the descriptor is ready.
- `task<T>` — the move-only coroutine return type.
- `coro_scheduler()` — the current thread's `CoroutineScheduler`; `coro_scheduler().spawn(std::move(t))` runs a `task` detached.
- `run_for(qb::duration)` — drive the loop and the coroutine scheduler for a bounded window (standalone use).
- `run_sync(awaitable)` — block the current thread until an awaitable completes, returning its result; for bridging synchronous code (test setup, `main`) to coroutine APIs.

The scheduler is cooperative and single-threaded per listener: exactly one coroutine runs at a time, and another can only start at a `co_await` suspension point. That invariant gives the coroutine sync primitives (`async_mutex`, `semaphore`, channels) mutual exclusion without OS locks. The full coroutine surface — generators, channels, combinators (`when_all`/`when_any`), cancellation, structured concurrency — is covered in [qb-io: coroutines](../3_qb_io/coroutines.md).

Callbacks and coroutines coexist on one loop; `listener::run()` drains ready libev watchers and then runs ready coroutines in the same cycle. Choose readiness callbacks for protocol-shaped, stream-of-messages I/O, and coroutines for sequential request/response logic that would otherwise fragment across callbacks.

### Asynchronous I/O event types

Components built on the CRTP bases receive I/O state changes as `qb::io::async::event::*` structs delivered to matching `on(EventType&&)` handlers. The standard set (`qb/io/async/event/`):

| Event | Meaning |
| --- | --- |
| `event::io` | Low-level read/write readiness on a watched descriptor (handled internally by the bases). |
| `event::timer` | A `with_timeout` deadline elapsed. |
| `event::file` | A watched file or directory's attributes changed (`file_watcher` / `directory_watcher`). |
| `event::signal` | An OS signal was delivered. |
| `event::disconnected` | The connection closed or was lost; carries a `reason` and, on error, a `std::error_code`. |
| `event::input_drained` (alias `event::eof`) | The protocol parsed every complete message and the input buffer is now empty (`pendingRead() == 0`). Not an end-of-stream notification — the connection may still be open. |
| `event::eos` | All buffered output was written to the transport. |
| `event::pending_read` / `event::pending_write` | Bytes remain in the input / output buffer after the current pass (`pending_read` carries the leftover byte count). |
| `event::dispose` | Final cleanup hook before a standalone component is torn down. |
| `event::handshake` | A TLS/transport handshake completed. |

Disconnection reasons are typed by `qb::io::async::event::disconnect_reason` (`qb/io/async/event/disconnected.h`), whose underlying type is `int` so raw integer codes remain interchangeable:

| Reason | Value | Cause |
| --- | --- | --- |
| `peer_closed` | `0` | Normal shutdown — peer closed, or we closed cleanly. |
| `user_initiated` | `1` | Explicit `disconnect()` from user code (the default). |
| `protocol_error` | `-1` | The protocol marked itself `not_ok()`. |
| `message_too_large` | `-2` | DoS guard: an incoming message exceeded `max_message_size()`. |
| `buffer_overflow` | `-3` | DoS guard: a read or write buffer exceeded its configured maximum. |

Positive codes above `1` are reserved for application use. (For example, `qbm-http` defines its own positive reason codes.)

## Pitfalls

- **`callback(func)` with no delay is synchronous.** The single-argument overload, and the two-argument overload with a non-positive timeout, run `func()` immediately on the calling stack — they do not defer to a later loop iteration. Only `callback(func, positive_timeout)` defers. Do not rely on the no-delay form to "yield" control back to the loop.
- **One listener per thread; never share I/O objects.** A component, timer, or coroutine binds to the listener of the thread that created it. Touching it from another thread corrupts the loop. To cross threads, send an event (under `qb-core`) or use a per-thread listener.
- **Do not re-enter the loop from a handler.** Calling `run()`, `run_once()`, or `run_until()` from inside an `on()` handler or coroutine that is already running under the scheduler throws `std::logic_error`. Let the current dispatch return.
- **`run_once()` can block on idle `ev_io`-only loops when timerfd is enabled.** qb's bundled libev disables timerfd by default (the `QB_LIBEV_USE_TIMERFD` CMake option defaults to `OFF` in `qb/modules/ev/CMakeLists.txt`). If you build with `-DQB_LIBEV_USE_TIMERFD=ON`, then when there are only `ev_io` watchers and no heap timers, `EVRUN_ONCE` can block for libev's internal maximum wait time. In pump loops prefer `run_until(status)` or `run(EVRUN_NOWAIT)` regardless.
- **Guard actor state in *delayed* callbacks.** A delayed `callback` (or a coroutine that resumes after a suspension) may outlive the actor that scheduled it. Capture the actor's id by value and push an event back, or check `is_alive()`, rather than dereferencing a captured `this`. The synchronous no-delay path is exempt because the actor cannot have been destroyed yet.

## See also

- [qb-io: the asynchronous system](../3_qb_io/async_system.md) — the full reference for the listener, timers, watchers, and the CRTP bases.
- [qb-io: coroutines](../3_qb_io/coroutines.md) — `task<T>`, channels, combinators, cancellation, and structured concurrency.
- [qb-io: transports](../3_qb_io/transports.md) — TCP/UDP/SSL/QUIC transports plugged into the CRTP bases.
- [qb-io: protocols](../3_qb_io/protocols.md) — defining wire framing with `AProtocol`.
- [The event system](./event_system.md) — actor-to-actor events (distinct from the I/O readiness events above).
- [Concurrency and parallelism](./concurrency.md) — how listeners map onto `VirtualCore` worker threads.
