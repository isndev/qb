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
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
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
#include <atomic>
#include <condition_variable>
#include <csignal>
#include <qb/system/container/unordered_map.h>
#include <thread>
#include <vector>
// include from qb
#include <qb/system/lockfree/mpsc.h>
#include <qb/system/time.h>
#include <qb/utility/compat.h>
#include "CoreSet.h"
#include "Event.h"

namespace qb {

class Main;
class VirtualCore;
class IActorFactory;

/**
 * @var NoAffinity
 * @brief Public sentinel `CoreId` value meaning "no CPU affinity requested".
 * @details
 * Named, expressive opt-out for CPU pinning. Use it inside a `CoreIdSet`
 * passed to `CoreInitializer::setAffinity` when you want the OS scheduler
 * to remain in charge of thread placement while still making intent clear:
 *
 * @code
 * // Explicit "no pinning, OS decides" — preserves intent in source.
 * engine.core(0).setAffinity(qb::CoreIdSet{qb::NoAffinity});
 *
 * // Mixed: let the sentinel be harmless filler in a set of real ids.
 * engine.core(1).setAffinity(qb::CoreIdSet{2, qb::NoAffinity});
 * @endcode
 *
 * Runtime behaviour: `VirtualCore::__init__` filters out any `CoreId >=
 * qb::MaxCores` (which includes `NoAffinity`). If the resulting set contains
 * zero real core ids, no `pthread_setaffinity_np` /
 * `SetThreadAffinityMask` call is issued — semantics identical to passing an
 * empty `CoreIdSet`. This makes `NoAffinity` a safe, well-defined sentinel.
 *
 * @warning **Opting out is what a Mac gives you anyway.** The paragraph above
 * describes the sentinel; it does not promise that omitting the sentinel pins
 * anything. Whether a *real* `CoreId` produces an OS-level pin is a platform
 * property, and on macOS the answer is "no" or "not the way it reads":
 * - macOS has no `pthread_setaffinity_np`, so qb supplies one that calls
 *   `thread_policy_set(THREAD_AFFINITY_POLICY)`. On **Apple Silicon** that
 *   flavor is not implemented: every call answers `KERN_NOT_SUPPORTED`, and
 *   the shim deliberately reports *success* for that code (otherwise every
 *   core of every run would warn). Nothing is pinned, silently.
 * - Where macOS *does* implement it (Intel), `<mach/thread_policy.h>` calls it
 *   experimental and a scheduler **hint**: threads sharing an affinity *tag*
 *   are placed so as to share an L2 cache. qb passes the `CoreId` as that tag,
 *   so `setAffinity(3)` requests "group me with other tag-3 threads", not "pin
 *   me to CPU 3". That is a different guarantee from the Linux/Windows one, on
 *   every Mac.
 * - The shim also honours only the **first** real id in the set, so a
 *   multi-core `CoreIdSet` narrows to its lowest member there.
 *
 * Do not infer placement from a successful `setAffinity()`; ask
 * `qb::CPU::ThreadPinningSupported()` (`qb/system/cpu.h`), which reports
 * whether this host implements per-thread pinning at all — a runtime probe, so
 * it is also right for an x86_64 binary running under Rosetta 2. Linux and
 * other POSIX use the real `pthread_setaffinity_np`; MSVC Windows uses
 * `SetThreadAffinityMask`; a Windows GNU build applies no affinity at all.
 *
 * Implementation detail: value is `std::numeric_limits<CoreId>::max()`,
 * deliberately strictly greater than `qb::MaxCores`, so it can never be
 * mistaken for a legitimate hardware core id.
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
        explicit operator bool() const noexcept;

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
    qb::duration _latency;
    qb::duration _idle_spin;

    qb::unordered_set<ServiceId>                _registered_services;
    std::vector<std::unique_ptr<IActorFactory>> _actor_factories;
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
     * @warning This is a **request**, and on macOS it does not pin: the call succeeds while
     *          nothing is placed (Apple Silicon) or groups threads by cache tag rather than
     *          binding them to a CPU (Intel). See `qb::NoAffinity` above for the full account,
     *          and branch on `qb::CPU::ThreadPinningSupported()` rather than on this returning.
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
     * @see setIdleSpin() for how long an idle core keeps polling before it takes that sleep.
     */
    CoreInitializer &setLatency(qb::duration latency = qb::duration::zero()) noexcept;

    /*!
     * @brief Set how long an idle VirtualCore keeps polling before it parks.
     * @param idle_spin Time since the core's last activity after which it may block on its
     *                  mailbox. Defaults to `kDefaultIdleSpin` (50 µs).
     * @return Reference to this `CoreInitializer` for method chaining.
     * @note
     * Only meaningful with `latency > 0`; a `latency == 0` core never parks. The floor is what
     * keeps a request/response exchange on the lock-free path: a core that parked the moment
     * it found nothing would pay an OS park + wake on every hop of a ping-pong (~2–13 µs,
     * against ~300 ns polling), because a reply always arrives a few hundred nanoseconds after
     * the request left. `qb::duration::zero()` parks on the first idle pass (the 3.0 behaviour
     * for a one-event-per-pass workload); a larger value trades idle CPU for a longer window in
     * which a late reply is still picked up at polling latency.
     * This setting takes effect when the engine starts.
     */
    CoreInitializer &setIdleSpin(qb::duration idle_spin = kDefaultIdleSpin) noexcept;

    /**
     * @brief Gets the CoreId associated with this initializer.
     * @return The `CoreId` (unsigned short) of the VirtualCore this initializer configures.
     */
    [[nodiscard]] CoreId getIndex() const noexcept;
    /**
     * @brief Gets the currently configured CPU affinity set for this core.
     * @return Const reference to a `CoreIdSet` representing the CPU cores this VirtualCore may run on.
     */
    [[nodiscard]] CoreIdSet const &getAffinity() const noexcept;
    /**
     * @brief Gets the currently configured maximum event loop latency (in ns) for this core.
     * @return `qb::duration` latency value. See `setLatency()` for interpretation.
     */
    [[nodiscard]] qb::duration getLatency() const noexcept;
    /**
     * @brief Gets the configured idle-spin floor for this core.
     * @return `qb::duration` value. See `setIdleSpin()` for interpretation.
     */
    [[nodiscard]] qb::duration getIdleSpin() const noexcept;

    /// Default `setIdleSpin()`: measured to hold a two-core ping-pong on the polling path on
    /// Windows, WSL2 and Linux alike, while an idle core still parks within ~50 µs of its last event.
    static constexpr qb::duration kDefaultIdleSpin = std::chrono::microseconds{50};
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
    constexpr static const uint64_t MaxRingEvents = (((std::numeric_limits<uint16_t>::max)()) / QB_LOCKFREE_EVENT_BUCKET_BYTES);
    //////// Types

public:
    /**
     * @brief One core's inbound ring set plus its park/unpark handshake.
     * @details The consumer (the owning `VirtualCore`) parks in `wait()` once it has been idle
     *          for `_idle_spin`; a producer (any core's `send()`, or a `__flush_all__` that gave
     *          up its retry budget) calls `notify()` after its enqueue. The handshake is a
     *          Dekker pair over `_parked`:
     *
     *            consumer: `_parked = true; fence(seq_cst); if (has_data()) return; lock; wait`
     *            producer: `enqueue;       fence(seq_cst); if (!_parked)  return; lock; notify`
     *
     *          Either the producer's enqueue is visible to the consumer's `has_data()` or the
     *          consumer's `_parked` is visible to the producer's load — the two fences forbid the
     *          case where both miss. The producer that does see `_parked` takes the mutex before
     *          notifying, which orders it against the consumer's lock-then-wait: if the producer
     *          locks first, the consumer's predicate finds the data before it ever waits; if the
     *          consumer locks first, it is already waiting when the notify lands. Both are
     *          load-bearing — the shipped 3.0 form (`wait_for` with no predicate, `notify_all()`
     *          without the mutex) lost the wakeup for every enqueue landing between the empty
     *          `consume_all` and the wait, and the core then slept the whole `_latency` (on MSVC,
     *          a whole scheduler tick: measured ~13 ms per loss, 50–95 % of a 2-core ping-pong's
     *          wait time). When nobody is parked, `notify()` costs one fence and one load.
     */
    class Mailbox : public lockfree::mpsc::ringbuffer<EventBucket, MaxRingEvents, 0> {
        const qb::duration _latency;   ///< Longest single park; 0 = the consumer never parks.
        const qb::duration _idle_spin; ///< Idle time the consumer spins through before it parks.
        // The consumer's "I am about to block" flag. Its own line: producers read it on every
        // enqueue, the consumer writes it only around a park, and nothing else may share it.
        alignas(QB_LOCKFREE_CACHELINE_BYTES) std::atomic<bool> _parked{false};
        std::mutex              _mtx;
        std::condition_variable _cv;

        // The Dekker fence, once for both halves. gcc's -fsanitize=thread does not MODEL a
        // fence (it still emits it) and says so with -Wtsan at every use. That blindness costs
        // nothing here: TSan reports data races on non-atomic memory through happens-before,
        // and every object the handshake touches — `_parked`, the ring indices, the slots they
        // publish with release/acquire — is either atomic or ordered by one, so a fence TSan
        // cannot see produces neither a false report nor a lost one. A store-load reordering,
        // the bug class the fence exists for, is outside what TSan can observe with or without
        // it. clang's TSan models seq_cst fences and warns about nothing.
        static void
        seq_cst_fence() noexcept {
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wtsan"
#endif
            std::atomic_thread_fence(std::memory_order_seq_cst);
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
        }

    public:
        explicit Mailbox(std::size_t const nb_producer, qb::duration const latency, qb::duration const idle_spin)
            : lockfree::mpsc::ringbuffer<EventBucket, MaxRingEvents, 0>(nb_producer)
            , _latency(latency)
            , _idle_spin(idle_spin) {}

        /**
         * @brief Park the consumer until data is enqueued, `notify()` lands, or `_latency` elapses.
         * @ingroup Engine
         * @details Consumer side of the handshake described on the class. Returns at once — without
         *          touching the mutex — if a producer has already published something; otherwise
         *          blocks for at most `_latency`, re-checking `has_data()` on every wake so a spurious
         *          one cannot return early with an empty ring set. A no-op when `_latency` is 0.
         * @note Only the owning core's thread may call this.
         */
        void
        wait() noexcept {
            if (_latency <= qb::duration::zero())
                return;
            _parked.store(true, std::memory_order_relaxed);
            seq_cst_fence();
            if (!has_data()) {
                std::unique_lock lk(_mtx);
                _cv.wait_for(lk, _latency, [this] { return has_data(); });
            }
            _parked.store(false, std::memory_order_relaxed);
        }

        /**
         * @brief Wake the consumer if it is parked; producer side of the handshake.
         * @ingroup Engine
         * @details Call AFTER the enqueue whose data the consumer must see. Costs one fence and one
         *          load on the common (nobody parked) path; takes the mutex only when the consumer
         *          has announced a park, which is what makes the wake-up impossible to lose. At
         *          `_latency` 0 only the fence runs — see the note in the body.
         */
        void
        notify() noexcept {
            // The fence is issued in spin mode too, on purpose. It is the producer's half of the
            // Dekker pair, so a parking mailbox needs it for correctness — but on x86 a seq_cst
            // fence is also an `mfence`, which drains the store buffer, so the event just enqueued
            // becomes visible to the consumer NOW rather than whenever the buffer happens to
            // flush. Measured on a two-core ping-pong (qb-vs-others audit, axis K; interleaved
            // A/B on a quiet host, p50 of 7 runs): Windows/MSVC 296–309 ns per round-trip
            // without it, 259–263 with it — the spinning cell was SLOWER than the parking one
            // (256–272) before; Linux/g++-14 204–219 → 205–208, worst run 295 → 215. It is paid
            // once per cross-core event, after the bucket copy that already dominates: the bulk
            // case (`BM_Multi_Producer_Consumer`, 1M events) is unchanged at 18.6 M msg/s.
            // arm64 (`dmb ish`) is unmeasured; the macOS bench gate is the instrument.
            seq_cst_fence();
            if (_latency <= qb::duration::zero())
                return;
            if (!_parked.load(std::memory_order_relaxed))
                return;
            {
                std::lock_guard lk(_mtx);
            }
            _cv.notify_one();
        }

        /**
         * @brief Get the latency setting for this mailbox.
         * @ingroup Engine
         * @return The configured latency in nanoseconds. If 0, the mailbox operates in a low-latency (busy-spin) mode for its consumer.
         */
        [[nodiscard]] qb::duration
        getLatency() const noexcept {
            return _latency;
        }

        /**
         * @brief Get the idle-spin floor for this mailbox's consumer.
         * @ingroup Engine
         * @return How long the consumer keeps polling after its last activity before it parks.
         *         Meaningless when `getLatency()` is 0.
         */
        [[nodiscard]] qb::duration
        getIdleSpin() const noexcept {
            return _idle_spin;
        }
    };

private:
    const CoreSet                         _core_set;
    std::vector<std::unique_ptr<Mailbox>> _mail_boxes;
    // Per-core "has left __workflow__" flag, indexed by RESOLVED core index (parallel to
    // _mail_boxes). Set (release) by a VirtualCore as the last thing before its worker
    // thread returns — after its final mailbox drain — so it will no longer accept cross-core
    // events. Read (acquire) by peers in their shutdown residual drain to tell a transiently
    // backpressured LIVE core (keep retrying — its __receive__ frees space) from one that has
    // GONE (dispose the residue that can never be delivered).
    std::vector<std::atomic<bool>> _core_stopped;

public:
    SharedCoreCommunication() = delete;
    explicit SharedCoreCommunication(CoreInitializerMap const &core_initializers) noexcept;

    ~SharedCoreCommunication() noexcept;

    /**
     * @brief Mark a core as having left its workflow (shutdown bookkeeping).
     * @param resolved_index The RESOLVED core index (as used for _mail_boxes / outbound pipes).
     * @details Single writer per index (the core itself), published with release ordering.
     */
    void
    mark_core_stopped(CoreId const resolved_index) noexcept {
        _core_stopped[resolved_index].store(true, std::memory_order_release);
    }

    /**
     * @brief Whether a core has left its workflow and no longer drains its mailbox.
     * @param resolved_index The RESOLVED core index.
     * @return true once the target core has finished __workflow__.
     */
    [[nodiscard]] bool
    is_core_stopped(CoreId const resolved_index) const noexcept {
        return _core_stopped[resolved_index].load(std::memory_order_acquire);
    }

    /**
     * @brief Dispose any events still sitting in the core mailboxes at teardown.
     * @details Closes the narrow shutdown window in which a peer's last cross-core flush lands
     *          in a core's mailbox AFTER that core's final __receive__ but before it publishes
     *          its stopped flag: the core never drains it, so its non-trivial QoS-2 payload
     *          would leak. Frees them via the global disposer registry (no-op for trivially
     *          destructible events).
     * @warning MUST be called only after every worker thread has joined — it is single-threaded
     *          and performs no synchronization against live producers/consumers.
     */
    void dispose_residual_mailbox_events() noexcept;

    /**
     * @brief Send an event to the mailbox of its destination VirtualCore.
     * @ingroup Engine
     * @param source_index The **physical** producer core's resolved index (the calling
     *        VirtualCore's `_resolved_index`). Selects the single-producer ring in the
     *        destination MPSC mailbox — it must be the sending thread's own core, not
     *        `event.source` (which `forward()` preserves as the original sender).
     * @param event The `qb::Event` to send. The event's `dest_core()` determines the target mailbox.
     * @return `true` if the event was successfully enqueued into the destination core's mailbox.
     *         `false` if the destination core ID is invalid, the mailbox is full (rare),
     *         or another error occurred during enqueueing.
     * @details This method is used internally by actors and the engine to route events between cores.
     *          It relies on the MPSC ringbuffer implementation for the mailboxes.
     */
    [[nodiscard]] bool send(CoreId source_index, Event const &event) const noexcept;

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
    const CoreId id;

    /** @brief Reference to the CoreInitializer for this core */
    CoreInitializer &initializer;

    /** @brief Reference to the shared communication infrastructure */
    SharedCoreCommunication &shared_com;

    /** @brief Atomic counter for synchronizing core startup */
    std::atomic<uint64_t> &sync_start;

    /**
     * @brief C++20 stop token wired to the engine's `qb::stop_source`.
     * @details
     * Each worker polls it from its event loop. Combined with the legacy
     * `Main::_signal_pending` path, it provides a deterministic, signal-free
     * cancellation channel (e.g. when `Main` is destroyed without any POSIX
     * signal being delivered).
     */
    qb::stop_token stop_token;
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
    constexpr static const uint64_t MaxRingEvents = (((std::numeric_limits<uint16_t>::max)()) / QB_LOCKFREE_EVENT_BUCKET_BYTES);
    //////// Types
    using Mailbox = lockfree::mpsc::ringbuffer<EventBucket, MaxRingEvents, 0>;

    static std::atomic<std::sig_atomic_t> _signal_pending;
    /// Bumped by `onSignal()` and `stop()` on every raised signal. `_signal_pending` only holds the
    /// LATEST signum and is never cleared during a run, so a per-core "already consumed" latch would
    /// drop every signal after the first (a SIGHUP reload then SIGTERM, or `stop()` after any earlier
    /// signal, left the engine unstoppable). Each `VirtualCore` re-synthesizes its `SignalEvent`
    /// whenever this generation advances past the one it last delivered — so distinct AND repeated
    /// signals are all delivered. Must stay lock-free for signal-handler safety (asserted in Main.cpp).
    static std::atomic<unsigned int> _signal_generation;

    std::atomic<uint64_t> _sync_start;
    static void           onSignal(int signal) noexcept;
    static void           start_thread(CoreSpawnerParameter const &params) noexcept;
    /// Install the documented default signal dispositions at engine start: SIGINT + SIGTERM
    /// everywhere, plus — on Windows — the console-control bridge (SIGBREAK, i.e. CTRL_BREAK
    /// and CTRL_CLOSE as the CRT raises them, translated to SIGTERM so cross-platform actor
    /// code and the default kill path work with no #ifdef). An explicit registerSignal(SIGBREAK)
    /// still delivers SIGBREAK untranslated.
    static void install_default_signals() noexcept;
    static bool __wait__all__cores__ready(std::size_t nb_core, std::atomic<uint64_t> &sync_start) noexcept;

private:
    /**
     * @brief Cancellation channel shared with all core workers.
     * @details
     * Tokens are dispatched through `CoreSpawnerParameter::stop_token` to each
     * `qb::jthread` in `_cores`. A `request_stop()` is issued on destruction
     * (and may be triggered programmatically) so worker loops exit cleanly
     * without requiring a POSIX signal. The destructor of `qb::jthread`
     * additionally joins the worker, making RAII-based shutdown automatic.
     */
    qb::stop_source          _stop_source;
    std::vector<qb::jthread> _cores;
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
     * // core_config.setLatency(std::chrono::microseconds(100)); // 100us
     * @endcode
     */
    [[nodiscard]] CoreInitializer &core(CoreId const index);

    /*!
     * @brief Set the default event loop latency for all VirtualCores.
     * @ingroup Engine
     * @param latency The maximum time in nanoseconds for cores to wait when idle. `0` means no wait (low latency mode).
     * @details This sets the latency for all cores that haven\'t had a specific latency set via `CoreInitializer::setLatency()`.\n
     *          See `CoreInitializer::setLatency()` for more details on latency values.
     * @attention This function is only available before the engine is running.
     */
    void setLatency(qb::duration latency = qb::duration::zero());

    /*!
     * @brief Set the idle-spin floor for all VirtualCores.
     * @ingroup Engine
     * @param idle_spin Time an idle core keeps polling before it parks; see `CoreInitializer::setIdleSpin()`.
     * @details Applies to every core, overriding any per-core value set earlier via `core(id).setIdleSpin()`.
     * @attention This function is only available before the engine is running.
     */
    void setIdleSpin(qb::duration idle_spin = CoreInitializer::kDefaultIdleSpin);

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
     * `Main::start()` installs `SIGINT` and `SIGTERM` itself — both are treated as terminal by
     * `Actor::on(SignalEvent&)`'s default body, so either one unwinds every actor through the
     * normal `kill()` path. (`SIGTERM` is a standard C signal and is installed on every platform;
     * Windows simply never delivers it outside `std::raise`.)\n
     * A signal you register yourself is delivered to actors as a `SignalEvent` but is **not**
     * terminal — `SIGHUP` / `SIGUSR1` are meant for config reload or a stats dump. Override
     * `on(SignalEvent&)` (re-registering the handler from your derived type in `onInit()`) to act
     * on it, and call `kill()` there if you do want it to stop the actor.\n
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

// ============================================================================================
// Main -- TEMPLATE BODIES.  TEMPLATES ONLY BELOW THIS LINE.
//
// Actor creation, core initialization and system configuration: the definitions of the member
// templates declared by `CoreInitializer`, `CoreInitializer::ActorBuilder` and `Main` above.
// They shipped as `core/Main.tpp` through 2.6.0 and moved here in 3.0 -- `.h` is now the only
// header extension in qb.
//
// The `#include "Actor.h"` below is deliberate in BOTH its presence and its position.
//   * Presence: the bodies need `TActorFactory` (Actor.h:2095), the `service_type` concept
//     (Actor.h:110) and `Service` (Actor.h:1722). Main.h's own DECLARATIONS need none of
//     them -- `IActorFactory` is forward-declared at Main.h:49 -- which is why this header
//     still compiles alone and why the include was never needed above.
//   * Position: at the tail, not in the include block at the top. Main.h is one of the most
//     densely cited headers in the readme book (31 `Main.h:NNN` citations across seven
//     pages, measured; it was written "~18 across five"); hoisting one line into the include
//     block would shift every one of them and force a re-point-by-offset, which is exactly
//     how a wrong citation gets propagated. Appending shifts nothing.
//     The `qb::NoAffinity` block above WAS extended in 3.0 -- because the old text implied
//     that `setAffinity()` pins on every platform, which is false on macOS -- and all 31
//     were then re-derived AT THE NEW COORDINATES rather than offset. Nine of them turned
//     out to have been wrong beforehand (off-by-one onto a closing `*/`, or pointing at an
//     unrelated line entirely), which is the argument for re-reading rather than shifting.
//   * NOT VirtualCore.h. Main.tpp used to pull it, and nothing here needs it: `Main::addActor`
//     goes through `core(cid)`, a `CoreInitializer` declared above. Adding it would also make
//     `<qb/core/Main.h>` alone drag <windows.h>/WIN32_LEAN_AND_MEAN/NOMINMAX into every TU,
//     and it would close a cycle (VirtualCore.h:64 includes this header).
//
// TEMPLATES ONLY, and the reason outlives the extension: this header is reached both by
// libqb-core's single amalgamated TU and by every consumer TU, and the include guard stops
// double inclusion only WITHIN one TU. Every definition below is a template, which gives it
// vague linkage; one non-template, non-`inline` definition added here is an instant duplicate
// symbol between the archive and any consumer object. `inline` is not the workaround -- it
// leaves N definitions of one entity in the program. Anything non-template belongs in Main.cpp.
// ============================================================================================
#include "Actor.h"

namespace qb {
class Main;

template <typename _Actor, typename... _Args>
ActorId
CoreInitializer::addActor(_Args &&...args) noexcept {
    ActorId id = ActorId::NotFound;
    // C++20: use service_type concept instead of std::is_base_of_v
    if constexpr (service_type<_Actor>) {
        if (_registered_services.find(_Actor::ServiceIndex) == _registered_services.end()) {
            _registered_services.insert(_Actor::ServiceIndex);
            id = ActorId(_Actor::ServiceIndex, _index);
        } else {
            QB_LOG_CRIT("[Start Sequence] Failed to add Service Actor(" << typeid(_Actor).name() << ")"
                                                                        << " in Core(" << _index << ")"
                                                                        << " : Already registered");
            return id;
        }
    } else {
        if (unlikely(_next_id == std::numeric_limits<ServiceId>::max())) {
            QB_LOG_CRIT("[Start Sequence] Failed to add Actor(" << typeid(_Actor).name() << ")"
                                                                << " in Core(" << _index << ")"
                                                                << " : Max number of Actors reached");
            return id;
        }
        id = ActorId(_next_id++, _index);
    }
    _actor_factories.push_back(std::make_unique<TActorFactory<_Actor, _Args...>>(id, std::forward<_Args>(args)...));
    return id;
}

template <typename _Actor, typename... _Args>
CoreInitializer::ActorBuilder &
CoreInitializer::ActorBuilder::addActor(_Args &&...args) noexcept {
    auto id = _initializer.template addActor<_Actor, _Args...>(std::forward<_Args>(args)...);
    if (!id.is_valid())
        _valid = false;

    _ret_ids.push_back(id);
    return *this;
}

template <typename _Actor, typename... _Args>
ActorId
Main::addActor(CoreId const cid, _Args &&...args) {
    return core(cid).addActor<_Actor>(std::forward<_Args>(args)...);
}

} // namespace qb
#endif // QB_MAIN_H
