
#ifndef CUBE_EVENT_H
# define CUBE_EVENT_H
# include <bitset>
# include "utils/branch_hints.h"
# include "utils/prefix.h"
# include "ActorId.h"

namespace cube {

    template<typename T>
    struct type {
        constexpr static void id() {}
    };

    template<typename T>
    constexpr uint32_t type_id() { return static_cast<uint32_t>(reinterpret_cast<std::size_t>(&type<T>::id)); }

    struct CUBE_LOCKFREE_CACHELINE_ALIGNMENT Event {
    public:
        uint32_t id;
        uint16_t bucket_size;
        std::bitset<16> state;
        // for users
        ActorId dest;
        ActorId source;

    public:
        Event() = default;

        inline ActorId getDestination() const { return dest; }
        inline ActorId getSource() const { return source; }
    };

    struct ServiceEvent : public Event {
        ActorId forward;
        uint32_t service_event_id;
    };

    struct KillEvent : public Event {
    };

}

#endif //CUBE_EVENT_H
