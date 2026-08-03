/**
 * @file qb/io/async/event/base.h
 * @brief Base class for asynchronous events in the QB IO library
 *
 * This file defines the base infrastructure for events in the asynchronous I/O system.
 * It provides an interface for kernel event registration and a base template class
 * that wraps libev events to be used throughout the library.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * @ingroup IO
 */

#ifndef QB_IO_ASYNC_EVENT_BASE_H
#define QB_IO_ASYNC_EVENT_BASE_H

#include <qb/vendor/qev/qev++.h>

/* qb builds its vendored libev with EV_MULTIPLICITY=1 (qb/vendor/qev/config.h.cmakein). qev.h only
 * learns that from the generated qev_config.h, and its last-resort lookup for that file is guarded
 * by __has_include (ev.h:28-31) -- so if the file is not reachable, ev.h SILENTLY falls back to its
 * own default of EV_FEATURE_CONFIG == 4. Every qev_* prototype then grows a loop parameter that the
 * compiled libqev.a does not have: an ODR/ABI mismatch that links and then misbehaves at runtime.
 * Make the miss a compile error instead. */
#if !defined(EV_MULTIPLICITY) || (EV_MULTIPLICITY) != 1
#error "qb: libev configuration header not reached (EV_MULTIPLICITY != 1). qb::io's exported \
EV_CONFIG_H=<qb/vendor/qev/qev_config.h> definition, or the include directory carrying it, is missing."
#endif

namespace qb::io::async {

class listener; // fwd-decl for friendship

/**
 * @interface IRegisteredKernelEvent
 * @ingroup Async
 * @brief Interface for kernel event registration and invocation.
 *
 * This interface provides a common abstraction for objects that can be registered
 * with the `listener` to handle specific kernel-level events (wrapped by libev).
 * When a monitored event occurs, the `listener` calls the `invoke()` method
 * of the corresponding `IRegisteredKernelEvent` implementation.
 *
 * **Intrusive bookkeeping (QB_IO_PLAN 2.20):** every registered event is also a
 * node in the owning `listener`'s doubly-linked list. The list links live in
 * the interface itself to keep `registerEvent` / `unregisterEvent` O(1) without
 * an extra hash-table indirection (the previous `std::unordered_set<void *>`
 * implied one heap allocation *and* one hash computation per registration).
 * Only the `listener` may touch the links — users of this interface never see
 * them.
 */
class IRegisteredKernelEvent {
    friend class listener;

    // Intrusive list links (null when not currently registered).
    IRegisteredKernelEvent *_list_prev         = nullptr;
    IRegisteredKernelEvent *_list_next         = nullptr;
    bool                    _detached_by_clear = false;

    // Loop-owned, self-deleting handlers (async::callback's `Timeout<F>`) have no
    // external owner: they `delete this` only when their one-shot timer fires. If
    // the listener is torn down while such a timer is still pending, the timer never
    // fires and nothing reclaims the object — `listener::clear()` must destroy it.
    // These fields carry a type-erased deleter for that owner and stay null for every
    // externally-owned watcher (TCP sessions, `ScopedTimeout`, …) whose own destructor
    // already reclaims this wrapper, so their teardown path is unchanged.
    void *_owner                            = nullptr;
    void (*_destroy_owner)(void *) noexcept = nullptr;

public:
    /**
     * @brief Mark this watcher's handler as a loop-owned, self-deleting object.
     * @param owner   Pointer to the owning object (e.g. the `Timeout<F>` itself).
     * @param destroy Type-erased deleter invoked by `listener::clear()` if the watcher
     *                is still registered at teardown. Destroying the owner cascades
     *                (through its `async::base` destructor) to unregister and free this
     *                wrapper, so no separate wrapper delete is needed.
     * @details Called once at construction by self-owned async helpers. Externally
     *          owned watchers never call this and keep the default (null) behaviour.
     */
    void
    set_owner(void *owner, void (*destroy)(void *) noexcept) noexcept {
        _owner         = owner;
        _destroy_owner = destroy;
    }

    /**
     * @brief Virtual destructor.
     */
    virtual ~IRegisteredKernelEvent() = default;

    /**
     * @brief Stop the concrete libev watcher without destroying the wrapper.
     *
     * listener::clear() uses this to detach live async objects safely: the
     * owning async::base still holds a reference to the embedded event and
     * will unregister/delete the wrapper from its own destructor.
     */
    virtual void stop() noexcept = 0;

    /**
     * @brief Event invocation method, called by the listener when the event triggers.
     *
     * Implementing classes should define their specific event handling logic in this method.
     * This typically involves casting to the concrete event type and calling the user's
     * `on(SpecificEvent&)` handler.
     */
    virtual void invoke() = 0;
};

namespace event {

/**
 * @class base
 * @ingroup AsyncEvent
 * @brief Base template class for all qb-io specific asynchronous event types.
 *
 * This template class serves as the foundation for specific event wrappers like
 * `qb::io::async::event::io`, `qb::io::async::event::timer`, etc. It wraps the
 * corresponding libev event watcher (e.g. `ev::io`, `ev::timer`) and holds a pointer
 * to the `IRegisteredKernelEvent` interface for dispatching.
 *
 * @tparam _EV_EVENT The libev event watcher type (e.g. `ev::io`, `ev::timer`) being wrapped.
 */
template <typename _EV_EVENT>
struct base : public _EV_EVENT {
    using ev_t = _EV_EVENT;             /**< Alias for the underlying libev event watcher type. */
    IRegisteredKernelEvent *_interface; /**< Pointer to the kernel event interface responsible for handling this event. */
    int                     _revents;   /**< Stores the event flags (e.g., EV_READ, EV_WRITE) received from libev when the event triggers. */

    /**
     * @brief Constructor.
     * @param loop Reference to the libev event loop (`ev::loop_ref`) this event will be associated with.
     */
    explicit base(ev::loop_ref loop) noexcept
        : _EV_EVENT(loop)
        , _interface(nullptr)
        , _revents(0) {}
};

} // namespace event
} // namespace qb::io::async

#endif // QB_IO_ASYNC_EVENT_BASE_H
