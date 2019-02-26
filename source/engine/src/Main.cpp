//
// Created by isndev on 12/4/18.
//

#include <cube/engine/Main.h>
#include <cube/engine/Core.h>

namespace cube {

    Main::Main(std::unordered_set<uint8_t> const &core_set)
            : _core_set (core_set)
            , _mail_boxes(_core_set.getSize())
    {
        _cores.reserve(_core_set.getNbCore());
        sync_start.store(0, std::memory_order_release);
        for (auto core_id : core_set) {
            const auto nb_producers = _core_set.getNbCore() - 1;
            _mail_boxes[_core_set.resolve(core_id)] = new MPSCBuffer(nb_producers ? nb_producers : 1);
            _cores.emplace(core_id, new Core(core_id, *this));
        }
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
        if (!source_index)
            source_index = event.dest._index;
        return _mail_boxes[_core_set.resolve(event.dest._index)]->enqueue(source_index,
                                                                          reinterpret_cast<const CacheLine *>(&event),
                                                                          event.bucket_size);
    }

    void Main::start(bool async) const {
		LOG_INFO << "[CUBE] init with " << getNbCore() << " cores";
		std::size_t i = 1;
        for (auto core : _cores) {
            if (!async && i == _cores.size())
                core.second->__spawn__();
            else
                core.second->start();
            ++i;
        }
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

}