
#ifndef CUBE_ACTOR_H
# define CUBE_ACTOR_H
# include <unordered_map>

# include "../../utils/nocopy.h"
# include "Event.h"

namespace cube {

    template<typename _Handler>
    class Actor
            : nocopy,
              ActorId,
              public _Handler::IActor {
        using ActorProxy = typename _Handler::ActorProxy;
        friend typename _Handler::base_t;

        _Handler *_handler;
    protected:
        inline void __set_id(ActorId const &id) {
            static_cast<ActorId &>(*this) = id;
        }

        inline ActorProxy proxy() {
            return {id(), this};
        }

        virtual bool onInit() { return true; }

    protected:
        Actor() : ActorId(_Handler::generate_id()), _handler(nullptr) {
            this->template registerEvent<KillEvent>(*this);
            this->template registerEvent<Event>(*this);
        }

        virtual ~Actor() {}

    public:
        inline ActorId id() const {
            return *this;
        }

        inline auto getPipe(ActorId const dest) const {
            return _handler->getProxyPipe(dest, id());
        }

        template <typename _Actor>
        inline void registerCallback(_Actor &actor) const {
            _handler->registerCallback(actor);
        }

        inline void unregisterCallback() const {
            _handler->unregisterCallback(id());
        }

        inline void kill() const {
            _handler->killActor(id());
        }

        template<typename _Actor, typename ..._Init>
        inline auto addRefActor(_Init &&...init) const {
            return _handler->template addReferencedActor<_Actor>(std::forward<_Init>(init)...);
        }

        template< template <typename __Handler> typename _Actor
                , typename ..._Init >
        inline auto addRefActor(_Init &&...init) const {
            return _handler->template addReferencedActor<_Actor>(std::forward<_Init>(init)...);
        }

        template< template<typename __Handler, typename _Trait> typename _Actor
                , typename _Trait
                , typename ..._Init >
        inline auto addRefActor(_Init &&...init) const {
            return _handler->template addReferencedActor<_Actor, _Trait>(std::forward<_Init>(init)...);
        }

        template<typename _Data, typename ..._Init>
        inline _Data &push(ActorId const &dest, _Init const &...init) const {
            return _handler->template push<_Data>(dest, id(), init...);
        }

        inline void reply(Event &event) const {
            _handler->reply(event);
        }

        inline void forward(ActorId const dest, Event &event) const {
            _handler->forward(dest, event);
        }

        inline void send(Event const &event) const {
            _handler->send(event);
        }

        inline bool try_send(Event const &event) const {
            return _handler->try_send(event);
        }

        template<typename _Data, typename ..._Init>
        inline void send(ActorId const &dest, _Init &&...init) const {
            _handler->template send<_Data, _Init...>(dest, id(), std::forward<_Init>(init)...);
        }

        inline auto &sharedData() const {
            return _handler->sharedData();
        }

        inline auto time() const {
            return _handler->time();
        }

        inline uint64_t bestTime() const {
            return _handler->bestTime();
        }

        inline uint32_t bestCore() const {
            return _handler->bestCore();
        }

        void on(Event const &event) const {
            LOG_WARN << "Actor[" << _id << "." << _index << "] received removed event[" << event.id << "]";
        }

        void on(KillEvent const &) {
            kill();
        }

        template<typename _Data>
        inline void unregisterEvent() {
            this->template unregisterEvent<_Data>(*this);
        };

    };

    template <typename _Handler>
    class UserActor : public Actor<_Handler> {
    public:

        UserActor() = delete;
        UserActor(uint32_t const id) {
            this->__set_id(ActorId(id, _Handler::_index));
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
