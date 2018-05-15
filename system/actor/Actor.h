
#ifndef CUBE_ACTOR_H
# define CUBE_ACTOR_H
# include <iostream>
# include <unordered_map>

# include "Types.h"
# include "IActor.h"

namespace cube {

    template<typename _Handler>
    class Actor
            : public ActorId, public IActor, public nocopy {

        class IRegisterEvent {
        public:
            virtual ~IRegisterEvent() {}

            virtual void call(Event const *data) const = 0;
        };

        template<typename _Data, typename _Actor>
        class RegisterEvent : public IRegisterEvent {
            _Actor &_actor;
        public:
            RegisterEvent(_Actor &actor)
                    : _actor(actor) {}

            //virtual ~RegisterEvent(){}

            virtual void call(Event const *data) const override final {
                _actor.onEvent(*reinterpret_cast<_Data const *>(data));
            }
        };

        _Handler *_handler;
        std::unordered_map<uint64_t, IRegisterEvent const *> _event_map;

        friend _Handler;

    protected:
        Actor() : _handler(nullptr) {}

        virtual ~Actor() {}

    public:
        inline ActorId id() const {
            return *this;
        }

        inline ActorProxy proxy() {
            return {id(), this, _handler};
        }

        template<typename _Data, typename _Actor>
        void registerEvent(_Actor &actor) {
            _event_map.insert({type_id<_Data>(), new RegisterEvent<_Data, _Actor>(actor)});
        };


        template<typename _Actor, typename ..._Init>
        auto addRefActor(_Init const &...init) {
            return _handler->template addReferencedActor<_Actor, _Init...>(init...);
        }


        template<typename _Data, typename ..._Init>
        _Data &push(ActorId const &dest, _Init const &...init) {
            using event_t = TEvent<_Data, _Init...>;
            auto event = event_t(init...);
            event.bucket_size = sizeof(event_t) / CUBE_LOCKFREE_CACHELINE_BYTES;
            event.id = type_id<_Data>();
            event.dest = dest;
            event.source = id();
            return _handler->template push<event_t>(event);
        }

//        template<typename _Data, typename ..._Init>
//        void send(ActorId const &dest, _Init const &...init) {
//            _handler->template send<_Data, _Init...>(dest, id(), init...);
//        }

        virtual int init() { return 0; }

        virtual int main() { return 0; }

//        virtual /*int64_t*/void onDestroy() {}

        virtual void hasEvent(Event const *event) override final {
            const auto nb_buckets = event->context_size;
            for (std::size_t i = 0; i < nb_buckets;) {
                // TODO: secure this if event not registred
                _event_map[event->id]->call(event);
                i += event->bucket_size;
                event += event->bucket_size;
            }
        }

    };

}

#endif //CUBE_ACTOR_H
