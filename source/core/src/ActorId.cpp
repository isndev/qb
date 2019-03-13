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

#include <qb/core/ActorId.h>

namespace qb {
    ActorId::ActorId() : _id(0), _index(0) {}
    ActorId::ActorId(uint16_t const id, uint16_t const index)
            : _id(id), _index(index) {}

    ActorId::ActorId(uint32_t const id) {
        *reinterpret_cast<uint32_t *>(this) = id;
    }

    ActorId::operator const uint32_t &() const {
        return *reinterpret_cast<uint32_t const *>(this);
    }

    bool ActorId::operator!=(ActorId const rhs) const {
        return static_cast<uint32_t>(*this) != static_cast<uint64_t>(rhs);
    }

    bool ActorId::operator!=(uint32_t const rhs) const {
        return *this != ActorId(rhs);
    }

    uint16_t ActorId::sid() const {
        return _id;
    }

    uint16_t ActorId::index() const {
        return _index;
    }
}

qb::io::stream &operator<<(qb::io::stream &os, qb::ActorId const &id) {
    std::stringstream ss;
    ss << id.index() << "." << id.sid();
    os << ss.str();
    return os;
}
