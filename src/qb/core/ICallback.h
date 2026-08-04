/**
 * @file qb/core/ICallback.h
 * @brief Periodic-callback interface for actors in the QB Actor Framework.
 *
 * Defines `qb::ICallback`, the mixin interface that allows an actor to receive
 * a recurring invocation (`on(qb::LoopEvent const&)`) on every iteration of its owning
 * `VirtualCore`'s event loop.
 *
 * ### Usage pattern
 * 1. Inherit from **both** `qb::Actor` and `qb::ICallback`.
 * 2. Override `on(qb::LoopEvent const&)` with the periodic logic.
 * 3. Call `registerCallback(*this)` inside `onInit()` to activate the callback.
 * 4. Call `unregisterCallback()` (or `unregisterCallback(*this)`) at any time to
 *    deactivate it — including from within the tick handler itself.
 *
 * @warning The tick handler is executed on the `VirtualCore`'s event-loop thread.
 *          It **must be fast and non-blocking**. Any long computation or blocking
 *          I/O inside `on(qb::LoopEvent const&)` stalls the entire core and all actors
 *          running on it.
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
 * @ingroup Core
 */

#ifndef QB_ICALLBACK_H
#define QB_ICALLBACK_H

#include <cstdint>
// Documented entry point (<qb/icallback.h>): ICallback is a public base of user actors, so a
// translation unit can reach qb through this header alone. Carrying the link-time ABI contract
// here keeps the property "every top-level qb/*.h entry point fingerprints its configuration"
// true without exceptions to explain.
#include <qb/utility/abi.h>

namespace qb {

/*!
 * @struct LoopEvent
 * @ingroup Callback
 * @brief Per-iteration context delivered to `qb::ICallback::on(qb::LoopEvent const&)`.
 *
 * @details
 * `LoopEvent` is the "tick" notification the `VirtualCore` hands to every registered callback
 * on each pass of its event loop. It is **not** a routable `qb::Event`: it is never `push`ed,
 * has no source/destination, and is delivered by a direct virtual call — exactly like the
 * framework-synthesised notifications in `qb::io::async::event::*`. Handle it with
 * `void on(qb::LoopEvent const &)`.
 *
 * It carries read-only context about the current loop pass and is **forward-compatible**: new
 * fields may be appended in future versions without changing the handler signature, so prefer
 * reading what you need from `LoopEvent` over recomputing it.
 */
struct LoopEvent {
    /*!
     * @brief Cached loop timestamp — nanoseconds since the Unix epoch.
     * @details Identical to `qb::Actor::time()` for this iteration (sourced once from
     *          `qb::wall_now()` at the top of the loop), so every callback in the same pass
     *          observes the same value.
     */
    std::uint64_t now{0};
    /*!
     * @brief Monotonic 1-based index of the current `VirtualCore` loop pass.
     * @details Increases by one per loop iteration for the lifetime of the core (the first tick
     *          an actor observes after registering is **not** guaranteed to be `1` — it is the
     *          core's current pass number). Useful for "every N ticks" throttling.
     */
    std::uint64_t iteration{0};
};

/** @brief Alias for `qb::LoopEvent`. @ingroup Callback */
using loop_event = LoopEvent;

/*!
 * @interface ICallback
 * @ingroup Callback
 * @brief Mixin interface that grants an actor access to the `VirtualCore` event-loop tick.
 *
 * @details
 * Any actor that inherits from `ICallback` (in addition to `qb::Actor`) may register
 * itself for periodic invocations by calling `registerCallback(*this)` inside `onInit()`.
 * From that point the framework will call `on(qb::LoopEvent const&)` on **every** iteration of
 * the owning `VirtualCore`'s main loop — after the local event queue has been drained and
 * before outgoing pipes are flushed.
 *
 * ### Typical lifecycle
 * ```
 * onInit()
 *   └─ registerCallback(*this)        ← activates the hook
 *
 * [VirtualCore loop iteration N]
 *   ├─ process mailbox events
 *   ├─ process local events
 *   ├─ on(qb::LoopEvent const&)       ← called here
 *   └─ flush pipes
 *
 * [any handler or the tick]
 *   └─ unregisterCallback()           ← deactivates the hook
 * ```
 *
 * ### Complete example
 * @code
 * #include <qb/actor.h>
 * #include <qb/icallback.h>
 * #include <qb/io.h>
 *
 * class HeartbeatActor : public qb::Actor, public qb::ICallback {
 *     int _tick = 0;
 * public:
 *     qb::io::async::task<bool> onInit() override {
 *         registerEvent<qb::KillEvent>(*this);
 *         registerCallback(*this);  // start periodic callback
 *         co_return true;
 *     }
 *
 *     void on(qb::LoopEvent const &loop) override {
 *         ++_tick;
 *         qb::io::cout() << "tick #" << _tick << " (loop pass " << loop.iteration << ")\n";
 *         if (_tick >= 10) {
 *             unregisterCallback(); // stop ticking
 *             kill();
 *         }
 *     }
 *
 *     void on(qb::KillEvent const &) { kill(); }
 * };
 * @endcode
 *
 * @note The callback frequency depends on the `VirtualCore`'s loop rate, which is
 *       determined by the volume of events being processed and the latency setting
 *       configured via `CoreInitializer::setLatency()`.
 * @see qb::Actor::registerCallback
 * @see qb::Actor::unregisterCallback
 */
class ICallback {
public:
    /*!
     * @brief Virtual destructor — safe polymorphic deletion.
     */
    virtual ~ICallback() = default;

    /*!
     * @brief Called by the `VirtualCore` on every loop iteration while registered.
     * @param loop Read-only context for this loop pass (`now`, `iteration`); see `qb::LoopEvent`.
     *
     * @details
     * Override this pure virtual method with the logic that must run periodically. It mirrors the
     * framework's `on(Event&)` dispatch shape — the periodic tick is just another `on(...)` — and
     * `qb::LoopEvent` keeps the hook forward-compatible (new loop context can be added without
     * changing the signature).
     *
     * **Requirements:**
     * - Must complete in the shortest time possible.
     * - Must **never** block (no mutex waits, no synchronous I/O, no `sleep`).
     * - May call `unregisterCallback()` to stop future invocations.
     * - May send events to other actors via `push<Event>(dest, ...)`.
     */
    virtual void on(qb::LoopEvent const &loop) = 0;
};

/**
 * @typedef icallback
 * @brief Alias for the ICallback interface
 * @details Provided for naming consistency with other lowercase aliases in the framework
 * @ingroup Callback
 */
using icallback = ICallback;

} // namespace qb
#endif // QB_ICALLBACK_H
