//
// Created by isndev on 12/4/18.
//

#include <cube/actor/internal/CoreSet.h>

namespace cube {

    CoreSet::CoreSet(std::unordered_set <uint8_t> const &set)
            : _nb_core(set.size()), _size(0) {
        for (auto id : set)
            _size = id > _size ? id : _size;
        ++_size;
        _set.resize(_size);
        uint8_t idx = 0;
        for (auto id : set)
            _set[id] = idx++;
    }

    uint8_t CoreSet::resolve(std::size_t const id) const {
        return _set[id];
    }

    std::size_t CoreSet::getSize() const {
        return _size;
    }

    std::size_t CoreSet::getNbCore() const {
        return _nb_core;
    }

}