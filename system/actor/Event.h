
#ifndef CUBE_EVENT_H
# define CUBE_EVENT_H
# include "utils/prefix.h"
# include "ActorId.h"

namespace cube {

    struct CUBE_LOCKFREE_CACHELINE_ALIGNMENT Event {
    private:
        uint64_t id;
        ActorId dest;
        ActorId source;
        uint32_t context_size;
        uint16_t bucket_size;
        uint16_t alive;
    public:
        Event() = default;

        ActorId getSource() const { return source; }
        ActorId getDestination() const { return dest; }
        bool recycled() const { return static_cast<bool>(alive); }
    };

    template<typename T>
    struct type {
        static void id() {}
    };

    template<typename T>
    std::size_t type_id() { return reinterpret_cast<size_t>(&type<T>::id); }

    template<typename _Handler, typename _Data, typename ..._Init>
    struct CUBE_LOCKFREE_CACHELINE_ALIGNMENT TEvent : public _Handler, public _Data {
        TEvent() = delete;
        TEvent(TEvent &&) = default;
        TEvent(_Init ...init)
                : _Data(std::forward<_Init>(init)...) {}
    };

}

#endif //CUBE_EVENT_H
