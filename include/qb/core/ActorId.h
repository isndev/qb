
#ifndef QB_ACTORID_H
# define QB_ACTORID_H
# include <limits>
# include <cstdint>
// include from qb
# include <qb/utility/prefix.h>
# include <qb/io.h>

namespace qb {

    /*!
     * @class ActorId core/ActorId.h qb/actorid.h
     * @ingroup Engine
     * @brief Actor unique identifier
     * @details
     * ActorId is a composition of a Service Index (sid) and Core Index (index).
     */
    class ActorId {
        friend class Main;
        friend class Core;
        friend class Actor;

        uint16_t _id;
        uint16_t _index;

        ActorId(uint16_t const id, uint16_t const index);
    public:
        static constexpr uint32_t NotFound = 0;

        /*!
         * ActorId() == ActorId::NotFound
         */
        ActorId();
        ActorId(uint32_t const id);
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

qb::io::stream &operator<<(qb::io::stream &os, qb::ActorId const &id);

#endif //QB_ACTORID_H
