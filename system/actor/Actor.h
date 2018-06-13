
#ifndef CUBE_ACTOR_H
# define CUBE_ACTOR_H
# include <unordered_map>
# include "Event.h"

namespace cube {

    template<typename _Handler>
    class Actor
: public ActorId, public _Handler::IActor, public nocopy {
    using ActorProxy = typename _Handler::ActorProxy;

        class IRegisterEvent {
        public:
            virtual ~IRegisterEvent() {}
            virtual void invoke(Event const *data) const = 0;
        };

        template<typename _Data, typename _Actor>
        class RegisterEvent : public IRegisterEvent {
            _Actor &_actor;
        public:
            RegisterEvent(_Actor &actor)
                    : _actor(actor) {}

            virtual void invoke(Event const *data) const override final {
                auto &event = *reinterpret_cast<_Data const *>(data);
                _actor.onEvent(event);
                if (!event.recycled())
                    event.~_Data();
            }
        };

        _Handler *_handler;
        std::unordered_map<uint32_t, IRegisterEvent const *> _event_map;

        friend _Handler;

        inline void __set_id(ActorId const &id) {
            static_cast<ActorId &>(*this) = id;
        }

        inline ActorProxy proxy() {
            return {id(), this, _handler};
        }

        virtual bool onInit() { return true; }
        virtual void onEvent(Event const *event) override final {
            // TODO: secure this if event not registred
            _event_map[event->id]->invoke(event);
        }

    protected:
        Actor() : _handler(nullptr) {
            _event_map.reserve(32);
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

        template <typename _Actor>
        inline void registerCallBack(_Actor &actor) const {
            _handler->registerCallBack(actor);
        }

        inline void unRegisterCallBack() const {
            _handler->unRegisterCallBack(id());
        }

        inline void kill() const {
            _handler->killActor(id());
        }

        template<typename _Actor, typename ..._Init>
        inline auto addRefActor(_Init const &...init) const {
            return _handler->template addReferencedActor<_Actor, _Init...>(init...);
        }

        template<template <typename __Handler> typename _Actor, typename ..._Init>
        inline auto addRefActor(_Init const &...init) const {
            return _handler->template addReferencedActor<_Actor, _Init...>(init...);
        }

        template<template <typename _Trait, typename __Handler> typename _Actor, typename _Trait, typename ..._Init>
        inline auto addRefActor(_Init const &...init) const {
            return _handler->template addReferencedActor<_Actor, _Trait, _Init...>(init...);
        }


        template<typename _Data, typename ..._Init>
        inline _Data &push(ActorId const &dest, _Init const &...init) const {
            return _handler->template push<_Data>(dest, id(), init...);
        }

        template<typename _Data>
        inline _Data &reply(_Data const &event) const {
            return _handler->template reply<_Data>(event);
        }

        template<typename _Data>
        inline _Data &forward(ActorId const dest, _Data const &event) const {
            return _handler->template forward<_Data>(dest, event);
        }

        template<typename _Data, typename ..._Init>
        inline void send(ActorId const &dest, _Init &&...init) const {
            return _handler->template send<_Data, _Init...>(dest, id(), std::forward<_Init>(init)...);
        }

        inline auto &sharedData() const {
            return _handler->sharedData();
        }

        void onEvent(Event const &event) const {
            LOG_WARN << "Actor[" << _id << "." << _index << "] received removed event[" << event.id << "]";
        }

    };

}

#endif //CUBE_ACTOR_H
