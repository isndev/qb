/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2019 isndev (www.qbaf.io). All rights reserved.
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
#include <cstring>
#include <qb/core/Main.h>
#include <qb/core/VirtualCore.h>

namespace qb {

    void Main::onSignal(int signal) {
        io::cout() << "Received signal(" << signal << ") will stop the engine" << std::endl;
        is_running = false;
    }

    void Main::__init__() noexcept {
        _cores.resize(_core_set.getNbCore());
        for (auto core_id : _core_set._raw_set) {
            const auto nb_producers = _core_set.getNbCore();
            _mail_boxes[_core_set.resolve(core_id)] = new MPSCBuffer(nb_producers);
        }
        sync_start.store(0, std::memory_order_release);
        is_running = false;
        generated_sid = VirtualCore::_nb_service + 1;
        LOG_INFO("[MAIN] Init with " << getNbCore() << " cores");
    }

    Main::Main(CoreSet const &core_set) noexcept
            : _core_set (core_set)
            , _mail_boxes(_core_set.getSize())
    {
        __init__();
    }

    Main::Main(std::unordered_set<CoreId> const &core_set) noexcept
            : _core_set (core_set)
            , _mail_boxes(_core_set.getSize())
    {
        __init__();
    }

    Main::~Main() {
        for (auto mailbox : _mail_boxes) {
            if (mailbox)
                delete mailbox;
        }
        for (auto &actor_factory : _actor_factories)
            for (auto factory : actor_factory.second)
                delete factory.second;
    }

    bool Main::send(Event const &event) const noexcept {
        CoreId source_index = _core_set.resolve(event.source._index);

        return _mail_boxes[_core_set.resolve(event.dest._index)]->enqueue(source_index,
                                                                          reinterpret_cast<const CacheLine *>(&event),
                                                                          event.bucket_size);
    }

    void Main::start_thread(CoreId coreId, Main &engine) noexcept {
        VirtualCore core(coreId, engine);
        VirtualCore::_handler = &core;
        try {
            // Init VirtualCore
            auto &core_factory = engine._actor_factories[coreId];
            core.__init__();
            if (!core_factory.size()) {
                LOG_CRIT("" << core << " Started with 0 Actor");
                Main::sync_start.store(VirtualCore::Error::NoActor, std::memory_order_release);
                return;
            }
            else if (std::any_of(core_factory.begin(), core_factory.end(),
                    [&core](auto it) {
                        return !core.appendActor(*it.second->create(), it.second->isService());
                    }))
            {
                LOG_CRIT("Actor at " << core << " failed to init");
                Main::sync_start.store(VirtualCore::Error::BadActorInit, std::memory_order_release);
            }
            core.__init__actors__();
            if (!core.__wait__all__cores__ready())
                return;
            core.__workflow__();
        } catch (std::exception &e) {
			(void)e;
            LOG_CRIT("Exception thrown on " << core << " what:" << e.what());
            Main::sync_start.store(VirtualCore::Error::ExceptionThrown, std::memory_order_release);
            Main::is_running = false;
        }
    }

    void Main::start(bool async) {
		is_running = true;
		auto i = 0u;
		for (auto &coreId : _core_set.raw()) {
		    const auto index = _core_set.resolve(coreId);
            if (!async && i == (_core_set.getNbCore() - 1))
                start_thread(coreId, *this);
            else
                _cores[index] = std::thread(start_thread, coreId, std::ref(*this));
            ++i;
		}

        uint64_t ret = 0;
        do {
            std::this_thread::yield();
            ret = sync_start.load(std::memory_order_acquire);
        }
        while (ret < _cores.size());
        if (ret < VirtualCore::Error::BadInit) {
            LOG_INFO("[Main] Init Success");
            std::signal(SIGINT, &onSignal);
        } else {
            LOG_CRIT("[Main] Init Failed");
            io::cout() << "CRITICAL: Core Init Failed -> show logs to have more details" << std::endl;
        }
    }

    bool Main::hasError() noexcept{
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

    Main::MPSCBuffer &Main::getMailBox(CoreId const id) const noexcept {
        return *_mail_boxes[_core_set.resolve(id)];
    }

    CoreId Main::getNbCore() const noexcept {
        return static_cast<CoreId>(_core_set.getNbCore());
    }

    std::atomic<uint64_t> Main::sync_start(0);
    bool Main::is_running(false);
    ServiceId Main::generated_sid = 0;

    Main::CoreBuilder::CoreBuilder(CoreBuilder const &rhs) noexcept
            : _index(rhs._index)
            , _main(rhs._main)
            , _ret_ids(std::move(rhs._ret_ids))
            , _valid(rhs._valid)
    {}

    bool Main::CoreBuilder::valid() const noexcept { return _valid; }
    Main::CoreBuilder::operator bool() const noexcept { return valid(); }
    Main::CoreBuilder::ActorIdList const &Main::CoreBuilder::idList() const noexcept {
        return _ret_ids;
    }

    Main::CoreBuilder Main::core(CoreId const index) noexcept {
        return {*this, index};
    }

}