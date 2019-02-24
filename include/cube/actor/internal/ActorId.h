
#ifndef CUBE_ACTORID_H
# define CUBE_ACTORID_H
# include <limits>
# include <cstdint>
// include from cube
# include <cube/utility/prefix.h>
# include <cube/actor/io.h>

namespace cube {

    struct CUBE_LOCKFREE_CACHELINE_ALIGNMENT CacheLine {
        uint32_t __raw__[16];
    };

    struct ActorId {
        friend class Main;
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

        uint16_t sid() const {
            return _id;
        }

        uint16_t index() const {
            return _index;
        }
    };

}

cube::io::stream &operator<<(cube::io::stream &os, cube::ActorId const &id);

#endif //CUBE_ACTORID_H
