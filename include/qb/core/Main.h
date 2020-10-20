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

#ifndef QB_MAIN_H
#define QB_MAIN_H
#include <iostream>
#include <qb/system/container/unordered_map.h>
#include <thread>
#include <condition_variable>
#include <vector>
// include from qb
#include "CoreSet.h"
#include "Event.h"
#include <qb/system/lockfree/mpsc.h>

namespace qb {

class Main;
class VirtualCore;
class IActorFactory;

constexpr const CoreId NoAffinity = std::numeric_limits<CoreId>::max();

class CoreInitializer : nocopy {
    friend class Main;
    friend class VirtualCore;

public:
    /*!
     * @class ActorBuilder core/Main.h qb/main.h
     * @brief Helper to build Actors in VirtualCore
     */
    class ActorBuilder {
    public:
        using ActorIdList = std::vector<ActorId>;

    private:
        friend class CoreInitializer;

        CoreInitializer &_initializer;
        ActorIdList _ret_ids;
        bool _valid;

        explicit ActorBuilder(CoreInitializer &initializer) noexcept;

    public:
        ActorBuilder() = delete;
        ActorBuilder(ActorBuilder const &rhs) = default;

        /*!
         * @brief Create new _Actor
         * @tparam _Actor DerivedActor type
         * @param args arguments to forward to the constructor of the _Actor
         * @return itself
         * @details
         * create new _Actor on attached VirtualCore, function can be chained.\n
         * example:
         * @code
         * auto builder = main.core(0).builder(); // get actor builder of CoreInitializer
         * 0 builder.addActor<MyActor>(param1, param2) .addActor<MyActor>(param1, param2)
         *        // ...
         *        ;
         * @endcode
         * @attention
         * This function is not available while engine is running.
         */
        template <typename _Actor, typename... _Args>
        ActorBuilder &addActor(_Args &&... args) noexcept;

        [[nodiscard]] bool valid() const noexcept;
        explicit operator bool() const noexcept;

        /*!
         * @brief Get list of created ActorId by the ActorBuilder
         * @return Created ActorId list
         */
        [[nodiscard]] ActorIdList idList() const noexcept;
    };

private:
    const CoreId _index;
    ServiceId _next_id;
    qb::unordered_set<CoreId> _affinity;
    uint64_t _latency;

    qb::unordered_set<ServiceId> _registered_services;
    std::vector<IActorFactory *> _actor_factories;
    //        CoreSet _restricted_communication; future use

public:
    CoreInitializer() = delete;
    explicit CoreInitializer(CoreId index);
    ~CoreInitializer() noexcept;

    void clear() noexcept;

    /*!
     * @brief Create new _Actor
     * @tparam _Actor DerivedActor type
     * @param args arguments to forward to the constructor of the _Actor
     * @return ActorId of the created _Actor
     * @details
     * create new _Actor on VirtualCore index.\n
     * example:
     * @code
     * auto id = main.core(0).addActor<MyActor>(param1, param2);
     * @endcode
     * @attention
     * This function is not available while engine is running.
     */
    template <typename _Actor, typename... _Args>
    ActorId addActor(_Args &&... args) noexcept;

    /*!
     * @brief Get ActorBuilder from CoreIntializer
     * @return ActorBuilder
     * @attention
     * @code
     * auto builder1 = main.core(0).builder();
     * auto builder2 = main.core(0).builder();
     * // even both elements build the same VirtualCore
     * // builder1 != builder2
     * @endcode
     */
    ActorBuilder builder() noexcept;

    /*!
     * @brief Set VirtualCore affinity
     * @param cores list of physical cores to be used
     * @note
     * by default affinity is on VirtualCore index
     */
    CoreInitializer &setAffinity(CoreIdSet const &cores = {}) noexcept;

    /*!
     * @brief Set VirtualCore max wait latency in nanosecond if it hasn't received any core/io events
     * @param latency in nanoseconds (default 0)
     * @note
     * 0 is low latency (Core is used at 100%)
     * latency > 0 Core max wating time before handling events and callbacks
     */
    CoreInitializer &setLatency(uint64_t latency = 0) noexcept;

    [[nodiscard]] CoreId getIndex() const noexcept;
    [[nodiscard]] CoreIdSet const &getAffinity() const noexcept;
    [[nodiscard]] uint64_t getLatency() const noexcept;
};

using CoreInitializerMap = qb::unordered_map<CoreId, CoreInitializer>;

class SharedCoreCommunication : nocopy {
    friend class VirtualCore;
    friend class Main;
    constexpr static const uint64_t MaxRingEvents =
        (((std::numeric_limits<uint16_t>::max)()) / QB_LOCKFREE_EVENT_BUCKET_BYTES);
    //////// Types
    class Mailbox
        : public lockfree::mpsc::ringbuffer<EventBucket, MaxRingEvents, 0> {
        const uint64_t _latency;
        std::mutex _mtx;
        std::condition_variable _cv;
    public:

        explicit Mailbox(std::size_t const nb_producer, uint64_t const latency)
            : lockfree::mpsc::ringbuffer<EventBucket, MaxRingEvents, 0>(nb_producer)
            , _latency(latency) {}

        void
        wait() noexcept {
            if (_latency) {
                std::unique_lock lk(_mtx);
                _cv.wait_for(lk, std::chrono::nanoseconds(_latency));
            }
        }

        void
        notify() noexcept {
            if (_latency)
                _cv.notify_all();
        }

        [[nodiscard]] uint64_t
        getLatency() const noexcept {
            return _latency;
        }

    };

    const CoreSet _core_set;
    std::vector<std::atomic<bool>> _event_safe_deadlock;
    std::vector<Mailbox *> _mail_boxes;

public:
    SharedCoreCommunication() = delete;
    explicit SharedCoreCommunication(CoreInitializerMap const &core_initializers) noexcept;

    ~SharedCoreCommunication() noexcept;

    [[nodiscard]] bool send(Event const &event) const noexcept;
    [[nodiscard]] Mailbox &getMailBox(CoreId id) const noexcept;
    [[nodiscard]] CoreId getNbCore() const noexcept;
};

struct CoreSpawnerParameter {
    const CoreId id;
    CoreInitializer &initializer;
    SharedCoreCommunication &shared_com;
    std::atomic<uint64_t> &sync_start;
};

/*!
 * @class Main core/Main.h qb/main.h
 * @ingroup Core
 * @brief Core main class
 * @details
 * This is the Main engine class, initialized with desired CoreSet.
 */
class Main {
    friend class VirtualCore;
    constexpr static const uint64_t MaxRingEvents =
        (((std::numeric_limits<uint16_t>::max)()) / QB_LOCKFREE_EVENT_BUCKET_BYTES);
    //////// Types
    using Mailbox = lockfree::mpsc::ringbuffer<EventBucket, MaxRingEvents, 0>;

    static std::vector<Main *> _instances;
    static std::mutex _instances_lock;

    std::atomic<uint64_t> _sync_start;
    static void onSignal(int signal) noexcept;
    static void start_thread(CoreSpawnerParameter const &params) noexcept;
    static bool __wait__all__cores__ready(std::size_t nb_core,
                                          std::atomic<uint64_t> &sync_start) noexcept;

private:
    std::vector<std::thread> _cores;
    // Core Factory
    CoreInitializerMap _core_initializers;
    SharedCoreCommunication *_shared_com;
    bool _is_running;

public:
    using ActorIdList = CoreInitializer::ActorBuilder::ActorIdList;

    Main() noexcept;
    ~Main() noexcept;

    /*!
     * @brief Start the engine
     * @param async has blocking execution
     * @note
     * If async = false the main thread will by used by a VirtualCore engine.
     */
    void start(bool async = true) noexcept;

    bool hasError() const noexcept;

    /*!
     * @brief Stop the engine
     * @note
     * Same effect as receiving SIGINT Signal.
     */
    static void stop() noexcept;

    /*!
     * @brief Wait until engine terminates
     */
    void join();

public:
    /*!
     * @brief Create new _Actor
     * @tparam _Actor DerivedActor type
     * @param index VirtualCore index
     * @param args arguments to forward to the constructor of the _Actor
     * @return ActorId of the created _Actor
     * @details
     * create new _Actor on VirtualCore index.\n
     * example:
     * @code
     * auto id = addActor<MyActor>(0, param1, param2);
     * @endcode
     * @attention
     * This function is not available while engine is running.
     */
    template <typename _Actor, typename... _Args>
    ActorId addActor(CoreId index, _Args &&... args);

    /*!
     * @brief Get CoreIntializer from index
     * @param index VirtualCore index
     * @return CoreInitializer &
     * @attention
     * @code
     * auto &initializer1 = main.core(0);
     * auto &initializer2 = main.core(0);
     * // initializer1 == initializer2
     * // Core initializers are not copyable
     * @endcode
     * @attention
     * This function is not available while engine is running.
     */
    CoreInitializer &core(CoreId index);

    qb::CoreIdSet usedCoreSet() const {
        qb::CoreIdSet ret;
        for (const auto &it : _core_initializers)
            ret.emplace(it.first);
        return ret;
    }


    /*!
     * @brief Register signal for all engines
     * @param signum Signal number
     * @note
     * Signal SIGINT is registered by default
     * This signal is used to kill all Actors
     * @code
     */
    static void registerSignal(int signum) noexcept;
    /*!
     * @brief Unregister signal for all engines
     * @param signum Signal number
     */
    static void unregisterSignal(int signum) noexcept;
    /*!
     * @brief Ignore process signal
     * @param signum Signal number
     */
    static void ignoreSignal(int signum) noexcept;
};

using engine = Main;

} // namespace qb

#endif // QB_MAIN_H
