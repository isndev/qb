
#ifndef CUBE_ACTORID_H
# define CUBE_ACTORID_H
# include <limits>
# include <cstdint>

# include "../io.h"

namespace cube {

    struct ActorId {
        using NotFound = ActorId;

        uint16_t _id;
        uint16_t _index;
    public:
        ActorId() : _id(0), _index(0) {}
        ActorId(uint16_t const id, uint16_t const index)
                : _id(id), _index(index) {}
        ActorId(ActorId const &) = default;

        ActorId(uint32_t const id) {
            *reinterpret_cast<uint32_t *>(this) = id;
        }

        inline operator const uint32_t &() const {
            return *reinterpret_cast<uint32_t const *>(this);
        }

        inline bool operator!=(ActorId const &rhs) const {
            return static_cast<uint32_t>(*this) != static_cast<uint64_t>(rhs);
        }
    };

    class Event;
    class GenericHandler {
    public:
        class BaseActor {
        public:
            virtual ~BaseActor() {}

            virtual bool onInit() = 0;
            virtual void on(Event *) = 0;
        };

        class ICallback {
        public:
            virtual ~ICallback() {}
            virtual void onCallback() = 0;
        };

        struct Pipe{};
        struct ActorProxy {};

        using base_t = int;
    };

    template <template <typename Handler> typename _Actor, std::size_t CoreIndex = std::numeric_limits<std::size_t >::max()>
    struct Tag
    {
      constexpr static ActorId id() { return ActorId(_Actor<GenericHandler>::Tag, CoreIndex); }
    };

    template <template <typename Handler> typename _Actor>
    struct Tag<_Actor, std::numeric_limits<std::size_t >::max()>
    {
        constexpr static ActorId id(std::size_t const CoreIndex) { return ActorId(_Actor<GenericHandler>::Tag, CoreIndex); }
    };

}

#endif //CUBE_ACTORID_H
