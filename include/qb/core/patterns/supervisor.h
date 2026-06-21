/**
 * @file qb/core/patterns/supervisor.h
 * @brief `Supervisor` / `SupervisedActor` — restart child actors on failure (no coroutine needed).
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
 * @ingroup Patterns
 */

#ifndef QB_CORE_PATTERNS_SUPERVISOR_H
#define QB_CORE_PATTERNS_SUPERVISOR_H

#include <cstddef>
#include <cstdint>
#include <vector>
#include <qb/core/Actor.h>

namespace qb {

/**
 * @enum restart_strategy
 * @ingroup Patterns
 * @brief How a `Supervisor` restarts children when one terminates.
 */
enum class restart_strategy {
    one_for_one, ///< Restart only the child that went down.
    one_for_all, ///< Restart every child when any one goes down.
    rest_for_one ///< Restart the child that went down and every child started after it.
};

/**
 * @struct ChildDown
 * @ingroup Patterns
 * @brief Sent by a supervised child to its `Supervisor` when it terminates.
 * @details Carries the child's `slot` and `generation`; the supervisor ignores a notification
 *          whose generation no longer matches (a stale report from an already-replaced child).
 */
struct ChildDown : qb::Event {
    std::size_t   slot;       ///< The child's slot index in the supervisor.
    std::uint64_t generation; ///< The child's generation (stale generations are ignored).
    ChildDown(std::size_t s, std::uint64_t g) noexcept
        : slot(s)
        , generation(g) {}
};

/** @brief Alias for `ChildDown`. */
using child_down = ChildDown;

/**
 * @class SupervisedActor
 * @ingroup Patterns
 * @brief Base for an actor managed by a `Supervisor`.
 * @details It knows its supervisor, slot and generation, and notifies the supervisor when it stops
 *          so it can be restarted. Derive from it instead of `qb::Actor` and call `stop()` (or
 *          `notify_supervisor_down()`) on cooperative termination.
 */
class SupervisedActor : public qb::Actor {
    qb::ActorId   _supervisor;
    std::size_t   _slot;
    std::uint64_t _generation;

protected:
    SupervisedActor(qb::ActorId supervisor, std::size_t slot, std::uint64_t generation) noexcept
        : _supervisor(supervisor)
        , _slot(slot)
        , _generation(generation) {}

    /** @brief The supervising actor's id. */
    [[nodiscard]] qb::ActorId
    supervisor() const noexcept {
        return _supervisor;
    }

    /** @brief Tell the supervisor this child is going down (so it can restart it). */
    void
    notify_supervisor_down() const noexcept {
        this->template push<qb::ChildDown>(_supervisor, _slot, _generation);
    }

    /** @brief Cooperative termination: notify the supervisor, then `kill()` this child. */
    void
    stop() {
        notify_supervisor_down();
        this->kill();
    }
};

/** @brief Alias for `SupervisedActor`. */
using supervised_actor = SupervisedActor;

/**
 * @class Supervisor
 * @ingroup Patterns
 * @brief Supervises a fixed set of child actors, restarting them per a `restart_strategy`.
 * @details
 * Per-core supervision: override `spawn_child(slot, generation)` to create child `slot` with
 * `addRefActor<Child>(id(), slot, generation, …)` (a `SupervisedActor`). A child calls `stop()` to
 * terminate; the supervisor then restarts it — and, for `one_for_all` / `rest_for_one`, its
 * siblings — bumping each restarted slot's generation so stale `ChildDown`s are ignored. An optional
 * `max_restarts` cap calls `on_escalate()` instead of restarting once exceeded.
 * @note Cooperative: a child that dies without calling `stop()` (e.g. a failed `onInit`) is not
 *       auto-detected — supervision keys off the `ChildDown` notification.
 */
class Supervisor : public qb::Actor {
public:
    /**
     * @param strategy   Which children to restart when one goes down.
     * @param child_count Number of supervised slots.
     * @param max_restarts Restart-intensity cap (0 = unlimited); `on_escalate()` fires past it.
     */
    Supervisor(qb::restart_strategy strategy, std::size_t child_count,
               unsigned max_restarts = 0) noexcept
        : _strategy(strategy)
        , _count(child_count)
        , _max_restarts(max_restarts) {}

    qb::io::async::task<bool>
    onInit() override {
        registerEvent<qb::ChildDown>(*this);
        _children.assign(_count, qb::ActorId{});
        _gen.assign(_count, 0);
        for (std::size_t i = 0; i < _count; ++i)
            start_slot(i);
        co_return true;
    }

    void
    on(qb::ChildDown &e) {
        if (e.slot >= _count || e.generation != _gen[e.slot])
            return; // stale or unknown notification — ignore
        if (_max_restarts && _restarts >= _max_restarts) {
            on_escalate();
            return;
        }
        ++_restarts;
        switch (_strategy) {
            case qb::restart_strategy::one_for_one:
                ++_gen[e.slot];
                start_slot(e.slot);
                break;
            case qb::restart_strategy::one_for_all:
                restart_slots(0, _count, e.slot);
                break;
            case qb::restart_strategy::rest_for_one:
                restart_slots(e.slot, _count, e.slot);
                break;
        }
    }

    /** @brief Current child id at `slot` (invalid id if out of range). */
    [[nodiscard]] qb::ActorId
    child(std::size_t slot) const {
        return slot < _children.size() ? _children[slot] : qb::ActorId{};
    }
    /** @brief Total restarts performed so far. */
    [[nodiscard]] unsigned
    restarts() const noexcept {
        return _restarts;
    }
    /** @brief Number of supervised slots. */
    [[nodiscard]] std::size_t
    child_count() const noexcept {
        return _count;
    }

protected:
    /**
     * @brief Create the child for `slot` and return its id.
     * @details Override and call e.g. `return addRefActor<Child>(id(), slot, generation).id();`
     *          where `Child` derives from `SupervisedActor`.
     */
    virtual qb::ActorId spawn_child(std::size_t slot, std::uint64_t generation) = 0;

    /** @brief Called when the restart-intensity cap is exceeded (default: no-op). */
    virtual void
    on_escalate() {}

private:
    void
    start_slot(std::size_t i) {
        _children[i] = spawn_child(i, _gen[i]);
    }

    // Restart slots [from, to). `down` already terminated, so it is not killed, only respawned.
    void
    restart_slots(std::size_t from, std::size_t to, std::size_t down) {
        for (std::size_t j = from; j < to; ++j) {
            ++_gen[j]; // invalidate any in-flight ChildDown from the outgoing child
            if (j != down)
                this->template push<qb::KillEvent>(_children[j]); // stop the survivor (no ChildDown)
        }
        for (std::size_t j = from; j < to; ++j)
            _children[j] = spawn_child(j, _gen[j]);
    }

    qb::restart_strategy       _strategy;
    std::size_t                _count;
    unsigned                   _max_restarts;
    unsigned                   _restarts = 0;
    std::vector<qb::ActorId>   _children;
    std::vector<std::uint64_t> _gen;
};

/** @brief Alias for `Supervisor`. */
using supervisor = Supervisor;

} // namespace qb

#endif // QB_CORE_PATTERNS_SUPERVISOR_H
