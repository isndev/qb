/**
 * @file qb/core/patterns/idempotency.h
 * @brief Responder-side request de-duplication for the `ask` pattern (exactly-once effects).
 *
 * `ask_retry` re-sends a request with a fresh `correlation_id` per attempt, so a reply lost to a
 * timeout makes the responder run the side effect twice. Carry a **stable** `idempotency_key` on
 * the request (preserved across retries, since each attempt copies the request) and let the
 * responder de-duplicate by it: the first request computes and caches the response; a repeat with
 * the same key replays the cached response without re-running the effect.
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

#ifndef QB_CORE_PATTERNS_IDEMPOTENCY_H
#define QB_CORE_PATTERNS_IDEMPOTENCY_H

#include <cstddef>
#include <list>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <qb/core/Actor.h>
#include "request.h" // ask_event_type

namespace qb {

/**
 * @concept idempotent_event
 * @ingroup Patterns
 * @brief A `qb::Request`-style exchange that also carries a stable `idempotency_key`.
 * @details Add an `idempotency_key` field to your `Request<Resp>` subtype (any hashable, default-
 *          constructible type — typically `std::uint64_t`). The default-valued key means "no key"
 *          and is never de-duplicated (every such request runs the effect).
 */
template <class E>
concept idempotent_event = ask_event_type<E> && requires(E e) {
    e.response;
    e.idempotency_key;
};

/**
 * @class dedup_map
 * @ingroup Patterns
 * @brief A bounded **LRU** cache of `key → response`, the store behind `answer_idempotent`.
 * @tparam Key The idempotency key type (hashable, equality-comparable).
 * @tparam Resp The cached response payload.
 * @details Holds at most `capacity` entries; inserting beyond it evicts the least-recently-used
 *          entry. `find` counts as a use (promotes to most-recently-used). Core-local (single
 *          thread) — no locking, matching the actor model. Use it as a responder member.
 */
template <class Key, class Resp>
class dedup_map {
    using list_t = std::list<std::pair<Key, Resp>>; // front = LRU, back = MRU

public:
    /** @brief Create a cache holding at most `capacity` entries (clamped to >= 1). */
    explicit dedup_map(std::size_t capacity = 1024)
        : _cap(capacity ? capacity : std::size_t{1}) {}

    /**
     * @brief Look up `key`; on a hit, promote it to most-recently-used.
     * @return Pointer to the cached response (valid until the next mutating call), or `nullptr`.
     */
    [[nodiscard]] const Resp *
    find(const Key &key) {
        auto it = _index.find(key);
        if (it == _index.end())
            return nullptr;
        _order.splice(_order.end(), _order, it->second); // move node to MRU end
        return &it->second->second;
    }

    /**
     * @brief Insert or update `key → value`, evicting the LRU entry if over capacity.
     */
    void
    put(const Key &key, Resp value) {
        if (auto it = _index.find(key); it != _index.end()) {
            it->second->second = std::move(value);
            _order.splice(_order.end(), _order, it->second);
            return;
        }
        if (_order.size() >= _cap) {
            _index.erase(_order.front().first); // evict LRU
            _order.pop_front();
        }
        _order.emplace_back(key, std::move(value));
        _index.emplace(key, std::prev(_order.end()));
    }

    /** @brief True if `key` is cached (does not promote it). */
    [[nodiscard]] bool
    contains(const Key &key) const noexcept {
        return _index.find(key) != _index.end();
    }

    /** @brief Number of cached entries. */
    [[nodiscard]] std::size_t
    size() const noexcept {
        return _order.size();
    }

    /** @brief Capacity (max entries before eviction). */
    [[nodiscard]] std::size_t
    capacity() const noexcept {
        return _cap;
    }

    /** @brief Drop all cached entries. */
    void
    clear() noexcept {
        _order.clear();
        _index.clear();
    }

private:
    std::size_t                                            _cap;
    list_t                                                 _order;
    std::unordered_map<Key, typename list_t::iterator>     _index;
};

/**
 * @brief Idempotent responder helper — de-duplicates by `e.idempotency_key`, then `answer`s.
 * @ingroup Patterns
 * @tparam E     An `idempotent_event` (a `qb::Request` subtype with an `idempotency_key` field).
 * @tparam Cache A `dedup_map` (or any cache exposing `find(key)->const Resp*` and `put(key, resp)`).
 * @tparam Fn    Callable `Resp(E const&)` computing the response (the side effect) from the request.
 * @param self  The responding actor.
 * @param e     The received request event.
 * @param cache The responder's `dedup_map` keyed by `idempotency_key`.
 * @param fn    Computes the response; **runs at most once per distinct key**.
 * @details
 * Call from the responder's `on(E&)`. It (1) routes any reply to one of `self`'s own pending asks
 * via `resolve_ask(e)`, (2) for a non-default key already in `cache`, replies the cached response
 * **without** running `fn`, otherwise (3) runs `fn`, caches the result under the key, and replies.
 * A default-valued key (`{}`) bypasses the cache (always runs `fn`) — set a stable key to dedup.
 * @code
 * struct Charge : qb::Request<Receipt> { std::uint64_t idempotency_key{}; double amount{}; };
 * // responder:
 * qb::dedup_map<std::uint64_t, Receipt> _seen{4096};
 * void on(Charge &c) { qb::answer_idempotent(*this, c, _seen, [&](Charge const &r){ return charge(r); }); }
 * @endcode
 * @see qb::answer, qb::ask_retry, qb::dedup_map
 */
template <idempotent_event E, class Cache, class Fn>
void
answer_idempotent(qb::Actor &self, E &e, Cache &cache, Fn &&fn) {
    if (self.resolve_ask(e))
        return; // a reply to one of our own asks — already delivered to the coroutine.

    using key_t      = std::decay_t<decltype(e.idempotency_key)>;
    const key_t &key = e.idempotency_key;
    if (key != key_t{}) {                       // a real (non-default) key → eligible for dedup
        if (const auto *cached = cache.find(key)) {
            e.response = *cached;               // replay — do NOT re-run the side effect
            self.reply(e);
            return;
        }
        e.response = std::forward<Fn>(fn)(e);   // first time for this key
        cache.put(key, e.response);
        self.reply(e);
        return;
    }
    e.response = std::forward<Fn>(fn)(e);        // no key → behave like plain answer()
    self.reply(e);
}

} // namespace qb

#endif // QB_CORE_PATTERNS_IDEMPOTENCY_H
