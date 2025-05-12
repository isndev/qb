/**
 * @file qb/core/Main.h
 * @brief Main control for the QB Actor Framework
 *
 * This file defines the Main class which serves as the primary entry point and control
 * mechanism for the QB Actor Framework. It provides functionality for initializing,
 * configuring, and running the actor system, including management of virtual cores,
 * actor creation, and system-wide signal handling.
 *
 * The file also defines supporting classes such as CoreInitializer which handles
 * per-core configuration, and SharedCoreCommunication which manages inter-core
 * message passing.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2025 qb - isndev (cpp.actor)
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
 * limitations under the License.
 * @ingroup Core
 */

#ifndef QB_MAIN_H
#define QB_MAIN_H
#include <condition_variable>
#include <qb/system/container/unordered_map.h>
#include <thread>
#include <vector>
// include from qb
#include <qb/system/lockfree/mpsc.h>
#include "CoreSet.h"
#include "Event.h"

namespace qb {

class Main;
class VirtualCore;
class IActorFactory;

/**
 * @var NoAffinity
 * @brief Special constant indicating that no CPU affinity is desired
 * @details
 * Used with setAffinity functions to indicate that a VirtualCore should
 * not be restricted to specific CPU cores, letting the operating system
 * handle thread scheduling.
 * 
 * @ingroup Engine
 */
constexpr const CoreId NoAffinity = std::numeric_limits<CoreId>::max();

/**
 * @class CoreInitializer
 * @brief Handles pre-start configuration for a single VirtualCore.
 * @ingroup Engine
 * @details
 * This class allows setting up properties like core affinity, event loop latency,
 * and adding initial actors to a VirtualCore before the main engine starts.
 * Instances are typically obtained via `Main::core(core_id)`.
 */
class CoreInitializer : nocopy {
    friend class Main;
    friend class VirtualCore;

public:
    /*!
     * @class ActorBuilder
     * @brief Helper to fluently build multiple Actors for a CoreInitializer.
     * @ingroup Engine
     * @details
     * Provides a chained interface to add multiple actors to a specific VirtualCore
     * during the setup phase via its CoreInitializer.
     */
    class ActorBuilder {
    public:
        using ActorIdList = std::vector<ActorId>;

    private:
        friend class CoreInitializer;

        CoreInitializer &_initializer;
        ActorIdList      _ret_ids;
        bool             _valid;

        /**
         * @brief Private constructor for ActorBuilder.
         * @param initializer Reference to the CoreInitializer that owns this builder.
         */
        explicit ActorBuilder(CoreInitializer &initializer) noexcept;

    public:
        ActorBuilder()                        = delete;
        ActorBuilder(ActorBuilder const &rhs) = default;

        /*!
         * @brief Create and add a new _Actor to the VirtualCore associated with this builder.
         * @tparam _Actor DerivedActor type to create.
         * @tparam _Args Arguments to forward to the constructor of the _Actor.
         * @param args Arguments to forward to the _Actor's constructor.
         * @return Reference to this ActorBuilder for method chaining.
         * @details
         * Creates a new _Actor on the attached VirtualCore. This function can be chained
         * to add multiple actors in a single statement.
         * Example:
         * @code
         * // auto builder = main.core(0).builder(); // Get actor builder
         * // builder.addActor<MyActor1>(param1, param2)
         * //        .addActor<MyActor2>(arg_a)
         * //        .addActor<MyServiceActor>();
         * @endcode
         * @attention This function is only available before the engine is running.
         *            If actor creation fails (e.g. duplicate ServiceActor, max actors reached),
         *            the `valid()` state of the builder will become false.
         */
        template <typename _Actor, typename... _Args>
        ActorBuilder &addActor(_Args &&...args) noexcept;

        /**
         * @brief Checks if all actor additions via this builder were successful up to this point.
         * @return `true` if all preceding `addActor()` calls on this builder instance succeeded,
         *         `false` if any actor creation failed (e.g., duplicate ServiceActor, max actors, or internal error).
         * @details If this returns `false`, subsequent calls to `addActor()` on this builder may also effectively fail
         *          or might not add to the `idList()`.
         */
        [[nodiscard]] bool valid() const noexcept;
        /**
         * @brief Explicit boolean conversion, equivalent to calling `valid()`.
         * @return `true` if the builder is in a valid state (all actor additions succeeded so far),
         *         `false` otherwise.
         * @see valid()
         */
        explicit           operator bool() const noexcept;

        /*!
         * @brief Get the list of ActorIds created by this ActorBuilder instance.
         * @return An `ActorIdList` (std::vector<ActorId>) containing the IDs of all actors
         *         successfully created by this builder instance up to the point of calling, in the order of creation.
         *         If `valid()` is false, this list may not contain all attempted actors or may be incomplete.
         */
        [[nodiscard]] ActorIdList idList() const noexcept;
    };

private:
    const CoreId _index;
    ServiceId    _next_id;
    CoreIdSet    _affinity;
    uint64_t     _latency;

    qb::unordered_set<ServiceId> _registered_services;
    std::vector<IActorFactory *> _actor_factories;
    //        CoreSet _restricted_communication; future use

public:
    CoreInitializer() = delete;
    /**
     * @brief Constructor for CoreInitializer.
     * @param index The CoreId this initializer is for.
     */
    explicit CoreInitializer(CoreId index);
    /** @brief Destructor. Cleans up actor factories. */
    ~CoreInitializer() noexcept;

    /** 
     * @brief Clears all registered actor factories for this initializer.
     * @details This removes any pending actor creation tasks that were added via `addActor()` or `builder()`
     *          but before the engine was started. Useful if re-configuration is needed before `Main::start()`.
     */
    void clear() noexcept;

    /*!
     * @brief Create and add a new _Actor to this VirtualCore.
     * @tparam _Actor DerivedActor type to create.
     * @tparam _Args Arguments to forward to the constructor of the _Actor.
     * @param args Arguments to forward to the _Actor's constructor.
     * @return `ActorId` of the created _Actor. Returns `ActorId::NotFound` on failure
     *         (e.g., duplicate ServiceActor, max actors reached).
     * @details
     * Creates a new _Actor instance scheduled to run on the VirtualCore associated
     * with this CoreInitializer.
     * Example:
     * @code
     * // auto id = main.core(0).addActor<MyActor>(param1, param2);
     * // if (id.is_valid()) { ... }
     * @endcode
     * @attention This function is only available before the engine is running.
     */
    template <typename _Actor, typename... _Args>
    ActorId addActor(_Args &&...args) noexcept;

    /*!
     * @brief Get an ActorBuilder for this CoreInitializer.
     * @return An `ActorBuilder` instance for fluently adding multiple actors to this core.
     * @attention
     * Each call to `builder()` returns a new `ActorBuilder` instance.
     * @code
     * // auto builder1 = main.core(0).builder();
     * // auto builder2 = main.core(0).builder();
     * // // Even though both builders configure the same VirtualCore,
     * // // builder1 and builder2 are distinct objects.
     * @endcode
     */
    [[nodiscard]] ActorBuilder builder() noexcept;

    /*!
     * @brief Set the CPU affinity for the VirtualCore associated with this initializer.
     * @param cores A `CoreIdSet` specifying the set of physical CPU cores this VirtualCore thread
     *              should be allowed to run on. An empty set typically means default OS scheduling.
     * @return Reference to this `CoreInitializer` for method chaining.
     * @note By default, affinity is typically set to allow the VirtualCore thread to run on any CPU.
     *       This setting takes effect when the engine starts.
     */
    CoreInitializer &setAffinity(CoreIdSet const &cores = {}) noexcept;

    /*!
     * @brief Set the maximum event loop latency for the VirtualCore.
     * @param latency The maximum time in nanoseconds the VirtualCore's event loop will wait
     *                if it hasn't received any core/IO events. Defaults to 0.
     * @return Reference to this `CoreInitializer` for method chaining.
     * @note
     * - `0` (default): Low latency mode. The VirtualCore spins actively, consuming 100% CPU
     *   on its assigned core, to process events with minimal delay.
     * - `latency > 0`: The VirtualCore may sleep for up to this duration if idle, reducing CPU usage.
     *   This introduces a potential worst-case latency for new event processing.
     * This setting takes effect when the engine starts.
     */
    CoreInitializer &setLatency(uint64_t latency = 0) noexcept;

    /** 
     * @brief Gets the CoreId associated with this initializer.
     * @return The `CoreId` (unsigned short) of the VirtualCore this initializer configures.
     */
    [[nodiscard]] CoreId           getIndex() const noexcept;
    /** 
     * @brief Gets the currently configured CPU affinity set for this core.
     * @return Const reference to a `CoreIdSet` representing the CPU cores this VirtualCore may run on.
     */
    [[nodiscard]] CoreIdSet const &getAffinity() const noexcept;
    /** 
     * @brief Gets the currently configured maximum event loop latency (in ns) for this core.
     * @return `uint64_t` latency value in nanoseconds. See `setLatency()` for interpretation.
     */
    [[nodiscard]] uint64_t         getLatency() const noexcept;
};

/**
 * @typedef CoreInitializerMap
 * @brief Map of CoreId to CoreInitializer objects
 * @details
 * This container maps core identifiers to their respective initializer objects,
 * providing a way to store and access configuration for all VirtualCores in the system.
 * 
 * @ingroup Engine
 */
using CoreInitializerMap = qb::unordered_map<CoreId, CoreInitializer>;

/**
 * @class SharedCoreCommunication
 * @brief Manages inter-core communication infrastructure (mailboxes).
 * @ingroup Engine
 * @details
 * This class is an internal component of `qb::Main`. It sets up and owns the
 * MPSC mailboxes used by VirtualCores to send events to each other. It is not
 * typically interacted with directly by application code.
 */
class SharedCoreCommunication : nocopy {
    friend class VirtualCore;
    friend class Main;
    constexpr static const uint64_t MaxRingEvents =
        (((std::numeric_limits<uint16_t>::max)()) / QB_LOCKFREE_EVENT_BUCKET_BYTES);
    //////// Types
    class Mailbox : public lockfree::mpsc::ringbuffer<EventBucket, MaxRingEvents, 0> {
        const uint64_t          _latency;
        std::mutex              _mtx;
        std::condition_variable _cv;

    public:
        explicit Mailbox(std::size_t const nb_producer, uint64_t const latency)
            : lockfree::mpsc::ringbuffer<EventBucket, MaxRingEvents, 0>(nb_producer)
            , _latency(latency) {}

        /**
         * @brief Waits for a notification on this mailbox, up to its configured latency.
         * @ingroup Engine
         * @details If the mailbox is configured with a non-zero latency (`_latency > 0`),
         *          this method blocks the calling thread (typically a VirtualCore's event loop)
         *          using a `std::condition_variable` for a duration up to `_latency` nanoseconds,
         *          or until `notify()` is called.
         *          If `_latency` is 0, this method returns immediately (effectively a no-op for waiting).
         *          This is used by VirtualCores to sleep when idle, reducing CPU usage.
         */
        void
        wait() noexcept {
            if (_latency) {
                std::unique_lock lk(_mtx);
                _cv.wait_for(lk, std::chrono::nanoseconds(_latency));
            }
        }

        /**
         * @brief Notifies a waiting thread (VirtualCore) that an event might be available in this mailbox.
         * @ingroup Engine
         * @details If the mailbox is configured with a non-zero latency (`_latency > 0`),
         *          this method signals the `std::condition_variable` associated with this mailbox.
         *          This wakes up a VirtualCore thread that might be sleeping in the `wait()` method,
         *          prompting it to check the mailbox for new events.
         *          If `_latency` is 0, this method is a no-op.
         */
        void
        notify() noexcept {
            if (_latency)
                _cv.notify_all();
        }

        /**
         * @brief Get the latency setting for this mailbox.
         * @ingroup Engine
         * @return The configured latency in nanoseconds. If 0, the mailbox operates in a low-latency (busy-spin) mode for its consumer.
         */
        [[nodiscard]] uint64_t
        getLatency() const noexcept {
            return _latency;
        }
    };

    const CoreSet                  _core_set;
    std::vector<std::atomic<bool>> _event_safe_deadlock;
    std::vector<Mailbox *>         _mail_boxes;

public:
    SharedCoreCommunication() = delete;
    explicit SharedCoreCommunication(
        CoreInitializerMap const &core_initializers) noexcept;

    ~SharedCoreCommunication() noexcept;

    /**
     * @brief Send an event to the mailbox of its destination VirtualCore.
     * @ingroup Engine
     * @param event The `qb::Event` to send. The event's `dest_core()` determines the target mailbox.
     * @return `true` if the event was successfully enqueued into the destination core's mailbox.
     *         `false` if the destination core ID is invalid, the mailbox is full (rare),
     *         or another error occurred during enqueueing.
     * @details This method is used internally by actors and the engine to route events between cores.
     *          It relies on the MPSC ringbuffer implementation for the mailboxes.
     */
    [[nodiscard]] bool send(Event const &event) const noexcept;

    /**
     * @brief Get the mailbox for a specific VirtualCore.
     * @ingroup Engine
     * @param id The `CoreId` of the VirtualCore whose mailbox is requested.
     * @return Reference to the `Mailbox` object for the specified core.
     * @note This provides direct access to the inter-core communication channel. Used internally.
     *       Throws `std::out_of_range` if `id` is invalid.
     */
    [[nodiscard]] Mailbox &getMailBox(CoreId id) const noexcept;

    /**
     * @brief Get the number of VirtualCores configured in the system.
     * @ingroup Engine
     * @return The total number of `CoreId`s managed by this `SharedCoreCommunication` instance,
     *         which corresponds to the number of VirtualCores the engine will run.
     */
    [[nodiscard]] CoreId getNbCore() const noexcept;
};

/**
 * @struct CoreSpawnerParameter
 * @brief Internal structure for passing parameters to core spawning functions
 * @details
 * This structure encapsulates the parameters needed when spawning a new VirtualCore.
 * It contains the core's ID, reference to its initializer, the shared communication
 * infrastructure, and synchronization primitives to coordinate startup.
 * 
 * @ingroup Engine
 */
struct CoreSpawnerParameter {
    /** @brief The CoreId of the VirtualCore being spawned */
    const CoreId             id;
    
    /** @brief Reference to the CoreInitializer for this core */
    CoreInitializer         &initializer;
    
    /** @brief Reference to the shared communication infrastructure */
    SharedCoreCommunication &shared_com;
    
    /** @brief Atomic counter for synchronizing core startup */
    std::atomic<uint64_t>   &sync_start;
};

/*!
 * @class Main
 * @ingroup Engine
 * @brief The main controller for the QB Actor Framework engine.
 * @details
 * This class is the primary entry point for initializing, configuring, and running
 * the actor system. It manages the lifecycle of VirtualCores (worker threads),
 * provides an interface for adding actors to these cores, and handles system-wide
 * concerns like signal handling and overall system start/stop.
 */
class Main {
    friend class VirtualCore;
    constexpr static const uint64_t MaxRingEvents =
        (((std::numeric_limits<uint16_t>::max)()) / QB_LOCKFREE_EVENT_BUCKET_BYTES);
    //////// Types
    using Mailbox = lockfree::mpsc::ringbuffer<EventBucket, MaxRingEvents, 0>;

    static std::vector<Main *> _instances;
    static std::mutex          _instances_lock;

    std::atomic<uint64_t> _sync_start;
    static void           onSignal(int signal) noexcept;
    static void           start_thread(CoreSpawnerParameter const &params) noexcept;
    static bool           __wait__all__cores__ready(std::size_t            nb_core,
                                                    std::atomic<uint64_t> &sync_start) noexcept;

private:
    std::vector<std::thread> _cores;
    // Core Factory
    CoreInitializerMap                       _core_initializers;
    std::unique_ptr<SharedCoreCommunication> _shared_com;
    bool                                     _is_running;

public:
    using ActorIdList = CoreInitializer::ActorBuilder::ActorIdList;

    /** @brief Default constructor. Initializes the main engine structure. */
    Main() noexcept;
    /** @brief Destructor. Ensures graceful shutdown of the engine if running. */
    ~Main() noexcept;

    /*!
     * @brief Start the engine and its VirtualCore worker threads.
     * @ingroup Engine
     * @param async If `true` (default), the engine starts asynchronously, and this call returns immediately.
     *              The main application thread continues execution. `join()` should be called later to wait.
     *              If `false`, the calling thread becomes one of the VirtualCore worker threads (typically core 0).
     *              This call will block until the engine is stopped.
     * @note All actors and core configurations (affinity, latency) must be set up *before* calling `start()`.
     */
    void start(bool async = true) noexcept;

    /*!
     * @brief Check if any VirtualCore encountered an error and terminated prematurely.
     * @ingroup Engine
     * @return `true` if an error occurred in one or more cores, `false` otherwise.
     * @note This should typically be checked after `join()` returns.
     */
    [[nodiscard]] bool hasError() const noexcept;

    /*!
     * @brief Stop the engine and all its VirtualCores gracefully.
     * @ingroup Engine
     * @details This is a static method and can be called from any thread, including signal handlers.
     *          It signals all VirtualCores to shut down. Actors will typically receive a `KillEvent`.
     * @note Same effect as receiving a SIGINT or SIGTERM signal by default.
     */
    static void stop() noexcept;

    /*!
     * @brief Wait for the engine and all its VirtualCore threads to terminate.
     * @ingroup Engine
     * @details This function blocks until the engine has fully shut down.
     *          It should be called if `start(true)` (asynchronous start) was used.
     */
    void join();

public:
    /*!
     * @brief Add a new actor to a specified VirtualCore before the engine starts.
     * @ingroup Engine
     * @tparam _Actor DerivedActor type to create.
     * @tparam _Args Arguments to forward to the constructor of the _Actor.
     * @param index The `CoreId` of the VirtualCore to add this actor to.
     * @param args Arguments to forward to the _Actor\'s constructor.
     * @return `ActorId` of the created _Actor. Returns `ActorId::NotFound` on failure.
     * @details
     * A convenience method that is equivalent to `core(index).addActor<_Actor>(args...)`.
     * Example:
     * @code
     * // qb::Main engine;
     * // auto id = engine.addActor<MyActor>(0, param1, param2);
     * @endcode
     * @attention This function is only available before the engine is running.
     */
    template <typename _Actor, typename... _Args>
    ActorId addActor(CoreId index, _Args &&...args);

    /*!
     * @brief Get the CoreInitializer for a specific VirtualCore index.
     * @ingroup Engine
     * @param index The `CoreId` of the VirtualCore to configure.
     * @return Reference to the `CoreInitializer` for the specified core.
     * @details
     * Allows setting core-specific properties like affinity and latency before starting the engine.
     * @attention
     * This function is only available before the engine is running.
     * The returned reference is to an object managed by `qb::Main`.
     * @code
     * // qb::Main engine;
     * // qb::CoreInitializer& core_config = engine.core(0);
     * // core_config.setLatency(100000); // 100us
     * @endcode
     */
    [[nodiscard]] CoreInitializer &core(CoreId index);

    /*!
     * @brief Set the default event loop latency for all VirtualCores.
     * @ingroup Engine
     * @param latency The maximum time in nanoseconds for cores to wait when idle. `0` means no wait (low latency mode).
     * @details This sets the latency for all cores that haven\'t had a specific latency set via `CoreInitializer::setLatency()`.\n
     *          See `CoreInitializer::setLatency()` for more details on latency values.
     * @attention This function is only available before the engine is running.
     */
    void setLatency(uint64_t latency = 0);

    /*!
     * @brief Get the set of `CoreId`s that are currently configured to be used by the engine.
     * @ingroup Engine
     * @return A `qb::CoreIdSet` containing the IDs of all cores that will be launched.
     * @details This reflects the cores for which `CoreInitializer` objects exist, typically based
     *          on the arguments passed to the `Main` constructor or default hardware concurrency.
     */
    [[nodiscard]] qb::CoreIdSet usedCoreSet() const;

    /*!
     * @brief Register a system signal to be handled by the engine (results in graceful shutdown).
     * @ingroup Engine
     * @param signum The signal number (e.g., `SIGUSR1`, `SIGHUP`).
     * @note
     * By default, `SIGINT` and `SIGTERM` (on non-Windows platforms) are registered to call `Main::stop()`.\n
     * Registered signals will trigger a graceful shutdown of all actors.\n
     * This is a static method and affects all `Main` instances if multiple were to exist (though typically only one exists).
     */
    static void registerSignal(int signum) noexcept;
    /*!
     * @brief Unregister a previously registered system signal from engine handling.
     * @ingroup Engine
     * @param signum The signal number to unregister.
     * @details After unregistering, the default OS behavior for that signal will apply if it occurs.\n
     * This is a static method.
     */
    static void unregisterSignal(int signum) noexcept;
    /*!
     * @brief Ignore a system signal, preventing the engine or default OS handler from processing it.
     * @ingroup Engine
     * @param signum The signal number to ignore (e.g., `SIGPIPE`).
     * @details This is a static method.
     */
    static void ignoreSignal(int signum) noexcept;
};

/**
 * @typedef engine
 * @brief Alias for the Main class
 * @details
 * Provides a concise alternative name for the main engine class.
 * This is provided for naming consistency with other lowercase aliases
 * in the framework.
 * 
 * @ingroup Engine
 */
using engine = Main;

} // namespace qb
#endif // QB_MAIN_H
