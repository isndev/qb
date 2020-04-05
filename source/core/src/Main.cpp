/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2020 isndev (www.qbaf.io). All rights reserved.
 *
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
 *         limitations under the License.
 */

#include <csignal>
#include <qb/core/Main.h>
#include <qb/core/VirtualCore.h>
#include <qb/io/async/listener.h>

namespace qb {

// CoreInitializer
CoreInitializer::CoreInitializer(CoreId index)
    : _index(index)
    , _next_id(VirtualCore::_nb_service + 1)
    , _low_latency(true) {}

CoreInitializer::~CoreInitializer() noexcept {
    clear();
}

void CoreInitializer::clear() noexcept {
    _next_id = VirtualCore::_nb_service + 1;
    for (auto factory : _actor_factories)
        delete factory;
    _actor_factories.clear();
    _registered_services.clear();
}

CoreInitializer::ActorBuilder CoreInitializer::builder() noexcept {
    return ActorBuilder{*this};
}

CoreInitializer &CoreInitializer::setLowLatency(bool state) noexcept {
    _low_latency = state;
    return *this;
}

bool CoreInitializer::isLowLatency() const noexcept {
    return _low_latency;
}

// !CoreInitializer

// CoreInitializer::ActorBuilder

CoreInitializer::ActorBuilder::ActorBuilder(CoreInitializer &initializer) noexcept
    : _initializer(initializer)
    , _valid(true) {}

bool CoreInitializer::ActorBuilder::valid() const noexcept {
    return _valid;
}
CoreInitializer::ActorBuilder::operator bool() const noexcept {
    return valid();
}
CoreInitializer::ActorBuilder::ActorIdList CoreInitializer::ActorBuilder::idList() const noexcept {
    return _ret_ids;
}
// !CoreInitializer::ActorBuilder

// SharedCoreCommunication
SharedCoreCommunication::SharedCoreCommunication(qb::unordered_set<CoreId> const &set) noexcept
    : _core_set(set)
    , _event_safe_deadlock(_core_set.getNbCore())
    , _mail_boxes(_core_set.getSize()) {
    for (auto core_id : set) {
        const auto nb_producers = _core_set.getNbCore();
        _mail_boxes[_core_set.resolve(core_id)] = new MPSCBuffer(nb_producers);
    }
}

SharedCoreCommunication::~SharedCoreCommunication() noexcept {
    for (auto mailbox : _mail_boxes) {
        delete mailbox;
    }
}

bool SharedCoreCommunication::send(Event const &event) const noexcept {
    CoreId source_index = _core_set.resolve(event.source._index);

    return static_cast<bool>(_mail_boxes[_core_set.resolve(event.dest._index)]->enqueue(
        source_index, reinterpret_cast<const EventBucket *>(&event), event.bucket_size));
}

SharedCoreCommunication::MPSCBuffer &SharedCoreCommunication::getMailBox(CoreId const id) const
    noexcept {
    return *_mail_boxes[_core_set.resolve(id)];
}

CoreId SharedCoreCommunication::getNbCore() const noexcept {
    return static_cast<CoreId>(_core_set.getNbCore());
}
// !SharedCoreCommunication

void Main::onSignal(int signal) {
    io::cout() << "Received signal(" << signal << ") will stop the engine" << std::endl;
    is_running = false;
}

Main::Main() noexcept {
    is_running = false;
}

Main::~Main() {
    if (is_running)
        join();
    delete _shared_com;
}

void Main::start_thread(CoreSpawnerParameter const &params) noexcept {
    VirtualCore core(params.id, params.shared_com);
    VirtualCore::_handler = &core;
    io::async::init();
    auto &initializer = params.initializer;
    core.setLowLatency(initializer.isLowLatency());
    try {
        // Init VirtualCore
        auto &core_factory = initializer._actor_factories;
        core.__init__();
        if (core_factory.empty()) {
            LOG_CRIT("" << core << " Started with 0 Actor");
            Main::sync_start.store(VirtualCore::Error::NoActor, std::memory_order_release);
            return;
        } else if (std::any_of(core_factory.begin(), core_factory.end(), [&core](auto it) {
                       return !core.appendActor(*it->create()).is_valid();
                   })) {
            LOG_CRIT("Actor at " << core << " failed to init");
            Main::sync_start.store(VirtualCore::Error::BadActorInit, std::memory_order_release);
        } else
            core.__init__actors__();
        initializer.clear();
        if (!core.__wait__all__cores__ready())
            return;
        core.__workflow__();
    } catch (std::exception &e) {
        (void)e;
        LOG_CRIT("Exception thrown on " << core << " what:" << e.what());
        Main::sync_start.store(VirtualCore::Error::ExceptionThrown, std::memory_order_release);
        Main::is_running = false;
        initializer.clear();
    }
}

void Main::start(bool async) {
    uint64_t ret = 0;
    if (!_core_initializers.empty()) {
        LOG_INFO("[MAIN] Init with " << _core_initializers.size() << " cores");
        is_running = true;
        sync_start.store(0, std::memory_order_release);

        qb::unordered_set<CoreId> core_ids;
        for (const auto &initializer : _core_initializers)
            core_ids.insert(initializer.first);
        _cores.resize(_core_initializers.size());
        _shared_com = new SharedCoreCommunication(core_ids);

        auto i = 0u;
        for (auto &it : _core_initializers) {
            if (!async && i == (_core_initializers.size() - 1))
                start_thread({it.first, it.second, *_shared_com});
            else
                _cores[i] = std::thread(start_thread,
                                        CoreSpawnerParameter{it.first, it.second, *_shared_com});
            ++i;
        }

        do {
            std::this_thread::yield();
            ret = sync_start.load(std::memory_order_acquire);
        } while (ret < _cores.size());
    } else {
        LOG_CRIT("[Main] Cannot start engine with 0 Actor");
        Main::sync_start.store(VirtualCore::Error::NoActor, std::memory_order_release);
        ret = VirtualCore::Error::NoActor;
    }

    if (ret < VirtualCore::Error::BadInit) {
        LOG_INFO("[Main] Init Success");
        std::signal(SIGINT, &onSignal);
    } else {
        is_running = false;
        LOG_CRIT("[Main] Init Failed");
        io::cout() << "CRITICAL: Core Init Failed -> show logs to have more details" << std::endl;
    }
}

bool Main::hasError() noexcept {
    return sync_start.load(std::memory_order_acquire) >= VirtualCore::Error::BadInit;
}

void Main::stop() noexcept {
    if (is_running)
        std::raise(SIGINT);
}

void Main::join() {
    for (auto &core : _cores) {
        if (core.joinable())
            core.join();
    }
}

std::atomic<uint64_t> Main::sync_start(0);
bool Main::is_running(false);

CoreInitializer &Main::core(CoreId const index) {
    if (is_running)
        throw std::runtime_error("Cannot access to CoreInitializers while engine is running");
    const auto &it = _core_initializers.find(index);
    if (it != _core_initializers.cend())
        return it->second;
    // Todo : not sure to be kept, max core id should be max uint16
    if (index > 255)
        throw std::range_error("Max core id managed by qb is 255");
    return _core_initializers.emplace(index, index).first->second;
}

} // namespace qb