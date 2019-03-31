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
    ActorId::ActorId() noexcept : _id(0), _index(0) {}
    ActorId::ActorId(ServiceId const id, CoreId const index) noexcept
            : _id(id), _index(index) {}

    ActorId::ActorId(uint32_t const id) noexcept {
        *reinterpret_cast<uint32_t *>(this) = id;
    }

    ActorId::operator const uint32_t &() const noexcept {
        return *reinterpret_cast<uint32_t const *>(this);
    }

    ServiceId ActorId::sid() const noexcept {
        return _id;
    }

    CoreId ActorId::index() const noexcept {
        return _index;
    }

    bool ActorId::isBroadcast() const noexcept {
        return _id == BroadcastSid;
    }
}

qb::io::stream &operator<<(qb::io::stream &os, qb::ActorId const &id) {
    std::stringstream ss;
    ss << id.index() << "." << id.sid();
    os << ss.str();
    return os;
}
