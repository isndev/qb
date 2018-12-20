
#ifndef CUBE_ACTOR_H
# define CUBE_ACTOR_H
# include <unordered_map>

# include "../../utils/nocopy.h"
# include "ICallback.h"
# include "ProxyPipe.h"
# include "Event.h"

namespace cube {

    class Core;
    class Actor : nocopy
            , public ActorId
    {
        friend class Core;

        class IRegisteredEvent {
        public:
            virtual ~IRegisteredEvent() {}
            virtual void invoke(Event *data) const = 0;
        };

        template<typename _Data, typename _Actor>
        class RegisteredEvent : public IRegisteredEvent {
            _Actor &_actor;
        public:
            RegisteredEvent(_Actor &actor)
                    : _actor(actor) {}

            virtual void invoke(Event *data) const override final {
                auto &event = *reinterpret_cast<_Data *>(data);
                _actor.on(event);
                if (!event.state[0])
                    event.~_Data();
            }
        };


        Core * _handler = nullptr;
        std::unordered_map<uint32_t, IRegisteredEvent const *> _event_map;
    protected:

        void __set_id(ActorId const &id) {
            static_cast<ActorId &>(*this) = id;
        }

    protected:
        Actor();
        virtual ~Actor();
        virtual bool onInit() = 0;

        void on(Event *event) const;
        void on(Event const &event);
        void on(KillEvent const &);

    public:

        ActorId id() const {
            return *this;
        }

        template<typename _Data, typename _Actor>
        void registerEvent(_Actor &actor) {
            auto it = _event_map.find(type_id<_Data>());
            if (it != _event_map.end())
                delete it->second;
            _event_map.insert_or_assign(type_id<_Data>(), new RegisteredEvent<_Data, _Actor>(actor));
        }

        template<typename _Data, typename _Actor>
        void unregisterEvent(_Actor &actor) {
            auto it = _event_map.find(type_id<_Data>());
            if (it != _event_map.end())
                delete it->second;
            _event_map.insert_or_assign(type_id<_Data>(), new RegisteredEvent<Event, _Actor>(actor));
        }

        template<typename _Data>
        void unregisterEvent() {
            this->template unregisterEvent<_Data>(*this);
        }

        uint64_t time() const;
        ProxyPipe getPipe(ActorId const dest) const;
        uint16_t getIndex() const;

        template <typename _Actor>
        void registerCallback(_Actor &actor) const;

        void unregisterCallback() const;
        void kill() const;

        template<typename _Actor, typename ..._Init>
        auto addRefActor(_Init &&...init) const;

        template< template<typename _Trait> typename _Actor
                , typename _Trait
                , typename ..._Init >
        auto addRefActor(_Init &&...init) const;

        template<typename _Data, typename ..._Init>
        _Data &push(ActorId const &dest, _Init const &...init) const;

        template<typename _Data, typename ..._Init>
        _Data &fast_push(ActorId const &dest, _Init const &...init) const;

        template<typename _Data, typename ..._Init>
        void send(ActorId const &dest, _Init &&...init) const;

        void reply(Event &event) const;
        void forward(ActorId const dest, Event &event) const;
        void send(Event const &event) const;
        void push(Event const &event) const;
        bool try_send(Event const &event) const;

        template <typename T>
        ActorId getServiceId(uint16_t const index) const {
            return {T::sid, index};
        }
    };

    class ServiceActor : public Actor {
    public:
        ServiceActor(uint16_t const id) {
            this->__set_id(ActorId(id, 0));
        }
    };
}

cube::io::stream &operator<<(cube::io::stream &os, cube::Actor const &actor);

#endif //CUBE_ACTOR_H
