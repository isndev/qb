
#ifndef CUBE_ACTORID_H
# define CUBE_ACTORID_H
# include <limits>
# include <cstdint>

# include "../io.h"

namespace cube {

    struct ActorId {
        friend class Cube;
        friend class Core;
        friend class Actor;
        friend class ServiceActor;

        using NotFound = ActorId;

        uint16_t _id;
        uint16_t _index;

        ActorId(uint16_t const id, uint16_t const index);
        ActorId(uint32_t const id);
    public:
        ActorId();
        ActorId(ActorId const &) = default;

        operator const uint32_t &() const;
        bool operator!=(ActorId const &rhs) const;
    };

}

#endif //CUBE_ACTORID_H
