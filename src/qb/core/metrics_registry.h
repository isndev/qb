/**
 * @file qb/core/metrics_registry.h
 * @brief The per-core metrics service and the scrape that gathers every core: `qb::metrics::Registry`,
 *        `qb::metrics::scrape()` (Huly QB-154).
 *
 * Counters stay where they are counted: a core's own `qb::CoreStats`, an HTTP server's request metrics, a Redis or
 * PostgreSQL client's `stats()` -- plain integers of the core that owns them, never shared, never atomic. Nothing
 * runs between two scrapes. A scrape asks the `Registry` of each core, by event, for its snapshot: the registry
 * describes its core's `CoreStats` and calls the sources the actors of its core added (`Registry::add_source`),
 * on that core, then answers. `scrape()` merges the answers; a core that does not answer in time is reported by
 * `qb_scrape_core_up{core="N"} 0` and never fails the scrape. The asks go out from the registry of the scraping
 * actor's core, which receives the answers: the scraping actor needs nothing but that registry.
 *
 * main() adds `qb::metrics::Registry` on every core a scrape asks (`engine.addActor<qb::metrics::Registry>(core)`);
 * an actor publishes its counters through its core's registry, and scrapes from a coroutine:
 * @code
 * struct Jobs : qb::Actor {
 *     std::uint64_t              done = 0;
 *     qb::metrics::source_handle metrics; // removes the source with the actor
 *     qb::io::async::task<bool> onInit() override {
 *         if (auto *registry = getService<qb::metrics::Registry>())
 *             metrics = registry->add_source([this](qb::metrics::snapshot &s) {
 *                 s.counter("app_jobs_total", "Jobs done.").add({}, done);
 *             });
 *         spawn([this](qb::ScopedCoroContext) -> qb::io::async::task<void> {
 *             // reads *this before it suspends, never after
 *             auto snap = co_await qb::metrics::scrape(*this, getCoreSet(), std::chrono::milliseconds(500));
 *             qb::io::cout() << qb::metrics::write_text(snap);
 *         });
 *         co_return true;
 *     }
 * };
 * @endcode
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
 * @ingroup Engine
 */

#ifndef QB_CORE_METRICS_REGISTRY_H
#define QB_CORE_METRICS_REGISTRY_H

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <qb/io/async/coroutine.h>
#include <qb/system/time.h>
#include "Actor.h"
#include "metrics.h"
#include "patterns/request.h"

namespace qb::metrics {

/// A source of samples: fills the snapshot it is handed. Called on its registry's core, during a scrape.
using source = std::function<void(snapshot &)>;

/// What `scrape()` asks a core's `Registry`; the answer is that core's snapshot.
struct CollectRequest : qb::Request<snapshot> {};

namespace detail {
/// The sources of one registry, shared with the handles that remove them.
struct source_list {
    std::vector<std::pair<std::uint64_t, source>> entries;
    std::uint64_t                                 next = 1;
};

/// The service tag of `qb::metrics::Registry`.
struct registry_tag {};
} // namespace detail

/**
 * @brief Owns one source of a `Registry`: removes it when destroyed or `reset()`.
 * @details Move-only. An empty handle (default-constructed, moved-from, or returned for a null source) removes
 *          nothing, and a handle outliving its registry removes nothing either. Destroy it on the registry's
 *          core -- the actor that added the source does, from its own destructor.
 */
class source_handle {
    std::weak_ptr<detail::source_list> _list;
    std::uint64_t                      _id = 0;

public:
    /// An empty handle.
    source_handle() noexcept = default;
    /// The handle of source `id` in `list` (what `Registry::add_source` returns).
    source_handle(std::weak_ptr<detail::source_list> list, std::uint64_t id) noexcept;
    source_handle(source_handle &&other) noexcept;
    /// Removes the source this handle owned, then takes `other`'s.
    source_handle &operator=(source_handle &&other) noexcept;
    source_handle(source_handle const &)            = delete;
    source_handle &operator=(source_handle const &) = delete;
    /// Removes the source.
    ~source_handle();

    /// Removes the source now; the handle is empty afterwards.
    void reset() noexcept;
    /// Whether the handle still owns a source of a living registry.
    [[nodiscard]] bool active() const noexcept;
};

namespace detail {

/**
 * @brief The registry service of one core, for the service tag `Tag` (`qb::metrics::Registry` is the one tag a
 *        program needs; another tag gives an independent set of registries).
 */
template <typename Tag>
class basic_registry : public qb::ServiceActor<Tag> {
    std::shared_ptr<source_list> _sources;
    std::uint64_t                _source_failures = 0;
    bool                         _core_stats;

public:
    /**
     * @brief Constructs the registry of a core.
     * @param core_stats Whether its snapshots begin with the core's own counters (`describe(snapshot&,
     *        CoreStats const&, CoreId)`); on by default.
     */
    explicit basic_registry(bool core_stats = true);

    /// Registers the `CollectRequest` handler.
    qb::io::async::task<bool> onInit() override;

    /**
     * @brief Adds a source, called on this core at every scrape until its handle is destroyed.
     * @param fn Fills the snapshot it is handed. An exception it throws is contained: its samples of that scrape
     *        are dropped and `qb_metrics_source_failures_total{core}` counts it. A null `fn` adds nothing.
     * @return The handle that owns the source.
     */
    [[nodiscard]] source_handle add_source(source fn);

    /// The sources currently added.
    [[nodiscard]] std::size_t source_count() const noexcept;

    /**
     * @brief This core's snapshot: its `CoreStats` (when on), every source, canonicalized, and
     *        `qb_metrics_source_failures_total{core}`. What a `CollectRequest` is answered with.
     */
    [[nodiscard]] snapshot collect() noexcept;

    /// Answers a scrape's request with `collect()`.
    void on(CollectRequest &request) noexcept;
};

/// One registry's answer, or nothing when it did not come in time.
qb::io::async::task<std::optional<snapshot>> collect_from(qb::ScopedCoroContext ctx, qb::ActorId registry, qb::duration timeout);

/// The asks of `scrape()`, from the registry whose context is `ctx`, and the merge.
template <typename RegistryType>
qb::io::async::task<snapshot> run_scrape(qb::ScopedCoroContext ctx, std::vector<qb::CoreId> cores, qb::CoreId here,
                                         std::optional<snapshot> local, qb::duration timeout);

} // namespace detail

/**
 * @brief The metrics service of one core: add one to every core a scrape asks
 *        (`engine.addActor<qb::metrics::Registry>(core)`).
 * @details A `qb::ServiceActor`: its actors reach it with `getService<qb::metrics::Registry>()` (null on a core
 *          that has none) and add their sources; a scrape addresses it by `qb::Actor::getServiceId`. It holds no
 *          timer and receives nothing between scrapes.
 */
class Registry final : public detail::basic_registry<detail::registry_tag> {
public:
    using detail::basic_registry<detail::registry_tag>::basic_registry;
};

/**
 * @brief Gathers the snapshots of the registries of `cores` and merges them.
 * @details The registry of `self`'s core does the asking -- it receives the answers, so `self` registers
 *          nothing -- and its own core's snapshot is taken at once, when `cores` names it. Every other registry
 *          is asked at the same time (`qb::ask`, `timeout` each), and every answer or its timeout is awaited; the
 *          answers merge in the order of `cores`, then `canonicalize()`. Adds `qb_scrape_core_up{core}` (1 when
 *          the core answered, 0 when it did not: no registry there, or a core busy past `timeout`),
 *          `qb_scrape_conflicts` (families or histograms the merge dropped) and `qb_scrape_duration_seconds`.
 *          A core that does not answer never fails the scrape; the scraping registry killed while it waits ends it
 *          with `qb::io::async::cancelled_error`. Call it from a coroutine of an actor and `co_await` it.
 * @tparam RegistryType The registry service (`qb::metrics::Registry`).
 * @param self The scraping actor: its core must have a registry.
 * @param cores The cores to ask.
 * @param timeout How long each core has to answer.
 * @return The merged, canonical snapshot.
 * @throws std::logic_error when `self`'s core has no registry.
 */
template <typename RegistryType = Registry>
[[nodiscard]] qb::io::async::task<snapshot> scrape(qb::Actor const &self, std::vector<qb::CoreId> cores, qb::duration timeout);

/// `scrape()` over a core set -- `getCoreSet()` asks every core of the engine.
template <typename RegistryType = Registry>
[[nodiscard]] qb::io::async::task<snapshot> scrape(qb::Actor const &self, qb::CoreIdSet const &cores, qb::duration timeout);

// ---------------------------------------------------------------------------------------------------------------
// Template bodies.
// ---------------------------------------------------------------------------------------------------------------

namespace detail {

template <typename Tag>
basic_registry<Tag>::basic_registry(bool const core_stats)
    : _sources(std::make_shared<source_list>())
    , _core_stats(core_stats) {}

template <typename Tag>
qb::io::async::task<bool>
basic_registry<Tag>::onInit() {
    this->template registerEvent<CollectRequest>(*this);
    co_return true;
}

template <typename Tag>
source_handle
basic_registry<Tag>::add_source(source fn) {
    if (!fn)
        return {};
    const std::uint64_t id = _sources->next++;
    _sources->entries.emplace_back(id, std::move(fn));
    return source_handle{_sources, id};
}

template <typename Tag>
std::size_t
basic_registry<Tag>::source_count() const noexcept {
    return _sources->entries.size();
}

template <typename Tag>
snapshot
basic_registry<Tag>::collect() noexcept {
    snapshot snap;
    try {
        if (_core_stats)
            describe(snap, this->getCoreStats(), this->getIndex());
        // A copy: a source may add or remove sources while it runs.
        const auto entries = _sources->entries;
        for (auto const &entry : entries) {
            snapshot part;
            try {
                entry.second(part);
                snap.merge(std::move(part));
            } catch (...) {
                ++_source_failures;
            }
        }
        snap.canonicalize();
        snap.counter("qb_metrics_source_failures_total", "Metrics sources that threw while their core's registry collected them.")
            .add({{"core", std::to_string(this->getIndex())}}, _source_failures);
    } catch (...) {
        // Out of memory: what was assembled is the answer.
    }
    return snap;
}

template <typename Tag>
void
basic_registry<Tag>::on(CollectRequest &request) noexcept {
    qb::answer(*this, request, [this](CollectRequest const &) noexcept { return collect(); });
}

template <typename RegistryType>
qb::io::async::task<snapshot>
run_scrape(qb::ScopedCoroContext ctx, std::vector<qb::CoreId> cores, qb::CoreId const here, std::optional<snapshot> local,
           qb::duration const timeout) {
    const qb::mono_time                                       start = qb::mono_now();
    std::vector<qb::io::async::task<std::optional<snapshot>>> calls;
    std::vector<std::size_t>                                  asked; // the index in `cores` of each call
    calls.reserve(cores.size());
    for (std::size_t i = 0; i < cores.size(); ++i) {
        if (cores[i] == here)
            continue; // answered by `local`: a registry asking itself reads its own request as the answer (Huly QB-1100)
        calls.emplace_back(collect_from(ctx, qb::Actor::getServiceId<qb::detail::service_tag_t<RegistryType>>(cores[i]), timeout));
        asked.push_back(i);
    }
    std::vector<std::optional<snapshot>> answers(cores.size());
    if (!calls.empty()) {
        auto remote = co_await qb::io::async::when_all(std::move(calls));
        for (std::size_t k = 0; k < remote.size(); ++k)
            answers[asked[k]] = std::move(remote[k]);
    }
    for (std::size_t i = 0; i < cores.size(); ++i)
        if (cores[i] == here && local)
            answers[i] = *local;

    snapshot          out;
    std::vector<bool> up(cores.size(), false);
    for (std::size_t i = 0; i < answers.size(); ++i) {
        if (answers[i]) {
            up[i] = true;
            out.merge(std::move(*answers[i]));
        }
    }
    out.canonicalize();
    auto &core_up = out.gauge("qb_scrape_core_up", "Whether the core's metrics registry answered the scrape in time.");
    for (std::size_t i = 0; i < cores.size(); ++i)
        core_up.add({{"core", std::to_string(cores[i])}}, up[i] ? 1 : 0);
    out.gauge("qb_scrape_conflicts", "Families or histograms this scrape's merge dropped: one name with two types, or two bucket layouts.")
        .add({}, out.conflicts());
    out.gauge("qb_scrape_duration_seconds", "How long the scrape took.").add({}, std::chrono::duration<double>(qb::mono_now() - start).count());
    co_return out;
}

} // namespace detail

template <typename RegistryType>
qb::io::async::task<snapshot>
scrape(qb::Actor const &self, std::vector<qb::CoreId> cores, qb::duration const timeout) {
    auto *registry = self.template getService<RegistryType>();
    if (!registry)
        throw std::logic_error("qb::metrics::scrape: the scraping actor's core has no metrics registry");
    const qb::CoreId        here = self.getIndex();
    std::optional<snapshot> local;
    for (auto const core : cores) {
        if (core == here) {
            local = registry->collect();
            break;
        }
    }
    return detail::run_scrape<RegistryType>(registry->context(), std::move(cores), here, std::move(local), timeout);
}

template <typename RegistryType>
qb::io::async::task<snapshot>
scrape(qb::Actor const &self, qb::CoreIdSet const &cores, qb::duration const timeout) {
    return scrape<RegistryType>(self, cores.to_vector(), timeout);
}

} // namespace qb::metrics

#endif // QB_CORE_METRICS_REGISTRY_H
