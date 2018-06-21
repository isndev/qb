
#ifndef CUBE_ACTORID_H
# define CUBE_ACTORID_H
# include <utility>
# include <cstdint>

#include "utils/nocopy.h"

namespace cube {

    struct ActorId {
        using NotFound = ActorId;

        uint32_t _id;
        uint32_t _index;
    public:
        ActorId() : _id(0), _index(0) {}
        ActorId(uint32_t const id, uint32_t const index)
                : _id(id), _index(index) {}
        ActorId(ActorId const &) = default;

        ActorId(uint64_t const id) {
            *reinterpret_cast<uint64_t *>(this) = id;
        }

        inline operator const uint64_t &() const {
            return *reinterpret_cast<uint64_t const *>(this);
        }

        inline bool operator!=(ActorId const &rhs) const {
            return static_cast<uint64_t>(*this) != static_cast<uint64_t>(rhs);
        }
    };

    template <typename _Actor, std::size_t CoreIndex>
    struct Tag
    {
      constexpr static ActorId id() { return ActorId(_Actor::Tag, CoreIndex); }
    };
    
}

#endif //CUBE_ACTORID_H
