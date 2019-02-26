
#ifndef CUBE_ACTORID_H
# define CUBE_ACTORID_H
# include <limits>
# include <cstdint>
// include from cube
# include <cube/utility/prefix.h>
# include <cube/io.h>

namespace cube {

    /*!
     * @class ActorId engine/ActorId.h cube/actorid.h
     * @ingroup Engine
     * @brief Actor unique identifier
     * @details
     * ActorId is a composition of a Service Index (sid) and Core Index (index).
     */
    class ActorId {
        friend class Main;
        friend class Core;
        friend class Actor;
        friend class ServiceActor;

        uint16_t _id;
        uint16_t _index;

        ActorId(uint16_t const id, uint16_t const index);
        ActorId(uint32_t const id);
    public:
        static constexpr uint32_t NotFound = 0;

        /*!
         * ActorId() == ActorId::NotFound
         */
        ActorId();
        ActorId(ActorId const &) = default;

        operator const uint32_t &() const;
        bool operator!=(ActorId const rhs) const;
        bool operator!=(uint32_t const rhs) const;

        /*!
         * @return Service index
         */
        uint16_t sid() const;
        /*!
         * @return Core index
         */
        uint16_t index() const;
    };

}

cube::io::stream &operator<<(cube::io::stream &os, cube::ActorId const &id);

#endif //CUBE_ACTORID_H
