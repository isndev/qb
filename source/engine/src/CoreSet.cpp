//
// Created by isndev on 12/4/18.
//

#include <cube/engine/CoreSet.h>

namespace cube {

    CoreSet::CoreSet(std::unordered_set <uint8_t> const &set)
            : _raw_set(set)
            ,_nb_core(set.size())
            , _size(*std::max_element(set.cbegin(), set.cend()) + 1) {
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