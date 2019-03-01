
#ifndef CUBE_ACTOR_H
# define CUBE_ACTOR_H
# include <vector>
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
        void __set_id(ActorId const &id);

        mutable bool _alive = true;
        Core * _handler = nullptr;
        std::unordered_map<uint32_t, IRegisteredEvent const *> _event_map;

    protected:
        /*!
         * @name Construction/Destruction
         * @{
         */

        /*!
         * Default constructor
         */
        Actor();

        /*!
         * Virtual destructor
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
         * @attention
         * /!\ If initialization has failed DerivedActor will not be added to the engine
         */
        virtual bool onInit() = 0;

    public:
        /*!
         * Kill the Actor
         */
        void kill() const;

        /*!
         * @}
         */

    protected:
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
         * registerEvent<cube::Event>(*this);
         * // DerivedActor should also define the event callback
         * void on(cube::Event &event) {
         *   // do something before killing actor
         *   kill();
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
         * registerEvent<cube::KillEvent>(*this);
         * // DerivedActor should also define the event callback
         * void on(cube::KillEvent &event) {
         *   // do something before killing actor
         *   kill();
         * }
         * @endcode
         * @attention
         * /!\ Do not forget to call kill on overloaded function.
         */
        void on(KillEvent const &event);

        /*!
         * @}
         */

    public:

        /*!
         * @class EventBuilder engine/ActorId.h cube/actor.h
         * @ingroup Engine
         * @brief Helper to build Events
         */
        class EventBuilder {
            friend class Actor;

            ProxyPipe dest_pipe;

            EventBuilder(ProxyPipe const &pipe);

        public:
            EventBuilder() = delete;
            EventBuilder(EventBuilder const &rhs) = default;

            /*!
             * @brief Send a new ordered event
             * @tparam _Event DerivedEvent type
             * @param args arguments to forward to the constructor of the _Event
             * @return Current EventBuilder
             * @details
             * EventBuilder is given by Actor::to function.\n
             * This function can be chained.\n
             * All events pushed will be received ordered by push order.\n
             * example:
             * @code
             * to(destId)
             * .push<MyEvent1>()
             * .push<MyEvent2>(param1, param2)
             * // ...
             * ;
             * @endcode
             */
            template<typename _Event, typename ..._Args>
            EventBuilder &push(_Args &&...args);
        };

        /*!
         * @name Public Accessors
         * @{
         */

        /*!
         * Get current ActorId
         * @return ActorId
         */
        ActorId id() const { return *this; }

        /*!
         * Get current core index
         * @return core index
         */
        uint16_t getIndex() const;

        /*!
         * @private
         */
        template <typename T>
        ActorId getServiceId(uint16_t const index) const;

        /*!
         * @brief Get current time
         * @return nano timestamp since epoch
         * @details
         * @note
         * This value is optimized and updated each Core loop.
         * @code
         * // ...
         * auto t1 = time();
         * // ... some heavy calculation
         * assert(t1 == time()); // true - will not assert
         * @endcode
         * To get precise time use NanoTimestamp.
         */
        uint64_t time() const;

        /*!
         * @brief Check if Actor is alive
         * @return true if Actor is alive else false
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
         * @brief Register a looped callback
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
         *     registerCallback(*this);
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
         * @brief Unregister actor callback
         * @param actor reference of DerivedActor
         * @details
         * example:
         * @code
         * unregisterCallback(*this);
         * @endcode
         */
        template <typename _Actor>
        void unregisterCallback(_Actor &actor) const;

        /*!
         * @private
         */
        void unregisterCallback() const;


        /*!
         * @brief Actor will listen on new _Event
         * @tparam _Event DerivedEvent type
         * @param actor reference of DerivedActor
         * @details
         * example:
         * @code
         * virtual bool onInit() {
         *   registerEvent<MyEvent>(*this);
         *   // ...
         * }
         * @endcode
         * @note
         * _Actor must define the callback event function.
         * @code
         * void on(MyEvent &event) {
         *   // do something
         * }
         * @endcode
         */
        template<typename _Event, typename _Actor>
        void registerEvent(_Actor &actor);

        /*!
         * @brief Actor will stop listening _Event
         * @tparam _Event DerivedEvent type
         * @param actor reference of DerivedActor
         * @details
         * example:
         * @code
         * unregisterEvent<MyEvent>(*this);
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
         * @brief Get EventBuilder for ActorId destination
         * @param dest ActorId destination
         * @return EventBuilder
         * @details
         * A way to push chained events to destination Actor.\n
         * example:
         * @code
         * to(destId)
         * .push<MyEvent1>()
         * .push<MyEvent2>(param1, param2)
         * // ...
         * ;
         * @endcode
         * @attention
         * @code
         * auto builder1 = main.to(sameid);
         * auto builder2 = main.to(sameid);
         * // builder1 == builder2 -> true
         * @endcode
         */
        EventBuilder to(ActorId const dest) const;

        /*!
         * @brief Send a new ordered event
         * @tparam _Event DerivedEvent type
         * @param dest destination ActorId
         * @param args arguments to forward to the constructor of the _Event
         * @return a reference to the constructed _Event to send
         * @details
         * All events pushed to same actors id in context will be received ordered by push order.\n
         * example:
         * @code
         * // ...
         * auto &e = push<MyEvent>(id_1); // (1) first push
         * e.some_data = 1337; // set my event data without using constructor
         * push<MyEvent>(id_2, param2); // (2) id_2 != id_1 /!\ possible to be received before (1)
         * push<MyEvent>(id_1, param3); // (3) Guaranteed to be received after (1)
         * // ...
         * @endcode
         * @note
         * Pushed events are sent at the end of the core loop.
         * @attention
         * /!\ We recommend to non advanced users to use only this function to send events.
         */
        template<typename _Event, typename ..._Args>
        _Event &push(ActorId const &dest, _Args &&...args) const;

        /*!
         * @brief Send a new unordered event
         * @tparam _Event DerivedEvent type
         * @param dest destination ActorId
         * @param args arguments to forward to the constructor of the _Event
         * @details
         * All events sent using send function are not guaranteed to be received in order.\n
         * example:
         * @code
         * // ...
         * send<MyEvent>(id_1, param1); // (1)
         * send<MyEvent>(id_1, param2); // (2)
         * send<MyEvent>(id_1, param3); // (3)
         * // Actor with id_1 will receive events in random order
         * // ...
         * @endcode
         * @note
         * send may be faster than push in some cases.
         * @attention
         * /!\ We recommend to non advanced users to not use this function to send events.
         */
        template<typename _Event, typename ..._Args>
        void send(ActorId const &dest, _Args &&...args) const;

        /*!
         * @brief Reply an event
         * @param event any received event
         * @details
         * example:
         * @code
         * // ...
         * void on(MyEvent &event) {
         *   // do something...
         *   reply(event);
         * }
         * @endcode
         * @note
         * Replying an event is faster than pushing a new one.
         */
        void reply(Event &event) const;

        /*!
         * @brief Forward an event
         * @param dest destination ActorId
         * @param event any received event
         * @details
         * example:
         * @code
         * // ...
         * void on(MyEvent &event) {
         *   // do something...
         *   forward(id_1, event);
         * }
         * @endcode
         * @note
         * Forwarding an event is faster than pushing a new one.
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
         * @brief Get access to unidirectional out events pipe
         * @param dest destination ActorId
         * @return destination ProxyPipe
         * @details
         * If you want to send several events to same Actor or push dynamic sized events,\n
         * Actor API allows to retrieve a ProxyPipe to a desired Actor.\n
         * more details on ProxyPipe section
         */
        ProxyPipe getPipe(ActorId const dest) const;

        /*!
         * @brief Create new referenced _Actor
         * @tparam _Actor DerivedActor type
         * @param args arguments to forward to the constructor of the _Actor
         * @return _Actor * on success or nullptr on failure
         * @details
         * create and initialize new _Actor on same Core as the callee Actor.\n
         * example:
         * @code
         * auto actor = addRefActor<MyActor>(param1, param2);
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
        _Actor *addRefActor(_Args &&...args) const;

        /*!
         * @}
         */

    };

    /*!
     * @class ServiceActor actor.h cube/actor.h
     * @ingroup Engine
     * @brief SingletonActor base class
     * @details
     * ServiceActor is a special actor where DerivedActor
     * must define unique service index.\n
     * Inherited Service Actors are unique per Core.
     * @attention
     * /!\ Service Index from 0 to 1000 are reserved to Cube.\n
     * /!\ Max Service Index is 10000.
     */
    class ServiceActor : public Actor {
    public:
        ServiceActor() = delete;
        ServiceActor(uint16_t const sid) {
            __set_id(ActorId(sid, 0));
        }
    };
}

cube::io::stream &operator<<(cube::io::stream &os, cube::Actor const &actor);

#endif //CUBE_ACTOR_H
