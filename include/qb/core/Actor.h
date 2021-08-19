/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2020 isndev (www.qbaf.io). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 *         limitations under the License.
 */

#ifndef QB_ACTOR_H
#define QB_ACTOR_H
#include <map>
#include <tuple>
#include <utility>
#include <vector>
// include from qb
#include "Event.h"
#include "ICallback.h"
#include "Pipe.h"
#include <qb/system/container/unordered_map.h>
#include <qb/utility/nocopy.h>
#include <qb/utility/type_traits.h>

#ifdef __GNUG__
#include <cstdlib>
#include <memory>
#include <cxxabi.h>
#endif

namespace qb {

class VirtualCore;
class ActorProxy;
class Service;

/*!
 * @class Actor core/Actor.h qb/actor.h
 * @ingroup Core
 * @brief Actor base class
 * @details
 * The Actor sends event messages to be received by another Actor, which is then treated
 * by an Event handler.\n All UserActors should inherit from Actor class.
 */
class Actor : nocopy {
    friend class VirtualCore;
    friend class ActorProxy;
    friend class Service;

    const char *name = "unnamed";
    ActorId _id;
    mutable bool _alive = true;
    std::uint32_t id_type = 0u;

    /*!
     * @private
     * @tparam _Type
     */
    template <typename _Type>
    bool require_type() const noexcept;

    explicit Actor(ActorId id) noexcept;

protected:
    /*!
     * @private
     */
    template <typename Tag>
    static ServiceId registerIndex() noexcept;

    /*!
     * @name Construction/Destruction
     * @{
     */

    /*!
     */
    Actor() noexcept;

    /*!
     */
    virtual ~Actor() noexcept = default;

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
    virtual bool
    onInit() {
        return true;
    };

public:
    /*!
     * Kill the Actor
     */
    void kill() const noexcept;

    /*!
     * @}
     */

public:
    /*!
     * @name Registered Event
     * @{
     */

    /*!
     * @brief Receiving this event will kill the Actor
     * @param event received event
     * @details
     * This event can be overloaded by DerivedActor.\n
     * example:
     * @code
     * // ...
     * registerEvent<qb::KillEvent>(*this);
     * // DerivedActor should also define the event callback
     * void on(qb::KillEvent const &event) {
     *   // do something before killing actor
     *   kill();
     * }
     * @endcode
     * @attention
     * /!\ Do not forget to call kill on overloaded function.
     */
    void on(KillEvent const &event) noexcept;

    /*!
     * @brief Receiving signal
     * @param event received event
     * @details
     * This event can be overloaded by DerivedActor.\n
     * example:
     * @code
     * // ...
     * registerEvent<qb::SignalEvent>(*this);
     * // DerivedActor should also define the event callback
     * // ex : default behaviour
     * void on(qb::SignalEvent const &event) {
     *   // do something before killing actor
     *   if (event.signum == SIGINT)
     *      kill();
     * }
     * @endcode
     * @attention
     * /!\ by default has same behaviour than KillEvent.
     */
    void on(SignalEvent const &event) noexcept;

    /*!
     * @brief Receiving this event will unregister the Actor's callback
     * @param event received event
     * @attention
     * /!\ Do not overload this function.
     */
    void on(UnregisterCallbackEvent const &event) noexcept;

    void on(PingEvent const &event) noexcept;

    /*!
     * @}
     */

public:
    /*!
     * @class EventBuilder core/ActorId.h qb/actor.h
     * @brief Helper to build Events
     */
    class EventBuilder {
        friend class Actor;
        Pipe dest_pipe;

        explicit EventBuilder(Pipe const &pipe) noexcept;

    public:
        EventBuilder() = delete;
        EventBuilder(EventBuilder const &rhs) noexcept = default;

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
        template <typename _Event, typename... _Args>
        EventBuilder &push(_Args &&... args) noexcept;
    };

    /*!
     * @name Public Accessors
     * @{
     */

    /*!
     * Get ActorId
     * @return ActorId
     */
    ActorId
    id() const noexcept {
        return _id;
    }

    /*!
     * Get core index
     * @return core index
     */
    CoreId getIndex() const noexcept;

    /*!
     * Get derived class name
     * @return name as string_view
     */
    std::string_view getName() const noexcept;

    /*!
     * Get core set
     * @return Coreset of current engine
     */
    const qb::unordered_set<CoreId> &getCoreSet() const noexcept;

    /*!
     * @brief Get current time
     * @return nano timestamp since epoch
     * @details
     * @note
     * This value is optimized and updated each VirtualCore loop.
     * @code
     * // ...
     * auto t1 = time();
     * // ... some heavy calculation
     * assert(t1 == time()); // true - will not assert
     * @endcode
     * To get precise time use NanoTimestamp.
     */
    uint64_t time() const noexcept;

    /*!
     * @private
     */
    template <typename T>
    static ActorId getServiceId(CoreId index) noexcept;

    /*!
     * @brief Get direct access to ServiceActor* in same core
     * @return ptr to _ServiceActor else nullptr if not registered in core
     */
    template <typename _ServiceActor>
    _ServiceActor *getService() const noexcept;

    //void setCoreLowLatency(bool state) const noexcept;

    /*!
     * @brief Check if Actor is alive
     * @return true if Actor is alive else false
     */
    bool is_alive() const noexcept;

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
     * The registered callback will be called each VirtualCore loop.\n
     * _Actor must inherit and implement ICallback interface.\n
     * example:
     * @code
     * class MyActor
     * : public qb::Actor
     * , public qb::ICallback
     * {
     *   virtual bool onInit() {
     *     registerCallback(*this);
     *   }
     * // ...
     *   virtual void onCallback() final {
     *     // do something
     *   }
     * }
     * // ...
     * }
     * @endcode
     */
    template <typename _Actor>
    void registerCallback(_Actor &actor) const noexcept;

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
    void unregisterCallback(_Actor &actor) const noexcept;

    /*!
     * @private
     */
    void unregisterCallback() const noexcept;

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
    template <typename _Event, typename _Actor>
    void registerEvent(_Actor &actor) const noexcept;

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
    template <typename _Event, typename _Actor>
    void unregisterEvent(_Actor &actor) const noexcept;

    /*!
     * @private
     * @tparam _Event
     */
    template <typename _Event>
    void unregisterEvent() const noexcept;

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
    EventBuilder to(ActorId dest) const noexcept;

    /*!
     * @brief Send a new ordered event
     * @tparam _Event DerivedEvent type
     * @param dest destination ActorId
     * @param args arguments to forward to the constructor of the _Event
     * @return a reference to the constructed _Event to send
     * @details
     * All events pushed to same actors id in context will be received ordered by push
     * order.\n example:
     * @code
     * // ...
     * auto &e = push<MyEvent>(id_1); // (1) first push
     * e.some_data = 1337; // set my event data without using constructor
     * push<MyEvent>(id_2, param2); // (2) id_2 != id_1 /!\ possible to be received
     * before (1) push<MyEvent>(id_1, param3); // (3) Guaranteed to be received after (1)
     * // ...
     * @endcode
     * @note
     * Pushed events are sent at the end of the core loop.
     * @attention
     * /!\ We recommend to non advanced users to use only this function to send events.
     */
    template <typename _Event, typename... _Args>
    _Event &push(ActorId const &dest, _Args &&... args) const noexcept;

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
    template <typename _Event, typename... _Args>
    void send(ActorId const &dest, _Args &&... args) const noexcept;

    /*!
    * @brief build local event
    * @tparam _Event Event type
    * @param source ActorId source
    * @param args arguments to forward to the constructor of the _Event
    * @return a local constructed _Event
    * @details
    * Local events are used to make a direct call of on callbacks.\n example:
    * @code
    * // ...
    * auto e = local<MyEvent>(id()); // (1) build local event
    * e.some_data = 1337; // (2) set my event data without using constructor
    * on(e); // (3) call directly
     * // ...
    * @endcode
    * @note
    * event dest_id will be set to id of actor builder
    * @attention
    * /!\ We recommend to non advanced users to use only this function to send events.
    */
    template <typename _Event, typename... _Args>
    _Event build_event(qb::ActorId const source, _Args &&... args) const noexcept;

    template <typename _Type>
    inline bool
    is(uint32_t const id) const noexcept {
        return id == type_id<_Type>();
    }

    template <typename _Type>
    inline bool
    is(RequireEvent const &event) const noexcept {
        return event.type == type_id<_Type>();
    }

    template <typename... _Actors>
    bool require() const noexcept;

    template <typename _Event, typename... _Args>
    void broadcast(_Args &&... args) const noexcept;

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
    void reply(Event &event) const noexcept;

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
    void forward(ActorId dest, Event &event) const noexcept;

    // OpenApi : used for module
    void send(Event const &event) const noexcept;
    void push(Event const &event) const noexcept;
    bool try_send(Event const &event) const noexcept;

    /*!
     * @brief Get access to unidirectional out events pipe
     * @param dest destination ActorId
     * @return destination Pipe
     * @details
     * If you want to send several events to same Actor or push dynamic sized events,\n
     * Actor API allows to retrieve a Pipe to a desired Actor.\n
     * more details on Pipe section
     */
    Pipe getPipe(ActorId dest) const noexcept;

    /*!
     * @brief Create new referenced _Actor
     * @tparam _Actor DerivedActor type
     * @param args arguments to forward to the constructor of the _Actor
     * @return _Actor * on success or nullptr on failure
     * @details
     * create and initialize new _Actor on same VirtualCore as the callee Actor.\n
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
    template <typename _Actor, typename... _Args>
    _Actor *addRefActor(_Args &&... args) const;

    /*!
     * @}
     */
};

/*!
 * @class Service
 * @brief internal
 */
class Service : public Actor {
public:
    explicit Service(ServiceId sid);
};

/*!
 * @class ServiceActor actor.h qb/actor.h
 * @ingroup Core
 * @brief SingletonActor base class
 * @tparam Tag is a uniq struct Tag
 * @details
 * ServiceActor is a special actor where DerivedActor
 * must define a unique service index by Tag.\n
 * Inherited Service Actors are unique per VirtualCore.
 */
template <typename Tag>
class ServiceActor : public Service {
    friend class Main;
    friend class CoreInitializer;
    friend class VirtualCore;
    static const ServiceId ServiceIndex;

public:
    ServiceActor()
        : Service(ServiceIndex) {}
};

class IActorFactory {
public:
    virtual ~IActorFactory() = default;
    virtual Actor *create() = 0;
    [[nodiscard]] virtual bool isService() const = 0;
};

class ActorProxy {
protected:
    ActorProxy() = default;
    template <typename _Type>
    static void
    setType(Actor &actor) {
        actor.id_type = ActorProxy::getType<_Type>();
    }
    template <typename _Type>
    static void
    setName(Actor &actor) {
        actor.name = ActorProxy::getName<_Type>();
    }
public:
    template <typename _Type>
    static auto
    getType() {
        return type_id<_Type>();
    }
    template <typename _Type>
    static const char *
    getName() {
#ifdef __GNUC__
        static std::unique_ptr<char, void(*)(void*)> res {
                abi::__cxa_demangle(typeid(_Type).name(), nullptr, nullptr, nullptr),
                std::free
        };

        return res.get();
#else
        return typeid(_Type).name();
#endif
    }
};

template <typename _Actor, typename... _Args>
class TActorFactory
    : public IActorFactory
    , public ActorProxy {
    ActorId _id;
    std::tuple<typename remove_reference_if<
        _Args, std::is_trivially_copyable<std::remove_reference_t<
                   std::remove_all_extents_t<_Args>>>::value>::type...>
        _parameters;

public:
    explicit TActorFactory(ActorId const id, _Args &&... args)
        : _id(id)
        , _parameters(std::forward<_Args>(args)...) {}

    template <std::size_t... Is>
    Actor *
    create_impl(std::index_sequence<Is...>) {
        auto actor = new _Actor(std::get<Is>(_parameters)...);
        ActorProxy::setType<_Actor>(*actor);
        ActorProxy::setName<_Actor>(*actor);
        return actor;
    }

    Actor *
    create() final {
        return create_impl(std::index_sequence_for<_Args...>{});
    }

    [[nodiscard]] bool
    isService() const final {
        return std::is_base_of<Service, _Actor>::value;
    }
};

using actor = Actor;
template <typename Tag>
using service_actor = ServiceActor<Tag>;

qb::io::log::stream &operator<<(qb::io::log::stream &os, qb::Actor const &actor);
std::ostream &operator<<(std::ostream &os, qb::Actor const &actor);

} // namespace qb

#endif // QB_ACTOR_H
