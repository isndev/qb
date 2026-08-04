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
#include <deque>
#include <vector>
#include <qb/core/Actor.h>
#include <qb/system/time.h> // qb::duration
// Actor.h declares Actor::push<E>/CoroContext::push_to<E> and defines neither; the bodies are
// in Actor.tpp. This header INSTANTIATES them in a non-dependent context, so without this it
// compiles clean, warns only under -Wundefined-func-template (which -isystem hides from every
// find_package consumer), and emits an undefined reference. Free when core/patterns.h got
// there first -- QB_ACTOR_TPL makes the second inclusion a no-op.
#include <qb/core/Actor.tpp>

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
 * `max_restarts` cap calls `on_escalate()` instead of restarting once exceeded — either cumulative
 * (over the supervisor's whole life) or, with a non-zero `restart_window`, as a **sliding-window
 * intensity** (the conventional "N restarts within T" rule). Killing the supervisor itself
 * (a `KillEvent`) tears down all its children first, so they are never orphaned; `Main::stop()` /
 * `SIGINT` already broadcasts to every actor, children included.
 * @note Cooperative: a child that dies without calling `stop()` (e.g. a failed `onInit`) is not
 *       auto-detected — supervision keys off the `ChildDown` notification.
 */
class Supervisor : public qb::Actor {
public:
    /**
     * @param strategy   Which children to restart when one goes down.
     * @param child_count Number of supervised slots.
     * @param max_restarts Restart-intensity cap (0 = unlimited); `on_escalate()` fires past it.
     * @param restart_window If non-zero, `max_restarts` is counted only over the trailing
     *        `restart_window` (sliding-window intensity); if zero (default), it is cumulative.
     */
    Supervisor(qb::restart_strategy strategy, std::size_t child_count, unsigned max_restarts = 0,
               qb::duration restart_window = qb::duration::zero()) noexcept
        : _strategy(strategy)
        , _count(child_count)
        , _max_restarts(max_restarts)
        , _window(restart_window) {}

    qb::io::async::task<bool>
    onInit() override {
        registerEvent<qb::ChildDown>(*this);
        // Rebind KillEvent to THIS type so on(KillEvent) (child teardown) runs instead of the
        // base Actor::on(KillEvent) bound at construction.
        registerEvent<qb::KillEvent>(*this);
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
        if (_max_restarts && over_restart_limit()) {
            on_escalate();
            return;
        }
        record_restart();
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

    /**
     * @brief Killing the supervisor tears down its children first (no orphans).
     * @details Sends a `KillEvent` to every live child, then `kill()`s itself. Bumps each slot's
     *          generation so any in-flight `ChildDown` from a child is ignored (no spurious restart
     *          while shutting down). `Main::stop()` / `SIGINT` need no special handling — they
     *          broadcast to every actor, children included.
     */
    void
    on(qb::KillEvent const &) {
        for (std::size_t j = 0; j < _children.size(); ++j) {
            ++_gen[j]; // ignore any ChildDown racing the shutdown
            if (_children[j].is_valid())
                this->template push<qb::KillEvent>(_children[j]);
        }
        kill();
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

    /// Is the restart-intensity cap exceeded? Cumulative when `_window == 0`, else over the
    /// trailing `_window` (prunes expired restart timestamps first).
    [[nodiscard]] bool
    over_restart_limit() {
        if (_window <= qb::duration::zero())
            return _restarts >= _max_restarts; // cumulative (legacy)
        const auto now    = time();
        const auto win_ns = static_cast<std::uint64_t>(_window.count());
        const auto cutoff = now > win_ns ? now - win_ns : std::uint64_t{0};
        while (!_restart_times.empty() && _restart_times.front() < cutoff)
            _restart_times.pop_front();
        return _restart_times.size() >= _max_restarts;
    }

    /// Record a restart for both the cumulative counter and the sliding window.
    void
    record_restart() {
        ++_restarts;
        if (_window > qb::duration::zero())
            _restart_times.push_back(time());
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
    qb::duration               _window; ///< 0 = cumulative cap; >0 = sliding-window intensity.
    unsigned                   _restarts = 0;
    std::vector<qb::ActorId>   _children;
    std::vector<std::uint64_t> _gen;
    std::deque<std::uint64_t>  _restart_times; ///< restart timestamps (ns) for the sliding window.
};

/** @brief Alias for `Supervisor`. */
using supervisor = Supervisor;

} // namespace qb

#endif // QB_CORE_PATTERNS_SUPERVISOR_H
