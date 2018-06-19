
#ifndef CUBE_EVENT_H
# define CUBE_EVENT_H
# include <bitset>
# include "utils/prefix.h"
# include "ActorId.h"

namespace cube {

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

    struct TimedEvent : public Event {
        std::uint32_t save_id;
        std::uint64_t duration;
    };

    struct KillEvent : public Event {
    };

    template<typename T>
    struct type {
       constexpr static void id() {}
    };

    template<typename T>
    constexpr uint32_t type_id() { return static_cast<uint32_t>(reinterpret_cast<std::size_t>(&type<T>::id)); }

}

#endif //CUBE_EVENT_H
