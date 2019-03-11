
#ifndef QB_EVENT_H
# define QB_EVENT_H
# include <utility>
# include <bitset>
// include from qb
# include "ActorId.h"

namespace qb {

    template<typename T>
    struct type {
        constexpr static void id() {}
    };

    template<typename T>
    constexpr uint16_t type_id() { return static_cast<uint16_t>(reinterpret_cast<std::size_t>(&type<T>::id)); }

    /*!
     * @class Event core/Event.h qb/event.h
     * @ingroup Engine
     * @brief Event base class
     */
    class Event {
        friend class Main;
        friend class Core;
        friend class Actor;
        friend class ProxyPipe;
        friend struct ServiceEvent;

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

    /*!
     * @class ServiceEvent core/Event.h qb/event.h
     * @ingroup Engine
     * @brief More flexible Event
     * @details
     * Section in construction
     */
    struct ServiceEvent : public Event {
        ActorId forward;
        uint16_t service_event_id;

        inline void received() {
            std::swap(dest, forward);
            std::swap(id, service_event_id);
            live(true);
        }

        inline void live(bool flag) {
            state[0] = flag;
        }

        inline bool isLive() { return state[0]; }

        inline uint16_t bucketSize() const {
            return bucket_size;
        }
    };

    /*!
     * @class KillEvent core/Event.h qb/event.h
     * @ingroup Engine
     * default registered event to kill Actor by event
     */
    struct KillEvent : public Event {};

} // namespace qb

#endif //QB_EVENT_H
