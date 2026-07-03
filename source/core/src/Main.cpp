/**
 * @file qb/core/src/Main.cpp
 * @brief Implementation of the Main class for the QB Actor Framework
 *
 * This file contains the implementation of the Main class and related components
 * such as CoreInitializer and SharedCoreCommunication which form the foundation
 * of the QB Actor Framework's runtime system.
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

#include <csignal>
#include <iostream>
#include <qb/core/Main.h>
#include <qb/core/VirtualCore.h>
#include <qb/io/async/listener.h>

namespace qb {

// CoreInitializer
CoreInitializer::CoreInitializer(CoreId const index)
    : _index(index)
    // 2.3: `_nb_service` is now a `std::atomic<ServiceId>`. A relaxed load is
    // sufficient here — every writer (see `Actor::registerIndex<Tag>()`) has
    // already published its value through the associated magic-static
    // acquire edge before this constructor runs.
    , _next_id(static_cast<ServiceId>(VirtualCore::_nb_service.load(std::memory_order_relaxed) + 1))
    , _affinity{index}
    , _latency{} {}

CoreInitializer::~CoreInitializer() noexcept {
    clear();
}

void
CoreInitializer::clear() noexcept {
    _next_id = static_cast<ServiceId>(VirtualCore::_nb_service.load(std::memory_order_relaxed) + 1);
    _affinity.clear();
    _actor_factories.clear();
    _registered_services.clear();
}

CoreInitializer::ActorBuilder
CoreInitializer::builder() noexcept {
    return ActorBuilder{*this};
}

CoreInitializer &
CoreInitializer::setAffinity(CoreIdSet const &id) noexcept {
    _affinity = id;
    return *this;
}

CoreInitializer &
CoreInitializer::setLatency(qb::duration const latency) noexcept {
    _latency = latency;
    return *this;
}

CoreId
CoreInitializer::getIndex() const noexcept {
    return _index;
}

CoreIdSet const &
CoreInitializer::getAffinity() const noexcept {
    return _affinity;
}

qb::duration
CoreInitializer::getLatency() const noexcept {
    return _latency;
}

// !CoreInitializer

// CoreInitializer::ActorBuilder

CoreInitializer::ActorBuilder::ActorBuilder(CoreInitializer &initializer) noexcept
    : _initializer(initializer)
    , _valid(true) {}

bool
CoreInitializer::ActorBuilder::valid() const noexcept {
    return _valid;
}
CoreInitializer::ActorBuilder::
operator bool() const noexcept {
    return valid();
}
CoreInitializer::ActorBuilder::ActorIdList
CoreInitializer::ActorBuilder::idList() const noexcept {
    return _ret_ids;
}
// !CoreInitializer::ActorBuilder

static auto
set_from_core_initializers(CoreInitializerMap const &core_initializers) {
    CoreIdSet core_ids;
    for (const auto &index : core_initializers | std::views::keys)
        core_ids.insert(index);
    return core_ids;
}

// SharedCoreCommunication
SharedCoreCommunication::SharedCoreCommunication(CoreInitializerMap const &core_initializers) noexcept
    : _core_set(set_from_core_initializers(core_initializers))
    , _mail_boxes(_core_set.getSize())
    , _core_stopped(_core_set.getSize()) {
    for (auto &flag : _core_stopped)
        flag.store(false, std::memory_order_relaxed); // no core has stopped yet
    for (const auto &[index, initializer] : core_initializers) {
        const auto nb_producers               = _core_set.getNbCore();
        _mail_boxes[_core_set.resolve(index)] = std::make_unique<Mailbox>(nb_producers, initializer.getLatency());
    }
}

SharedCoreCommunication::~SharedCoreCommunication() noexcept = default;

bool
SharedCoreCommunication::send(CoreId const source_index, Event const &event) const noexcept {
    // `source_index` MUST be the PHYSICAL core whose thread is calling (the
    // caller's `_resolved_index`), NOT `_core_set.resolve(event.source.index())`.
    // Each MPSC producer slot is a single-producer ring; deriving it from
    // `event.source` let a `forward()` — which preserves the original sender —
    // write another core's slot concurrently with that core itself, a two-writer
    // data race that tears the ring's write index / bucket bytes.
    const CoreId dest_index = _core_set.resolve(event.dest.index());

    if (static_cast<bool>(_mail_boxes[dest_index]->enqueue(source_index, reinterpret_cast<const EventBucket *>(&event), event.bucket_size))) {
        _mail_boxes[dest_index]->notify();
        return true;
    }

    return false;
}

SharedCoreCommunication::Mailbox &
SharedCoreCommunication::getMailBox(CoreId const id) const noexcept {
    return *_mail_boxes[_core_set.resolve(id)].get();
}

void
SharedCoreCommunication::dispose_residual_mailbox_events() noexcept {
    // Type-erased disposal through the global static registry shared by every router::memh
    // instance; a default-constructed router suffices (it carries no per-core state for the
    // dispose path). Each surviving mailbox event is a byte-copy whose producer abandoned its
    // pipe copy without freeing the payload, so the mailbox copy is the sole owner — dispose
    // it exactly once. consume_all walks the ring storage in place (no scratch buffer).
    router::memh<Event> disposer;
    for (auto &mb : _mail_boxes) {
        if (!mb)
            continue;
        mb->consume_all([&disposer](EventBucket *buffer, std::size_t const nb_buckets) {
            std::size_t i = 0;
            while (i < nb_buckets) {
                auto      &event = *reinterpret_cast<Event *>(buffer + i);
                const auto bsz   = event.bucket_size;
                if (bsz == 0)
                    break; // defensive: malformed event, avoid a zero-stride infinite loop
                disposer.dispose(event);
                i += bsz;
            }
        });
    }
}

CoreId
SharedCoreCommunication::getNbCore() const noexcept {
    return static_cast<CoreId>(_core_set.getNbCore());
}
// !SharedCoreCommunication

static_assert(std::atomic<std::sig_atomic_t>::is_always_lock_free, "Main signal flag must stay lock-free to remain signal-handler-safe");

std::atomic<std::sig_atomic_t> Main::_signal_pending{0};

void
Main::onSignal(int const signum) noexcept {
    _signal_pending.store(signum, std::memory_order_relaxed);
}

Main::Main() noexcept
    : _stop_source()
    , _shared_com(nullptr)
    , _is_running(false) {}

Main::~Main() noexcept {
    if (_is_running) {
        // Ensure every worker receives a stop request before the engine object
        // is torn down. `qb::jthread` destructors will then request_stop() and
        // join automatically, but requesting here shortens the shutdown path
        // for workers that are currently parking on a high-latency mailbox.
        _stop_source.request_stop();
        join();
    }
}

void
Main::start_thread(CoreSpawnerParameter const &params) noexcept {
    auto       &initializer = params.initializer;
    VirtualCore core(initializer.getIndex(), params.shared_com);
    // Wire the engine-wide `qb::stop_token` so `__workflow__` can observe
    // cooperative cancellation requests issued via `qb::stop_source`.
    core.__set_stop_token__(params.stop_token);
    VirtualCore::_handler = &core;
    io::async::init();

    // Publish this core as stopped on EVERY exit from here on — including an
    // exception escaping a callback / IO handler inside __workflow__. Normally
    // __workflow__ marks itself stopped at its tail, but on a throw that tail is
    // skipped, so peers keep treating the crashed core as live and the shutdown
    // residual drain (which waits for every core's stopped flag) hangs
    // Main::join() forever. mark_core_stopped is an idempotent release store on
    // this core's own thread, so the normal in-workflow call is unaffected.
    struct ExitGuard {
        SharedCoreCommunication &com;
        CoreId                   idx;
        ~ExitGuard() {
            com.mark_core_stopped(idx);
            // `core` is a stack local; leaving `_handler` pointing at it dangles
            // once this returns. Matters for start(false), where the caller's own
            // thread ran start_thread and may touch the framework afterwards (an
            // Actor ctor only asserts non-null). Fires on every exit path.
            VirtualCore::_handler = nullptr;
        }
    } exit_guard{params.shared_com, core._resolved_index};

    try {
        // Init VirtualCore
        auto &core_factory = initializer._actor_factories;
        if (!core.__init__(initializer.getAffinity())) {
            LOG_CRIT(core << " Init Failed");
            params.sync_start.store(VirtualCore::Error::BadInit, std::memory_order_release);
        } else if (core_factory.empty()) {
            LOG_CRIT(core << " Started with 0 Actor");
            params.sync_start.store(VirtualCore::Error::NoActor, std::memory_order_release);
        } else if (std::ranges::any_of(
                       core_factory, [&core](auto const &it) { return !core.appendActor(std::unique_ptr<Actor>(it->create())).is_valid(); })) {
            LOG_CRIT("Actor at " << core << " failed to init");
            params.sync_start.store(VirtualCore::Error::BadActorInit, std::memory_order_release);
        } else if (!core.__init__actors__()) {
            LOG_CRIT("Actor at " << core << " failed to init");
            params.sync_start.store(VirtualCore::Error::BadActorInit, std::memory_order_release);
        }
        initializer.clear();
        if (!__wait__all__cores__ready(params.shared_com.getNbCore(), params.sync_start))
            return;
        core.__workflow__();
    } catch (const std::exception &e) {
        LOG_CRIT("Exception thrown on " << core << " what:" << e.what());
        params.sync_start.store(VirtualCore::Error::ExceptionThrown, std::memory_order_release);
        initializer.clear();
    } catch (...) {
        // A non-std::exception throw would otherwise escape this noexcept
        // function and std::terminate — and skip the stopped-flag publish.
        LOG_CRIT("Non-standard exception thrown on " << core);
        params.sync_start.store(VirtualCore::Error::ExceptionThrown, std::memory_order_release);
        initializer.clear();
    }
}

bool
Main::__wait__all__cores__ready(std::size_t const nb_core, std::atomic<uint64_t> &sync_start) noexcept {
    sync_start.fetch_add(1, std::memory_order_acq_rel);
    uint64_t ret = 0;
    do {
        spin_loop_pause();
        ret = sync_start.load(std::memory_order_acquire);
    } while (ret < nb_core);
    return ret < VirtualCore::Error::BadInit;
}

void
Main::setLatency(qb::duration const latency) {
    for (auto &initializer : _core_initializers | std::views::values)
        initializer.setLatency(latency);
}

qb::CoreIdSet
Main::usedCoreSet() const {
    qb::CoreIdSet ret;
    for (const auto &index : _core_initializers | std::views::keys)
        ret.emplace(index);
    return ret;
}

void
Main::start(bool async) noexcept {
    if (_is_running)
        return;
    _sync_start.store(0, std::memory_order_release);
    if (_core_initializers.empty()) {
        _sync_start.store(VirtualCore::Error::BadInit, std::memory_order_release);
        LOG_CRIT("[Start Sequence] Failed: No Core registered");
        return;
    }

    _is_running = true;
    _signal_pending.store(0, std::memory_order_relaxed);
    _shared_com = std::make_unique<SharedCoreCommunication>(_core_initializers);
    _cores.resize(_core_initializers.size());

    auto       i          = 0u;
    const auto stop_token = _stop_source.get_token();
    for (auto &it : _core_initializers) {
        if (!async && i == (_core_initializers.size() - 1)) {
            Main::registerSignal(SIGINT);
            // Synchronous fallback: the caller becomes the last worker. The
            // stop token is still wired so a `request_stop()` on the source
            // (from destruction or a signal-free stop path) cancels it too.
            start_thread({it.first, it.second, *_shared_com, _sync_start, stop_token});
        } else {
            // C++20: `qb::jthread` auto-joins on destruction. The worker is
            // spawned with the stop_token captured by value so the lifetime
            // of the token is tied to each thread — `Main::_stop_source` can
            // request_stop() asynchronously at any moment during `~Main()`
            // or `stop()` without fearing lifetime issues.
            _cores[i] = qb::jthread(
                [params = CoreSpawnerParameter{it.first, it.second, *_shared_com, _sync_start, stop_token}] { start_thread(params); });
        }
        ++i;
    }

    if (async) {
        uint64_t ret = 0;
        do {
            spin_loop_pause();
            ret = _sync_start.load(std::memory_order_acquire);
        } while (ret < _cores.size());
        Main::registerSignal(SIGINT);
    }

    if (hasError()) {
        _is_running = false;
        LOG_CRIT("[Main] Init Failed");
        std::cerr << "CRITICAL: Core Init Failed -> show logs to have more details" << std::endl;
    }
}

bool
Main::hasError() const noexcept {
    return _sync_start.load(std::memory_order_acquire) >= VirtualCore::Error::BadInit;
}

void
Main::stop() noexcept {
    // This path is documented as signal-handler-safe. C++ permits plain
    // lock-free atomic operations in signal handlers; the static_assert above
    // keeps that contract explicit.
    _signal_pending.store(SIGINT, std::memory_order_relaxed);
}

void
Main::join() {
    // C++20: `qb::jthread::join()` may be invoked explicitly; if the caller
    // skips this, the destructor of each element in `_cores` will request
    // a stop and join automatically. We still expose `join()` for users who
    // explicitly want to block until all workers terminate.
    for (auto &core : _cores) {
        if (core.joinable())
            core.join();
    }
    // Every worker has now terminated: it is safe (single-threaded) to free any event left in
    // a mailbox by a peer's final flush that landed after the destination core stopped
    // draining. Idempotent — a second join() (e.g. from ~Main after an explicit join) sweeps
    // already-empty mailboxes.
    if (_shared_com)
        _shared_com->dispose_residual_mailbox_events();
}

CoreInitializer &
Main::core(CoreId const index) {
    if (_is_running)
        throw std::runtime_error("Cannot access to CoreInitializers while engine is running");
    const auto &it = _core_initializers.find(index);
    if (it != _core_initializers.cend())
        return it->second;
    // Reject `qb::NoAffinity`, overflow values, and anything past the bitset
    // capacity used by `CoreSet` / `CoreIdBitSet`. `qb::MaxCores` is the
    // single source of truth (see `qb/include/qb/core/ActorId.h`), so no
    // magic number ever shows up at a call site (finding 2.12).
    if (index >= static_cast<CoreId>(qb::MaxCores))
        throw std::range_error("Max core id managed by qb is " + std::to_string(qb::MaxCores - 1));
    return _core_initializers.emplace(index, index).first->second;
}

namespace {
// Install a signal disposition. On POSIX use sigaction() rather than
// std::signal(): the latter has implementation-defined semantics and, under the
// historical System-V behaviour, resets the handler to SIG_DFL after the first
// delivery — so a second SIGINT/SIGTERM would terminate the process instead of
// triggering the graceful shutdown a second time. sigaction with no
// SA_RESETHAND keeps the handler installed; SA_RESTART avoids spuriously
// failing slow syscalls. On Windows, std::signal is the only portable option.
void
install_signal(int signum, void (*handler)(int)) noexcept {
#if defined(_WIN32) || defined(_WIN64)
    std::signal(signum, handler);
#else
    struct sigaction sa{};
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = (handler == SIG_DFL || handler == SIG_IGN) ? 0 : SA_RESTART;
    ::sigaction(signum, &sa, nullptr);
#endif
}
} // namespace

void
Main::registerSignal(int const signum) noexcept {
    install_signal(signum, &Main::onSignal);
}

void
Main::unregisterSignal(int const signum) noexcept {
    install_signal(signum, SIG_DFL);
}

void
Main::ignoreSignal(int const signum) noexcept {
    install_signal(signum, SIG_IGN);
}

} // namespace qb
