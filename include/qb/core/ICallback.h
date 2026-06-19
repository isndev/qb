/**
 * @file qb/core/ICallback.h
 * @brief Periodic-callback interface for actors in the QB Actor Framework.
 *
 * Defines `qb::ICallback`, the mixin interface that allows an actor to receive
 * a recurring invocation (`onCallback()`) on every iteration of its owning
 * `VirtualCore`'s event loop.
 *
 * ### Usage pattern
 * 1. Inherit from **both** `qb::Actor` and `qb::ICallback`.
 * 2. Override `onCallback()` with the periodic logic.
 * 3. Call `registerCallback(*this)` inside `onInit()` to activate the callback.
 * 4. Call `unregisterCallback()` (or `unregisterCallback(*this)`) at any time to
 *    deactivate it — including from within `onCallback()` itself.
 *
 * @warning `onCallback()` is executed on the `VirtualCore`'s event-loop thread.
 *          It **must be fast and non-blocking**. Any long computation or blocking
 *          I/O inside `onCallback()` stalls the entire core and all actors running
 *          on it.
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

namespace qb {

/*!
 * @interface ICallback
 * @ingroup Callback
 * @brief Mixin interface that grants an actor access to the `VirtualCore` event-loop tick.
 *
 * @details
 * Any actor that inherits from `ICallback` (in addition to `qb::Actor`) may register
 * itself for periodic invocations by calling `registerCallback(*this)` inside `onInit()`.
 * From that point the framework will call `onCallback()` on **every** iteration of the
 * owning `VirtualCore`'s main loop — after the local event queue has been drained and
 * before outgoing pipes are flushed.
 *
 * ### Typical lifecycle
 * ```
 * onInit()
 *   └─ registerCallback(*this)   ← activates the hook
 *
 * [VirtualCore loop iteration N]
 *   ├─ process mailbox events
 *   ├─ process local events
 *   ├─ onCallback()              ← called here
 *   └─ flush pipes
 *
 * [any handler or onCallback()]
 *   └─ unregisterCallback()      ← deactivates the hook
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
 *     bool onInit() override {
 *         registerEvent<qb::KillEvent>(*this);
 *         registerCallback(*this);  // start periodic callback
 *         return true;
 *     }
 *
 *     void onCallback() override {
 *         ++_tick;
 *         qb::io::cout() << "tick #" << _tick << "\n";
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
     *
     * @details
     * Override this pure virtual method with the logic that must run periodically.
     *
     * **Requirements:**
     * - Must complete in the shortest time possible.
     * - Must **never** block (no mutex waits, no synchronous I/O, no `sleep`).
     * - May call `unregisterCallback()` to stop future invocations.
     * - May send events to other actors via `push<Event>(dest, ...)`.
     */
    virtual void onCallback() = 0;
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
