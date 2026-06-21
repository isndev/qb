/**
 * @file qb/core/patterns/pubsub.h
 * @brief `PubSub<Topic>` — a per-core publish/subscribe bus (no coroutine needed).
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

#ifndef QB_CORE_PATTERNS_PUBSUB_H
#define QB_CORE_PATTERNS_PUBSUB_H

#include <algorithm>
#include <cstddef>
#include <vector>
#include <qb/core/Actor.h>

namespace qb {

/**
 * @class PubSub
 * @ingroup Patterns
 * @brief Per-core publish/subscribe bus for a single topic event type.
 * @tparam Topic The event type published on this bus.
 * @details A per-`VirtualCore` `ServiceActor`: add one with `core(i).addActor<qb::PubSub<Topic>>()`.
 * Same-core actors reach it via `getService<qb::PubSub<Topic>>()`: a subscriber calls
 * `subscribe(id())` (and registers a `Topic` handler); a publisher calls `publish(args…)` to fan a
 * freshly built `Topic` event out to every current subscriber. Per-core by design — a publication
 * reaches subscribers on the bus's own core only; add a bus per core for cross-core topics.
 * @code
 * // subscriber onInit (same core as the bus):
 * registerEvent<PriceTick>(*this);
 * getService<qb::PubSub<PriceTick>>()->subscribe(id());
 * // publisher (same core):
 * getService<qb::PubSub<PriceTick>>()->publish(symbol, price);   // builds PriceTick{symbol, price}
 * @endcode
 */
template <class Topic>
class PubSub : public qb::ServiceActor<PubSub<Topic>> {
    std::vector<qb::ActorId> _subscribers;

public:
    qb::io::async::task<bool>
    onInit() override {
        co_return true;
    }

    /** @brief Subscribe an actor (idempotent). It must register a `Topic` handler to receive. */
    void
    subscribe(qb::ActorId who) {
        for (auto const s : _subscribers)
            if (s == who)
                return;
        _subscribers.push_back(who);
    }
    /** @brief Unsubscribe an actor (no-op if not subscribed). */
    void
    unsubscribe(qb::ActorId who) {
        _subscribers.erase(std::remove(_subscribers.begin(), _subscribers.end(), who),
                           _subscribers.end());
    }
    /** @brief Current subscriber count. */
    [[nodiscard]] std::size_t
    subscriber_count() const noexcept {
        return _subscribers.size();
    }

    /**
     * @brief Publish: build a `Topic` from `args` and push a copy to every subscriber.
     * @tparam Args Constructor arguments for `Topic` (or a `Topic` to copy).
     */
    template <class... Args>
    void
    publish(Args const &...args) {
        for (auto const s : _subscribers)
            this->template push<Topic>(s, args...);
    }
};

} // namespace qb

#endif // QB_CORE_PATTERNS_PUBSUB_H
