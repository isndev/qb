//
// Created by isndev on 12/4/18.
//

#include <csignal>
#include <cstring>
#include <cube/core/Main.h>
#include <cube/core/Core.h>

namespace qb {

    void Main::onSignal(int signal) {
        io::cout() << "Received signal(" << signal << ") will stop the engine" << std::endl;
        is_running = false;
    }

    void Main::__init__() {
        _cores.reserve(_core_set.getNbCore());
        for (auto core_id : _core_set._raw_set) {
            const auto nb_producers = _core_set.getNbCore();
            _mail_boxes[_core_set.resolve(core_id)] = new MPSCBuffer(nb_producers);
            _cores.emplace(core_id, new Core(core_id, *this));
        }
        sync_start.store(0, std::memory_order_release);
        is_running = false;
        LOG_INFO << "[MAIN] Init with " << getNbCore() << " cores";
    }

    Main::Main(CoreSet const &core_set)
            : _core_set (core_set)
            , _mail_boxes(_core_set.getSize())
    {
        __init__();
    }

    Main::Main(std::unordered_set<uint8_t> const &core_set)
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

		for (auto core : _cores)
			delete core.second;

    }

    bool Main::send(Event const &event) const {
        uint16_t source_index = _core_set.resolve(event.source._index);

        return _mail_boxes[_core_set.resolve(event.dest._index)]->enqueue(source_index,
                                                                          reinterpret_cast<const CacheLine *>(&event),
                                                                          event.bucket_size);
    }

    void Main::start(bool async) const {
		std::size_t i = 1;
		is_running = true;
        for (auto core : _cores) {
            if (!async && i == _cores.size())
                core.second->__spawn__();
            else
                core.second->start();
            ++i;
        }

        uint64_t ret = 0;
        do {
            std::this_thread::yield();
            ret = sync_start.load(std::memory_order_acquire);
        }
        while (ret < _cores.size());
        if (ret < Core::Error::BadInit) {
            LOG_INFO << "[Main] Init Success";
            std::signal(SIGINT, &onSignal);
        } else {
            LOG_CRIT << "[Main] Init Failed";
            io::cout() << "CRITICAL: Engine Init Failed -> show logs to have more details" << std::endl;
        }
    }

    bool Main::hasError() {
        return sync_start.load(std::memory_order_acquire) >= Core::Error::BadInit;
    }

    void Main::stop() {
        if (is_running)
            std::raise(SIGINT);
    }

    void Main::join() const {
        for (auto core : _cores)
            core.second->join();
    }

    Main::MPSCBuffer &Main::getMailBox(uint8_t const id) const {
        return *_mail_boxes[_core_set.resolve(id)];
    }

    std::size_t Main::getNbCore() const {
        return _core_set.getNbCore();
    }

    std::atomic<uint64_t> Main::sync_start(0);
    bool Main::is_running(false);

    Main::CoreBuilder::CoreBuilder(CoreBuilder const &rhs)
            : _index(rhs._index)
            , _main(rhs._main)
            , _ret_ids(std::move(rhs._ret_ids))
            , _valid(rhs._valid)
    {}

    bool Main::CoreBuilder::valid() const { return _valid; }
    Main::CoreBuilder::operator bool() const { return valid(); }
    Main::CoreBuilder::ActorIdList const &Main::CoreBuilder::idList() const {
        return _ret_ids;
    }

    Main::CoreBuilder Main::core(uint16_t const index) {
        return {*this, index};
    }

}