
#ifndef CUBE_EVENT_H
# define CUBE_EVENT_H
# include "utils/prefix.h"
# include "ActorId.h"

namespace cube {

    struct CUBE_LOCKFREE_CACHELINE_ALIGNMENT Event {
    public:
        uint32_t id;
        uint16_t bucket_size;
        uint16_t alive;
        // for users
        ActorId dest;
        ActorId source;

    public:
        Event() = default;

        inline ActorId getDestination() const { return dest; }
        inline ActorId getSource() const { return source; }
        inline bool recycled() const { return static_cast<bool>(alive); }
    };

    template<typename T>
    struct type {
        static void id() {}
    };

    template<typename T>
    uint32_t type_id() { return static_cast<uint32_t>(reinterpret_cast<std::size_t>(&type<T>::id)); }

    template<typename _Handler, typename _Data, typename ..._Init>
    struct CUBE_LOCKFREE_CACHELINE_ALIGNMENT TEvent : public _Handler, public _Data {
        TEvent() = delete;
        TEvent(TEvent &&) = default;
        TEvent(_Init ...init)
                : _Data(std::forward<_Init>(init)...) {}
    };

}

#endif //CUBE_EVENT_H
