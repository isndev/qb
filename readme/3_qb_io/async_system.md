# The async runtime: event loop, timers, and callbacks

> **Audience:** Adopter · **Status:** stable · **Verified-against:** qb 2.0.0 (c++23)

`qb::io::async` is the single-threaded, libev-backed event loop that drives every non-blocking operation in `qb-io`: socket readiness, timers, scheduled callbacks, file-system watching, and the C++20 coroutine scheduler.

**Prerequisites:** [QB-IO overview](./README.md), [Time model](./utilities.md) — **See also:** [C++20 coroutines](./coroutines.md), [Transports](./transports.md), [Protocols](./protocols.md), [Async, lifecycle, and allocation invariants](../7_reference/io_invariants.md)

## Summary

The `qb::io::async` namespace provides an event-driven asynchronous model built on a single `listener` per thread. It integrates with the `qb-core` actor runtime — each `VirtualCore` owns one `listener` — but is fully usable standalone in any C++20 program. Two programming models share the same loop: event-driven handlers (`on(Event&&)`) and native coroutines (`co_await`). Neither runs concurrently with the other within a thread; the loop dispatches them sequentially. This page covers the loop itself, the callback and timer utilities, and the file watcher. For the coroutine model, see [C++20 coroutines](./coroutines.md).

Every timed API on this page takes `qb::duration` (a `std::chrono::nanoseconds` span) or any `std::chrono::duration`. Raw `double`-seconds arguments are not part of the public surface.

## Concepts

### The event loop: `qb::io::async::listener`

The `listener` class (`qb/io/async/listener.h`) is the central event-loop manager. It wraps a libev `ev::dynamic_loop`, constructed with `EVFLAG_AUTO` so libev selects the best available backend (epoll, kqueue, or select on POSIX; wepoll/IOCP on Windows).

- **Thread-local instance.** Each thread has its own loop, reached through the static member `qb::io::async::listener::current`. You never pass a listener around; you address the current thread's loop. Under `qb-core`, every `VirtualCore` thread has its own `listener::current`.
- **Single-threaded by contract.** A `listener` and every I/O object registered with it operate within one thread. I/O objects must not be shared across threads; isolation — not locking — provides their thread safety. On Windows the libev epoll path uses wepoll (IOCP), which additionally requires that the whole loop lifecycle stay on one thread.

The `listener` monitors four classes of kernel event and dispatches each to its registered handler:

| Watched source | qb-io event | Backed by |
|---|---|---|
| File-descriptor readiness (read/write) | `event::io` | `ev::io` |
| Timer / inactivity expiry | `event::timer` *(alias `event::timeout`)* | `ev::timer` |
| File or directory attribute change | `event::file` | `ev::stat` |
| OS signal | `event::signal<Sig>` | `ev::sig` |

When a watcher fires, the listener updates the wrapper's `_revents` field and calls `IRegisteredKernelEvent::invoke()`, which dispatches to the registered object's `on(SpecificEvent&)` method — checking `is_alive()` first when the handler type exposes it.

### Initialization

`qb::io::async::init()` exists for symmetry but is a **no-op**: `listener::current` is a `thread_local` that initializes itself on first access. You do not need to call it before using async features. When you need a *clean* loop — for example in a test `TearDown` — call `listener::current.clear()` directly, which detaches and flushes registered watchers. Do not rely on `init()` to reset state; it deliberately does nothing so that fixtures sharing a thread-local listener do not invalidate each other's already-registered watchers.

### Driving the loop (standalone usage)

Under `qb-core`, `qb::Main` runs the loop on each `VirtualCore`. Standalone, you drive it yourself. All free functions below operate on `listener::current`.

| Function | Behavior | Header |
|---|---|---|
| `async::run(int flag = 0)` | Run the loop with a libev flag. `0` blocks until `break_parent()` or no active watchers remain. Returns the number of events invoked. | `listener.h` |
| `async::run_once()` | `run(EVRUN_ONCE)` — wait for and process one block of events. | `listener.h` |
| `async::run_until(bool const& status)` | Repeatedly `run(EVRUN_NOWAIT)` while `status` is true; sleeps 50 µs between idle passes to avoid busy-spinning. | `listener.h` |
| `async::break_parent()` | Request `listener::current` to break out of its current `run()` cycle. | `listener.h` |
| `async::run_for(qb::duration)` | Pump the loop (and ready coroutines) for a `steady_clock`-measured duration, then return. Used by tests and coroutine drivers. | `coroutine/utils.h` |
| `async::run_sync(Awaitable&&)` | Spawn an awaitable and pump the loop until it completes, returning its result. Bridges synchronous code (e.g. test setup) to coroutine APIs. | `coroutine/utils.h` |

After processing libev events, every `run()` call also drains any ready coroutines through the listener's scheduler.

> **Pitfall — `EVRUN_ONCE` and timerfd.** When libev is built with timerfd-based time-jump detection (`QB_LIBEV_USE_TIMERFD=ON`, off by default) and only `ev_io` watchers are active with no heap timers, `EVRUN_ONCE` can block for libev's internal maximum wait time. In pump loops, prefer `run_until` or `run(EVRUN_NOWAIT)`.

> **Pitfall — re-entrancy.** `run`, `run_once`, `run_until`, `run_for`, and `run_sync` must not be called from inside a coroutine or actor handler that is itself executing under the coroutine scheduler's ready-drain. Doing so throws `std::logic_error` (and asserts in debug builds).

### Introspection

The `listener` exposes counters useful for monitoring and debugging:

- `nb_invoked_event()` — events invoked during the most recent `run()` (reset each call).
- `total_events_processed()` — cumulative events since listener creation (never reset).
- `size()` — number of currently registered watchers.

### Registering event handlers

`listener::current.registerEvent<_Event, _Actor, _Args...>(actor, args...)` associates a watcher with a handler and the loop:

- `_Event` — the qb-io event type (e.g. `event::io`, `event::timer`), each wrapping one libev watcher.
- `_Actor` — the handler instance; it must define an `on(_Event&)` method.
- `_Args...` — arguments forwarded to the libev watcher's `set()` (e.g. a file descriptor and `EV_READ`/`EV_WRITE` flags for `event::io`).

You rarely call this directly. The CRTP bases — `async::input`, `async::output`, `async::io`, `async::with_timeout`, `async::file_watcher` — register their watchers in their constructors and unregister them in their destructors. Registration and unregistration are O(1) with no per-event hash-table allocation: handlers are linked into an intrusive list owned by the listener, and each `RegisteredKernelEvent<E, A>` instantiation draws from a thread-local LIFO freelist, so steady-state churn performs no `malloc`/`free`.

## Scheduled callbacks: `async::callback`

`async::callback` (`qb/io/async/io.h`) schedules a callable for execution by the current thread's loop, optionally after a delay. It has two overloads:

```cpp
template <typename _Func>
void callback(_Func &&func);                              // (1) no delay

template <typename _Func, typename Rep, typename Period>
void callback(_Func &&func, std::chrono::duration<Rep, Period> timeout);  // (2) delayed
```

- **Overload (1)** invokes `func()` **immediately and synchronously** on the calling thread. It does not defer to the loop.
- **Overload (2)** with `timeout <= 0` likewise invokes `func()` immediately. With a positive duration it heap-allocates an internal `Timeout<_Func>` watcher that fires once after the delay and then deletes itself.

The delayed path is self-managing: the `Timeout<_Func>` registers with the listener, runs the callable, and frees itself. A thread-local LIFO freelist backs `Timeout<_Func>` allocation, so steady-state `callback()` traffic performs zero `malloc`/`free`. To honor short delays even when the calling thread has been idle outside the loop, overload (2) refreshes libev's cached "now" before arming the timer.

> **Exceptions are swallowed.** The delayed `Timeout<_Func>` invokes the callable inside `try { _func(); } catch (...) {}`. An exception escaping your callback is discarded, not propagated. Handle errors inside the callback.

```cpp
// src: derived from qb/source/io/tests/system/test-async-io.cpp
#include <qb/io/async.h>
#include <qb/system/timestamp.h>
#include <atomic>
#include <chrono>
#include <iostream>

using namespace std::chrono_literals;

int main() {
    std::atomic<bool> running{true};

    // Reschedule from inside the callback to build a periodic task.
    std::function<void()> tick = [&] {
        std::cout << "tick at " << qb::to_iso8601(qb::wall_now()) << '\n';
        if (running)
            qb::io::async::callback([&] { tick(); }, 1s);
    };
    qb::io::async::callback([&] { tick(); }, 1s);

    // One-shot stop after 5 seconds.
    qb::io::async::callback([&] {
        running = false;
        qb::io::async::break_parent();
    }, 5s);

    // Blocks until break_parent() runs or no active watchers remain.
    qb::io::async::run();
    return 0;
}
```

To **cancel** a pending callback or hold a handle to it, use `scoped_callback` instead.

## Owned, cancellable timers: `async::scoped_callback`

`scoped_callback` (`qb/io/async/io.h`) is the RAII counterpart to `callback`. It returns a `std::unique_ptr<ScopedTimeout<std::decay_t<_Func>>>` that the caller owns. Destroying or resetting the pointer stops the watcher and releases its registration; no self-delete is involved.

```cpp
#include <qb/io/async.h>
#include <chrono>
#include <iostream>

using namespace std::chrono_literals;

// Schedule work in 2 seconds, keeping a cancellation handle.
auto handle = qb::io::async::scoped_callback([] {
    std::cout << "2 seconds elapsed\n";
}, 2s);

// Cancel before it fires — stops and unregisters the watcher cleanly.
handle.reset();
```

A timeout of zero or less fires the callback inline at construction (matching `callback`'s immediate semantics) and marks the timer as fired. `ScopedTimeout` also exposes `fired()` and `cancel()` (which sets the timeout to `qb::duration::zero()`). As with `callback`, the callable runs inside a `catch (...)` and exceptions are swallowed.

| | `callback()` | `scoped_callback()` |
|---|---|---|
| Ownership | Self-deleting `Timeout<F>` | Caller-owned `unique_ptr<ScopedTimeout<F>>` |
| Cancellation | Not possible | `handle.reset()` or `handle->cancel()` |
| Heap traffic (steady state) | Zero (freelist) | One allocation per call |
| Best for | Fire-and-forget tasks | Watchdogs, retry loops, cancellable deadlines |

## Inactivity timeouts: `async::with_timeout<Derived>`

`with_timeout<Derived>` (`qb/io/async/io.h`) is a CRTP base that adds an inactivity timer to a class. It is the mechanism behind session idle-timeouts and operation deadlines.

```cpp
// src: derived from qb/source/io/tests/system/test-async-io.cpp (TimerHandler)
#include <qb/io/async.h>
#include <chrono>

using namespace std::chrono_literals;

class IdleHandler : public qb::io::async::with_timeout<IdleHandler> {
public:
    explicit IdleHandler(qb::duration timeout = 100ms)
        : with_timeout(timeout) {}

    // Called when the inactivity deadline is reached.
    void on(qb::io::async::event::timer const &) {
        // Handle the timeout: close a connection, fail an operation, etc.
    }

    void on_activity() {
        updateTimeout(); // Reset the countdown on each activity.
    }
};
```

- **Constructor.** `with_timeout(qb::duration timeout = std::chrono::seconds(3))` starts the timer when `timeout > 0`; a non-positive value leaves it disabled.
- **`updateTimeout()`** records the current loop time as the last activity. Because the underlying timer measures from last activity, calling this on each event resets the effective deadline without re-arming the watcher on every byte.
- **`setTimeout(qb::duration)`** changes the period and restarts the timer; pass `qb::duration::zero()` to disable it.
- **`getTimeout()`** returns the configured period as a `qb::duration` (zero when disabled).
- **Handler.** Implement `on(event::timer const&)` (or `on(event::timer&&)`). The base only forwards to your handler once the real deadline — accounting for the latest `updateTimeout()` — has elapsed; if activity is more recent, it silently re-arms for the remaining interval.

## Watching the file system: `async::file_watcher` and `async::directory_watcher`

`file_watcher<Derived>` and `directory_watcher<Derived>` (`qb/io/async/io.h`) wrap an `event::file` (libev `ev::stat`) to monitor a path for attribute changes such as size or modification time.

```cpp
#include <qb/io/async.h>
#include <chrono>
#include <iostream>
#include <string>

using namespace std::chrono_literals;

class LogTail : public qb::io::async::directory_watcher<LogTail> {
public:
    explicit LogTail(std::string path) {
        // Poll every 500 ms for attribute changes.
        start(path, 500ms);
    }

    void on(qb::io::async::event::file const &event) {
        if (event.attr.st_nlink == 0) {
            std::cout << "path removed\n";
            disconnect(); // Stop watching.
            return;
        }
        if (event.attr.st_mtime != event.prev.st_mtime)
            std::cout << "attributes changed, size " << event.attr.st_size << '\n';
    }
};
```

- **`start(std::string const& path, qb::duration interval = std::chrono::milliseconds(100))`** begins watching. `interval` is libev's polling cadence: shorter is more responsive but costs more CPU.
- **`disconnect()`** stops the watcher.
- The `event::file` payload carries `attr` (current `ev_statdata`) and `prev` (previous snapshot). `attr.st_nlink == 0` indicates deletion.

`file_watcher` differs from `directory_watcher` in that it also reads and frames file content through a `qb::io::async::IProtocol` (`do_read == true`). When the watched file grows, `file_watcher` calls `read_all()`, which runs the active protocol's `getMessageSize()` / `onMessage()` loop and enforces the configured maximum message size. `directory_watcher` (`do_read == false`) only forwards the `event::file` notification. The `async::file<Derived>` template (`qb/io/async/file.h`) composes `file_watcher` with `transport::file` for non-blocking file consumption; see [Transports](./transports.md).

## Standard event types: `qb::io::async::event::*`

`qb-io` defines strongly-typed event structs (`qb/io/async/event/all.h`). Custom I/O components — those deriving from `async::input`, `output`, or `io` — react by overriding `on(SpecificEvent&&)`.

| Event type | Trigger | Key fields |
|---|---|---|
| `disconnected` | Connection closed or I/O error | `int reason`, `std::error_code error_code`, `std::string message` |
| `input_drained` *(alias `eof`)* | Input buffer fully consumed (not an end-of-stream signal) | — |
| `eos` | Output buffer fully flushed | — |
| `file` | Watched file/directory attributes changed | `ev_statdata attr`, `ev_statdata prev` |
| `handshake` | Handshake complete | — |
| `io` | Raw fd readiness; `_revents` carries `EV_READ`/`EV_WRITE` | (libev `ev::io` base) |
| `pending_read` | Unprocessed bytes remain in the input buffer | `std::size_t bytes` |
| `pending_write` | Unsent bytes remain in the output buffer | `std::size_t bytes` |
| `signal<Sig>` | OS signal caught | (libev `ev::sig` base) |
| `timer` *(alias `timeout`)* | Timer or inactivity timeout expired | (libev `ev::timer` base) |
| `extracted` | Connection extracted from an I/O handler | — |
| `dispose` | Component is about to be destroyed | — |

### Disconnect reason codes

The `disconnected` event carries a `reason` field of type `disconnect_reason`, an `int`-backed enum. The integer backing keeps it interchangeable with raw codes, so applications may pass their own positive values.

| Code | Named constant | Meaning |
|---|---|---|
| `0` | `peer_closed` | Normal shutdown — peer closed, or the local side closed cleanly |
| `1` | `user_initiated` | Explicit `disconnect()` call from application code |
| `> 1` | *(application-defined)* | Custom application codes (e.g. `qbm-http` reason codes) |
| `-1` | `protocol_error` | Protocol marked itself `not_ok()` |
| `-2` | `message_too_large` | Incoming frame exceeded the configured maximum message size |
| `-3` | `buffer_overflow` | Read/write buffer exceeded its configured limit |

```cpp
void on(qb::io::async::event::disconnected &&ev) {
    using R = qb::io::async::event::disconnect_reason;
    switch (static_cast<R>(ev.reason)) {
        case R::peer_closed:       /* normal */    break;
        case R::protocol_error:    /* bad frame */ break;
        case R::message_too_large: /* DoS guard */ break;
        default:
            if (ev.error_code)
                std::cerr << "system error: " << ev.error_code.message() << '\n';
    }
}
```

### Handler signature rules

Event handlers in CRTP-derived classes must use a compatible signature:

- `void on(event::X&&)` — preferred (rvalue, move-enabled).
- `void on(event::X const&)` — accepted.
- `void on(event::X&)` — **not** accepted for rvalue-delivered events; it silently fails to bind.

See [Async, lifecycle, and allocation invariants](../7_reference/io_invariants.md) for the full CRTP dispatch rules.

## Pitfalls

- **`init()` does not reset the loop.** It is a no-op. For a clean event loop (tests, restarts), call `listener::current.clear()`.
- **`callback(func)` is not deferred.** Overload (1), and overload (2) with a non-positive duration, run the callable synchronously and immediately. Only a positive duration schedules a future fire.
- **Pass durations, not `double` seconds.** Every timed API takes `qb::duration` or a `std::chrono::duration`. There is no `double`-seconds public overload.
- **Callbacks swallow exceptions.** `Timeout` and `ScopedTimeout` wrap the callable in `catch (...)`. Errors that escape your callable are silently dropped.
- **Do not re-enter `run*` from a handler.** Calling `run`, `run_once`, `run_until`, `run_for`, or `run_sync` from inside a coroutine or actor handler executing under the scheduler throws `std::logic_error`.
- **One thread per listener.** Never share I/O objects, watchers, or the loop across threads. Each thread that does async work has its own `listener::current`.

## See also

- [C++20 coroutines](./coroutines.md) — the `co_await` model layered on this loop, plus `sleep`, combinators, channels, and the scheduler.
- [Transports](./transports.md) — the socket and file transports that ride on `async::input`/`output`/`io`.
- [Protocols](./protocols.md) — the `IProtocol` framing the watcher and I/O bases drive.
- [Async, lifecycle, and allocation invariants](../7_reference/io_invariants.md) — registration, dispatch, and allocation guarantees in reference form.
