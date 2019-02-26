
#ifndef CUBE_ACTOR_H
# define CUBE_ACTOR_H
# include <unordered_map>
// include from cube
# include <cube/utility/nocopy.h>
# include "ICallback.h"
# include "ProxyPipe.h"
# include "Event.h"

namespace cube {

    class Core;

    /*!
     * @class Actor engine/Actor.h cube/actor.h
     * @ingroup Engine
     * @brief Actor base class
     * @details
     * The Actor sends event messages to be received by another Actor, which is then treated by an Event handler.\n
     * All UserActors should inherit from Actor class.
     */
    class Actor
            : nocopy
            , ActorId
    {
        friend class Core;
        friend class ServiceActor;

        class IRegisteredEvent {
        public:
            virtual ~IRegisteredEvent() {}
            virtual void invoke(Event *data) const = 0;
        };

        template<typename _Event, typename _Actor>
        class RegisteredEvent : public IRegisteredEvent {
            _Actor &_actor;
        public:
            RegisteredEvent(_Actor &actor)
                    : _actor(actor) {}

            virtual void invoke(Event *data) const override final {
                auto &event = *reinterpret_cast<_Event *>(data);
                if (likely(_actor.isAlive()))
                    _actor.on(event);
                if (!event.state[0])
                    event.~_Event();
            }
        };

        void on(Event *event) const;
        void __set_id(ActorId const &id) {
            static_cast<ActorId &>(*this) = id;
        }

        mutable bool _alive = true;
        Core * _handler = nullptr;
        std::unordered_map<uint32_t, IRegisteredEvent const *> _event_map;

    protected:
        /*!
         * @name Construction/Destruction
         * @{
         */

        /*!
         * default constructor
         */
        Actor();

        /*!
         * virtual destructor
         */
        virtual ~Actor();

        /*!
         * @brief DerivedActor should implement this method
         * @return true on init success then false
         * @details
         * example:
         * @code
         * virtual bool onInit() {
         *   // register events and callback here
         *   // can also create actors
         *   // ...
         *   return true
         * }
         * @endcode
         * @note
         * /!\ if initialization has failed DerivedActor will not be added to the engine
         */
        virtual bool onInit() = 0;

        /*!
         * will kill the Actor
         */
        void kill() const;

        /*!
         * @}
         */


        /*!
         * @name Registered Event
         * @{
         */

        /*!
         * @brief Called when received unregistered event
         * @param event received event
         * @details
         * This event can be overloaded by DerivedActor.\n
         * example:
         * @code
         * // onInit()
         * this->template registerEvent<cube::Event>(*this);
         * // DerivedActor should also define the event callback
         * void on(cube::Event &event) {
         *   // do something before killing actor
         *   this->kill();
         * }
         * @endcode
         */
        void on(Event const &event);

        /*!
         * @brief Receiving this event will kill the Actor
         * @param event received event
         * @details
         * This event can be overloaded by DerivedActor.\n
         * example:
         * @code
         * // ...
         * this->template registerEvent<cube::KillEvent>(*this);
         * // DerivedActor should also define the event callback
         * void on(cube::KillEvent &event) {
         *   // do something before killing actor
         *   this->kill();
         * }
         * @endcode
         * @note
         * /!\ do not forget to call kill
         */
        void on(KillEvent const &event);

        /*!
         * @}
         */

    public:

        /*!
         * @name Public Accessors
         * @{
         */

        /*!
         * get current ActorId
         * @return ActorId
         */
        ActorId id() const { return *this; }

        /*!
         * get current core index
         * @return core index
         */
        uint16_t getIndex() const;

        /*!
         * @private
         */
        template <typename T>
        ActorId getServiceId(uint16_t const index) const;

        /*!
         * @brief get current time
         * @return nano timestamp since epoch
         * @details
         * @note
         * This value is optimized and updated each Core loop.
         * @code
         * // ...
         * auto t1 = this->time();
         * // ... some heavy calculation
         * assert(t1 == this->time()); // true - will not assert
         * @endcode
         * To get precise time use NanoTimestamp
         */
        uint64_t time() const;

        /*!
         * @brief check if Actor is alive
         * @return true if Actor is alive then false
         */
        bool isAlive() const;

        /*!
         * @}
         */

        /*!
         * @name Public Member Functions
         * This part describes how to manage Actor loop callback, events registration,
         * several ways to send events and create referenced actors.
         * @{
         */

        /*!
         * @brief will register actor callback
         * @param actor reference of DerivedActor
         * @details
         * The registered callback will be called each Core loop.\n
         * _Actor must inherit and implement ICallback interface.\n
         * example:
         * @code
         * class MyActor
         * : public cube::Actor
         * , public cube::ICallback
         * {
         *   virtual bool onInit() {
         *     this->registerCallback(*this);
         *   }
         * // ...
         *   virtual void onCallback() override final {
         *     // do something
         *   }
         * }
         * // ...
         * }
         * @endcode
         */
        template <typename _Actor>
        void registerCallback(_Actor &actor) const;

        /*!
         * @brief will unregister actor callback
         * @param actor reference of DerivedActor
         * @details
         * example:
         * @code
         * this->unregisterCallback(*this);
         * @endcode
         */
        template <typename _Actor>
        void unregisterCallback(_Actor &actor) const;

        /*!
         * @private
         */
        void unregisterCallback() const;


        /*!
         * @brief actor will listen on new _Event
         * @tparam _Event DerivedEvent type
         * @param actor reference of DerivedActor
         * @details
         * example:
         * @code
         * virtual bool onInit() {
         * this->template registerEvent<MyEvent>(*this);
         * // ...
         * }
         * @endcode
         * @note _Actor must define the callback event function
         * @code
         * void on(MyEvent &event) {
         * // do something
         * }
         * @endcode
         */
        template<typename _Event, typename _Actor>
        void registerEvent(_Actor &actor);

        /*!
         * @brief actor will stop listening _Event
         * @tparam _Event DerivedEvent type
         * @param actor reference of DerivedActor
         * @details
         * example:
         * @code
         * this->template unregisterEvent<MyEvent>(*this);
         * @endcode
         */
        template<typename _Event, typename _Actor>
        void unregisterEvent(_Actor &actor);

        /*!
         * @private
         * @tparam _Event
         */
        template<typename _Event>
        void unregisterEvent();

        /*!
         * @brief send a new ordered event
         * @tparam _Event DerivedEvent type
         * @param dest destination ActorId
         * @param args arguments to forward to the constructor of the _Event
         * @return a reference to the constructed _Event to send
         * @details
         * All events pushed to same actors id in context will be received ordered by push order.\n
         * example:
         * @code
         * // ...
         * auto &e = this->template push<MyEvent>(id_1); // (1) first push
         * e.some_data = 1337; // set my event data without using constructor
         * this->template push<MyEvent>(id_2, param2); // (2) id_2 != id_1 /!\ possible to be received before (1)
         * this->template push<MyEvent>(id_1, param3); // (3) Guaranteed to be received after (1)
         * // ...
         * @endcode
         * @note
         * Pushed events are sent at the end of the core loop\n
         * /!\ We recommend to non advanced users to use only this function to send events
         */
        template<typename _Event, typename ..._Args>
        _Event &push(ActorId const &dest, _Args const &...args) const;

        /*!
         * @private
         * @tparam _Event
         * @param dest
         * @param args
         */
        template<typename _Event, typename ..._Args>
        void fast_push(ActorId const &dest, _Args const &...args) const;

        /*!
         * @brief send a new unordered event
         * @tparam _Event DerivedEvent type
         * @param dest destination ActorId
         * @param args arguments to forward to the constructor of the _Event
         * @details
         * All events sent using send function are not guaranteed to be received in order.\n
         * example:
         * @code
         * // ...
         * this->template send<MyEvent>(id_1, param1); // (1)
         * this->template send<MyEvent>(id_1, param2); // (2)
         * this->template send<MyEvent>(id_1, param3); // (3)
         * // Actor with id_1 will receive events in random order
         * // ...
         * @endcode
         * @note
         * send may be faster than push in some cases\n
         * /!\ We recommend to non advanced users to not use this function to send events
         */
        template<typename _Event, typename ..._Args>
        void send(ActorId const &dest, _Args &&...args) const;

        /*!
         * @brief reply an event
         * @param event any received event
         * @details
         * example:
         * @code
         * // ...
         * void on(MyEvent &event) {
         *   // do something...
         *   this->reply(event);
         * }
         * @endcode
         * @note
         * reply an event is faster than push a new one
         */
        void reply(Event &event) const;

        /*!
         * @brief forward an event
         * @param dest destination ActorId
         * @param event any received event
         * @details
         * example:
         * @code
         * // ...
         * void on(MyEvent &event) {
         *   // do something...
         *   this->forward(id_1, event);
         * }
         * @endcode
         * @note
         * forward an event is faster than push a new one
         */
        void forward(ActorId const dest, Event &event) const;

        /*!
         * @private
         * @param event
         */
        void send(Event const &event) const;

        /*!
         * @private
         * @param event
         */
        void push(Event const &event) const;

        /*!
         * @private
         * @param event
         */
        bool try_send(Event const &event) const;

        /*!
         * @brief get access to unidirectional events pipe
         * @param dest destination ActorId
         * @return destination ProxyPipe
         * @details
         * If you want to send several events to same Actor or push dynamic sized events,\n
         * Actor API allow to users to retrieve an event ProxyPipe to a desired Actor.\n
         * more details on ProxyPipe section
         */
        ProxyPipe getPipe(ActorId const dest) const;

        /*!
         * @brief create new referenced _Actor
         * @tparam _Actor DerivedActor type
         * @param args arguments to forward to the constructor of the _Actor
         * @return _Actor * on success or nullptr on failure
         * @details
         * create and initialize new _Actor on same Core as the callee Actor.\n
         * example:
         * @code
         * auto actor = this->template addRefActor<MyActor>(param1, param2);
         * if (actor) {
         *   // actor was created and pushed to the engine
         * }
         * @endcode
         * @note
         * Referenced actors can be used as a normal C++ class,
         * the parent Actor can make direct calls to created actors "on" functions.\n
         * This is very useful to limit the number of events managed by the engine
         * then improve global performance.
         */
        template<typename _Actor, typename ..._Args>
        auto addRefActor(_Args &&...args) const;

        /*!
         * @brief create new referenced _Actor<_Trait>
         * @tparam _Actor DerivedActor type
         * @tparam _Trait _Actor template argument
         * @param args arguments to forward to the constructor of the _Actor<_Trait>
         * @return _Actor<_Trait> * on success or nullptr on failure
         * @details
         * example:
         * @code
         * auto actor = this->template addRefActor<MyActor, MyTrait>(param1, param2);
         * if (actor) {
         *   // actor was created and pushed to the engine
         * }
         * @endcode
         */
        template< template<typename _Trait> typename _Actor
                , typename _Trait
                , typename ..._Args >
        auto addRefActor(_Args &&...args) const;

        /*!
         * @}
         */

    };

    /*!
     * @class ServiceActor actor.h cube/actor.h
     * @ingroup Engine
     * @brief SingletonActor base class
     * @details
     *
     */
    class ServiceActor : public Actor {
    public:
        ServiceActor() = delete;
        ServiceActor(uint16_t const id) {
            this->__set_id(ActorId(id, 0));
        }
    };
}

cube::io::stream &operator<<(cube::io::stream &os, cube::Actor const &actor);

#endif //CUBE_ACTOR_H
