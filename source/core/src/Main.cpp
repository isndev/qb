/**
 * @file qb/core/src/Main.cpp
 * @brief Implementation of the Main class for the QB Actor Framework
 *
 * This file contains the implementation of the Main class and related components
 * such as CoreInitializer and SharedCoreCommunication which form the foundation
 * of the QB Actor Framework's runtime system.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2025 qb - isndev (cpp.actor)
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
#include <qb/core/Main.h>
#include <qb/core/VirtualCore.h>
#include <qb/io/async/listener.h>

namespace qb {

// CoreInitializer
CoreInitializer::CoreInitializer(CoreId const index)
    : _index(index)
    , _next_id(VirtualCore::_nb_service + 1)
    , _affinity{index}
    , _latency(0) {}

CoreInitializer::~CoreInitializer() noexcept {
    clear();
}

void
CoreInitializer::clear() noexcept {
    _next_id = VirtualCore::_nb_service + 1;
    _affinity.clear();
    for (auto factory : _actor_factories)
        delete factory;
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
CoreInitializer::setLatency(uint64_t const latency) noexcept {
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

uint64_t
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
CoreInitializer::ActorBuilder::operator bool() const noexcept {
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
    for (const auto &[index, _] : core_initializers)
        core_ids.insert(index);
    return core_ids;
}

// SharedCoreCommunication
SharedCoreCommunication::SharedCoreCommunication(
    CoreInitializerMap const &core_initializers) noexcept
    : _core_set(set_from_core_initializers(core_initializers))
    , _event_safe_deadlock(_core_set.getNbCore())
    , _mail_boxes(_core_set.getSize()) {
    for (const auto &[index, initializer] : core_initializers) {
        const auto nb_producers = _core_set.getNbCore();
        _mail_boxes[_core_set.resolve(index)] =
            new Mailbox(nb_producers, initializer.getLatency());
    }
}

SharedCoreCommunication::~SharedCoreCommunication() noexcept {
    for (auto mailbox : _mail_boxes) {
        delete mailbox;
    }
}

bool
SharedCoreCommunication::send(Event const &event) const noexcept {
    const CoreId source_index = _core_set.resolve(event.source.index());
    const CoreId dest_index   = _core_set.resolve(event.dest.index());

    if (static_cast<bool>(_mail_boxes[dest_index]->enqueue(
            source_index, reinterpret_cast<const EventBucket *>(&event),
            event.bucket_size))) {
        _mail_boxes[dest_index]->notify();
        return true;
    }

    return false;
}

SharedCoreCommunication::Mailbox &
SharedCoreCommunication::getMailBox(CoreId const id) const noexcept {
    return *_mail_boxes[_core_set.resolve(id)];
}

CoreId
SharedCoreCommunication::getNbCore() const noexcept {
    return static_cast<CoreId>(_core_set.getNbCore());
}
// !SharedCoreCommunication

std::vector<Main *> Main::_instances      = {};
std::mutex          Main::_instances_lock = {};

void
Main::onSignal(int const signum) noexcept {
    std::lock_guard lock(Main::_instances_lock);
    for (auto it : Main::_instances) {
        if (it->_is_running) {
            for (auto core_id : it->_shared_com->_core_set._raw_set) {
                SignalEvent event;
                VirtualCore::fill_event<SignalEvent>(event, BroadcastId(core_id),
                                                     BroadcastId(core_id));
                event.signum = signum;
                while (!it->_shared_com->send(event))
                    spin_loop_pause();
            }
        }
    }
}

Main::Main() noexcept
    : _shared_com(nullptr)
    , _is_running(false) {
    std::lock_guard lock(_instances_lock);
    Main::_instances.push_back(this);
}

Main::~Main() noexcept {
    if (_is_running)
        join();
    std::lock_guard lock(_instances_lock);
    Main::_instances.erase(
        std::find(Main::_instances.cbegin(), Main::_instances.cend(), this));
}

void
Main::start_thread(CoreSpawnerParameter const &params) noexcept {
    auto       &initializer = params.initializer;
    VirtualCore core(initializer.getIndex(), params.shared_com);
    VirtualCore::_handler = &core;
    io::async::init();

    try {
        // Init VirtualCore
        auto &core_factory = initializer._actor_factories;
        if (!core.__init__(initializer.getAffinity())) {
            LOG_CRIT(core << " Init Failed");
            params.sync_start.store(VirtualCore::Error::BadInit,
                                    std::memory_order_release);
        } else if (core_factory.empty()) {
            LOG_CRIT(core << " Started with 0 Actor");
            params.sync_start.store(VirtualCore::Error::NoActor,
                                    std::memory_order_release);
        } else if (std::any_of(core_factory.begin(), core_factory.end(),
                               [&core](auto it) {
                                   return !core.appendActor(*it->create()).is_valid();
                               })) {
            LOG_CRIT("Actor at " << core << " failed to init");
            params.sync_start.store(VirtualCore::Error::BadActorInit,
                                    std::memory_order_release);
        } else if (!core.__init__actors__()) {
            LOG_CRIT("Actor at " << core << " failed to init");
            params.sync_start.store(VirtualCore::Error::BadActorInit,
                                    std::memory_order_release);
        }
        initializer.clear();
        if (!__wait__all__cores__ready(params.shared_com.getNbCore(), params.sync_start))
            return;
        core.__workflow__();
    } catch (const std::exception &e) {
        LOG_CRIT("Exception thrown on " << core << " what:" << e.what());
        params.sync_start.store(VirtualCore::Error::ExceptionThrown,
                                std::memory_order_release);
        initializer.clear();
    }
}

bool
Main::__wait__all__cores__ready(std::size_t const      nb_core,
                                std::atomic<uint64_t> &sync_start) noexcept {
    sync_start.fetch_add(1, std::memory_order_acq_rel);
    uint64_t ret = 0;
    do {
        spin_loop_pause();
        ret = sync_start.load(std::memory_order_acquire);
    } while (ret < nb_core);
    return ret < VirtualCore::Error::BadInit;
}

void
Main::setLatency(uint64_t const latency) {
    for (auto &[_, initializer] : _core_initializers)
        initializer.setLatency(latency);
}

qb::CoreIdSet
Main::usedCoreSet() const {
    qb::CoreIdSet ret;
    for (const auto &[index, _] : _core_initializers)
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
    _shared_com = std::make_unique<SharedCoreCommunication>(_core_initializers);
    _cores.resize(_core_initializers.size());

    auto i = 0u;
    for (auto &it : _core_initializers) {
        if (!async && i == (_core_initializers.size() - 1)) {
            Main::registerSignal(SIGINT);
            start_thread({it.first, it.second, *_shared_com, _sync_start});
        } else
            _cores[i] = std::thread(
                start_thread,
                CoreSpawnerParameter{it.first, it.second, *_shared_com, _sync_start});
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
        std::cerr << "CRITICAL: Core Init Failed -> show logs to have more details"
                  << std::endl;
    }
}

bool
Main::hasError() const noexcept {
    return _sync_start.load(std::memory_order_acquire) >= VirtualCore::Error::BadInit;
}

void
Main::stop() noexcept {
    std::raise(SIGINT);
}

void
Main::join() {
    for (auto &core : _cores) {
        if (core.joinable())
            core.join();
    }
}

CoreInitializer &
Main::core(CoreId const index) {
    if (_is_running)
        throw std::runtime_error(
            "Cannot access to CoreInitializers while engine is running");
    const auto &it = _core_initializers.find(index);
    if (it != _core_initializers.cend())
        return it->second;
    // Todo : not sure to be kept, max core id should be max uint16
    if (index > 255)
        throw std::range_error("Max core id managed by qb is 255");
    return _core_initializers.emplace(index, index).first->second;
}

void
Main::registerSignal(int const signum) noexcept {
    std::signal(signum, &Main::onSignal);
}

void
Main::unregisterSignal(int const signum) noexcept {
    std::signal(signum, SIG_DFL);
}

void
Main::ignoreSignal(int const signum) noexcept {
    std::signal(signum, SIG_IGN);
}

} // namespace qb