
#ifndef CUBE_ACTOR_H
# define CUBE_ACTOR_H
# include <unordered_map>
# include "Event.h"

namespace cube {

    template<typename _Handler>
    class Actor
            : nocopy,
              ActorId,
              public _Handler::IActor {
        using ActorProxy = typename _Handler::ActorProxy;

        class IRegisterEvent {
        public:
            virtual ~IRegisterEvent() {}
            virtual void invoke(Event *data) const = 0;
        };

        template<typename _Data, typename _Actor>
        class RegisterEvent : public IRegisterEvent {
            _Actor &_actor;
        public:
            RegisterEvent(_Actor &actor)
                    : _actor(actor) {}

            virtual void invoke(Event *data) const override final {
                auto &event = *reinterpret_cast<_Data *>(data);
                _actor.onEvent(event);
                if (!event.state[0])
                    event.~_Data();
            }
        };

        _Handler *_handler;
        std::unordered_map<uint32_t, IRegisterEvent const *> _event_map;

        friend typename _Handler::base_t;
    public:
        using handler_t = _Handler;
    protected:
        inline void __set_id(ActorId const &id) {
            static_cast<ActorId &>(*this) = id;
        }

        inline ActorProxy proxy() {
            return {id(), this, _handler};
        }

        virtual bool onInit() { return true; }
        virtual void onEvent(Event *event) override final {
            // TODO: secure this if event not registred
            _event_map[event->id]->invoke(event);
        }

    protected:
        Actor() : ActorId(_Handler::generate_id()), _handler(nullptr) {
            _event_map.reserve(64);
            _event_map[type_id<KillEvent>()] = new RegisterEvent<KillEvent, Actor>(*this);
            _event_map[type_id<Event>()] = new RegisterEvent<Event, Actor>(*this);
        }

        virtual ~Actor() {
            for (const auto &revent : _event_map)
                delete revent.second;
        }

    public:
        inline ActorId id() const {
            return *this;
        }

        template<typename _Data, typename _Actor>
        inline void registerEvent(_Actor &actor) {
            auto it = _event_map.find(type_id<_Data>());
            if (it != _event_map.end())
                delete it->second;
            _event_map.insert_or_assign(type_id<_Data>(), new RegisterEvent<_Data, _Actor>(actor));
        };

        template<typename _Data, typename _Actor>
        inline void unRegisterEvent(_Actor &actor) {
            auto it = _event_map.find(type_id<_Data>());
            if (it != _event_map.end())
                delete it->second;
            _event_map.insert_or_assign(type_id<_Data>(), new RegisterEvent<Event, _Actor>(actor));
        };

        template<typename _Data>
        inline void unRegisterEvent() {
            auto it = _event_map.find(type_id<_Data>());
            if (it != _event_map.end())
                delete it->second;
            _event_map.insert_or_assign(type_id<_Data>(), new RegisterEvent<Event, Actor>(*this));
        };

        template <typename _Actor>
        inline void registerCallback(_Actor &actor) const {
            _handler->registerCallback(actor);
        }

        inline void unRegisterCallback() const {
            _handler->unRegisterCallback(id());
        }

        inline void kill() const {
            _handler->killActor(id());
        }

        template<typename _Actor, typename ..._Init>
        inline auto addRefActor(_Init const &...init) const {
            return _handler->template addReferencedActor<_Actor, _Init...>(init...);
        }

        template< template <typename __Handler> typename _Actor
                , typename ..._Init >
        inline auto addRefActor(_Init const &...init) const {
            return _handler->template addReferencedActor<_Actor, _Init...>(init...);
        }

        template< template<typename __Handler, typename _Trait> typename _Actor
                , typename _Trait
                , typename ..._Init >
        inline auto addRefActor(_Init const &...init) const {
            return _handler->template addReferencedActor<_Actor, _Trait, _Init...>(init...);
        }


        template<typename _Data, typename ..._Init>
        inline _Data &push(ActorId const &dest, _Init const &...init) const {
            return _handler->template push<_Data>(dest, id(), init...);
        }

        inline void reply(Event &event) const {
            return _handler->reply(event);
        }

        inline void forward(ActorId const dest, Event &event) const {
            return _handler->forward(dest, event);
        }

        inline void send(Event const &event) const {
            _handler->send(event);
        }

        inline bool try_send(Event const &event) const {
            return _handler->try_send(event);
        }

        template<typename _Data, typename ..._Init>
        inline void send(ActorId const &dest, _Init &&...init) const {
            return _handler->template send<_Data, _Init...>(dest, id(), std::forward<_Init>(init)...);
        }

        inline auto &sharedData() const {
            return _handler->sharedData();
        }

        inline auto getTime() const {
            return _handler->getTime();
        }

        inline uint64_t getBestTime() const {
            return _handler->getBestTime();
        }

        inline uint32_t getBestCore() const {
            return _handler->getBestCore();
        }

        void onEvent(Event const &event) const {
            LOG_WARN << "Actor[" << _id << "." << _index << "] received removed event[" << event.id << "]";
        }

        void onEvent(KillEvent const &) {
            kill();
        }

    };


    template <typename _Handler, uint32_t _Tag>
    class ServiceActor : public Actor<_Handler> {
    public:

        constexpr static const uint32_t Tag = _Tag;

        ServiceActor() {
            this->__set_id(ActorId(_Tag, _Handler::_index));
        }
    };
}

#endif //CUBE_ACTOR_H
