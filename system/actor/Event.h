
#ifndef CUBE_EVENT_H
# define CUBE_EVENT_H
# include <utility>
# include <bitset>

# include "../../utils/prefix.h"
# include "ActorId.h"

namespace cube {

    template<typename T>
    struct type {
        constexpr static void id() {}
    };

    template<typename T>
    constexpr uint16_t type_id() { return static_cast<uint16_t>(reinterpret_cast<std::size_t>(&type<T>::id)); }

    struct CUBE_LOCKFREE_CACHELINE_ALIGNMENT Event {
    public:
        uint16_t id;
        uint16_t bucket_size;
        std::bitset<32> state;
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
        uint16_t service_event_id;

        inline void received() {
            std::swap(dest, forward);
            std::swap(id, service_event_id);
        }
    };

    struct KillEvent : public Event {
    };

}

#endif //CUBE_EVENT_H
