//
// Created by isndev on 12/5/18.
//

#include "ActorId.h"

namespace cube {
    ActorId::ActorId() : _id(0), _index(0) {}
    ActorId::ActorId(uint16_t const id, uint16_t const index)
            : _id(id), _index(index) {}

    ActorId::ActorId(uint32_t const id) {
        *reinterpret_cast<uint32_t *>(this) = id;
    }

    ActorId::operator const uint32_t &() const {
        return *reinterpret_cast<uint32_t const *>(this);
    }

    bool ActorId::operator!=(ActorId const &rhs) const {
        return static_cast<uint32_t>(*this) != static_cast<uint64_t>(rhs);
    }
}

cube::io::stream &operator<<(cube::io::stream &os, cube::ActorId const &id) {
    std::stringstream ss;
    ss << id.index() << "." << id.sid();
    os << ss.str();
    return os;
}
