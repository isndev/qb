/**
 * @file qb/core/VirtualCore.h
 * @brief Defines the VirtualCore class, representing a worker thread in the QB Actor Framework.
 *
 * This file contains the definition for the `VirtualCore` class, which is a fundamental
 * component of the QB Actor Framework. Each `VirtualCore` instance typically runs in its
 * own thread and is responsible for managing the lifecycle and event processing for a
 * set of actors assigned to it. It handles event queues, inter-core communication
 * via mailboxes, and the execution of actor event handlers.
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

#ifndef QB_CORE_H
#define QB_CORE_H
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

#if defined(unix) || defined(__unix) || defined(__unix__)
#include <cerrno>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#elif defined(_WIN32) || defined(_WIN64)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif // !WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <process.h>
#include <windows.h>
#endif

// include from qb
#include <qb/system/allocator/pipe.h>
#include <qb/system/container/unordered_map.h>
#include <qb/system/event/router.h>
#include <qb/system/lockfree/mpsc.h>
#include <qb/system/time.h>
#include <qb/utility/compat.h>
#include "Actor.h"
#include "Event.h"
#include "ICallback.h"
#include "Main.h"
#include "Pipe.h"

namespace qb {

/*!
 * @class VirtualCore
 * @ingroup Engine
 * @brief Manages a virtual processing core (worker thread) in the actor system.
 * @details
 * A VirtualCore is responsible for executing actors assigned to it. It runs an
 * event loop that processes incoming events for its actors, manages actor lifecycles
 * (initialization, termination), and handles inter-core communication by dispatching
 * events to and from other VirtualCores via mailboxes.
 * Each VirtualCore typically runs in its own dedicated thread.
 */
class VirtualCore {
    // `inline` + QB_ABI_ANCHOR, NOT an out-of-line definition in VirtualCore.cpp: an out-of-line
    // thread_local emits a `non-external` TLS descriptor, private to its image, so a host and a
    // statically-linked plugin each got their own "current VirtualCore" on one thread. See
    // qb/utility/abi.h for the measurement.
    QB_ABI_ANCHOR static inline thread_local VirtualCore *_handler = nullptr;
    /**
     * @brief Global, race-free counter of registered service types.
     * @details
     * Finding 2.3 — each `ServiceActor<Tag>` triggers a single registration
     * through `Actor::registerIndex<Tag>()` whose magic-static barrier guards
     * an `std::atomic` increment here. All subsequent reads are relaxed
     * (no happens-before needed: writes are published under the magic static
     * acquire edge).
     */
    QB_ABI_ANCHOR static inline std::atomic<ServiceId> _nb_service{0};
    /**
     * @brief Access the `Tag → ServiceId` registration map (lazy singleton).
     * @details
     * Reads are safe once all static initialisers have run. Writes are only
     * performed under `servicesMutex()` from `Actor::registerIndex<Tag>()`
     * inside a magic static, guaranteeing at most one insertion per `Tag`
     * regardless of the number of concurrent TU initialisations.
     */
    QB_ABI_ANCHOR static qb::unordered_map<TypeId, ServiceId> &
    getServices() {
        static qb::unordered_map<TypeId, ServiceId> service_ids;
        return service_ids;
    }
    /**
     * @brief Mutex protecting mutating access to `getServices()`.
     * @details Magic static → safe to call from any TU's static initialiser.
     */
    QB_ABI_ANCHOR static std::mutex &
    servicesMutex() noexcept {
        static std::mutex mtx;
        return mtx;
    }

public:
    /*!
     * @enum Error
     * @ingroup Engine
     * @brief Error codes for virtual core operations and states.
     *        These flags can be combined to represent multiple error conditions.
     */
    enum Error : uint64_t {
        BadInit         = (1u << 9u),  ///< General initialization error for the VirtualCore.
        NoActor         = (1u << 10u), ///< An expected actor was not found or couldn't be processed.
        BadActorInit    = (1u << 11u), ///< An actor's `onInit()` method returned false or threw an exception.
        ExceptionThrown = (1u << 12u)  ///< An unhandled exception occurred during VirtualCore execution (e.g., in an actor event handler).
    };

    // The startup barrier (Main::start / Main::__wait__all__cores__ready) packs
    // the "cores ready" count and the error sentinels into one atomic counter:
    // a value >= BadInit means a core failed to start, any smaller value is the
    // clean ready-count (each core fetch_add(1)). That separation only holds
    // while the largest possible clean count — bounded by the number of cores,
    // i.e. MaxCores — stays strictly below the first error sentinel (BadInit).
    static_assert(static_cast<uint64_t>(qb::MaxCores) < static_cast<uint64_t>(BadInit),
                  "startup-barrier sentinel space (Error::BadInit) must exceed "
                  "MaxCores so a clean ready-count is never mistaken for an error");

private:
    friend class Actor;
    friend class CoroContext;
    friend class Service;
    friend class CoreInitializer;
    friend class Main;
    template <typename>
    friend class ActorHandle; // RefActorHandle is an alias of ActorHandle
    ////////////
    constexpr static const uint64_t MaxRingEvents = ((std::numeric_limits<uint16_t>::max)() + 1) / QB_LOCKFREE_EVENT_BUCKET_BYTES;
    /// Widest event a destination mailbox ring can ever accept — see VirtualCore.cpp.
    static constexpr std::size_t kMaxDeliverableBuckets = SharedCoreCommunication::MaxRingEvents;
    // Types
    using Mailbox         = SharedCoreCommunication::Mailbox;
    using EventBuffer     = std::array<EventBucket, MaxRingEvents>;
    using ActorMap        = qb::unordered_map<ActorId, std::unique_ptr<Actor>>;
    using CallbackMap     = qb::unordered_map<ActorId, ICallback *>;
    using PipeMap         = std::vector<VirtualPipe>;
    using RemoveActorList = qb::unordered_set<ActorId>;

    /**
     * @class ServiceIdPool
     * @ingroup Engine
     * @brief O(1) bitset-based pool of free `ServiceId` slots.
     * @details
     * Replaces the historical `std::set<ServiceId>` free-list with a contiguous
     * bitset (8 KiB per VirtualCore) allowing branch-predicted, allocation-free,
     * constant-time `acquire` / `release` operations, plus efficient empty checks.
     *
     * A word-level cursor (`_next_word`) accelerates `acquire()` by resuming the
     * scan from the last known word containing a free bit instead of rescanning
     * from index 0. `std::countr_zero` extracts the first free bit per word in
     * a single CPU instruction on modern targets.
     *
     * Thread-model: owned exclusively by a single `VirtualCore` worker thread;
     * no synchronization is performed.
     */
    class ServiceIdPool {
    public:
        static constexpr std::size_t kBits  = ActorId::BroadcastSid; // 65535 usable SIDs.
        static constexpr std::size_t kWords = (kBits + 63u) / 64u;   // 1024 × 64-bit words.

    private:
        std::array<std::uint64_t, kWords> _bits{};        ///< Bit = 1 → SID free, bit = 0 → SID taken.
        std::size_t                       _next_word = 0; ///< Scan cursor for fast `acquire()`.
        std::size_t                       _count     = 0; ///< Cached population count.

    public:
        /**
         * @brief Mark all service ids in the closed range `[min_sid, kBits)` as free.
         * @param min_sid First service id to make available (typically `_nb_service + 1`).
         */
        void
        init(ServiceId min_sid) noexcept {
            _bits.fill(0u);
            _count     = 0;
            _next_word = 0;
            // Set bits [min_sid, kBits) to 1.
            for (std::size_t sid = static_cast<std::size_t>(min_sid); sid < kBits; ++sid) {
                _bits[sid / 64u] |= (std::uint64_t{1} << (sid % 64u));
                ++_count;
            }
            // Prime the cursor to the first word that contains a free bit.
            _next_word = static_cast<std::size_t>(min_sid) / 64u;
        }

        /**
         * @brief Acquire the smallest free service id available.
         * @return A valid `ServiceId` or `ActorId::BroadcastSid` if the pool is exhausted.
         */
        [[nodiscard]] ServiceId
        acquire() noexcept {
            if (!_count)
                return ActorId::BroadcastSid;
            for (std::size_t w = _next_word; w < kWords; ++w) {
                if (auto word = _bits[w]; word) {
                    const auto bit = static_cast<std::size_t>(std::countr_zero(word));
                    _bits[w]       = word & (word - 1u); // Clear lowest set bit.
                    _next_word     = w;                  // Remember for next acquire.
                    --_count;
                    return static_cast<ServiceId>(w * 64u + bit);
                }
            }
            return ActorId::BroadcastSid;
        }

        /**
         * @brief Return a service id to the pool.
         * @param sid A previously acquired service id.
         */
        void
        release(ServiceId sid) noexcept {
            if (sid >= kBits)
                return;
            const std::size_t   w = static_cast<std::size_t>(sid) / 64u;
            const std::uint64_t m = std::uint64_t{1} << (static_cast<std::size_t>(sid) % 64u);
            if (!(_bits[w] & m)) {
                _bits[w] |= m;
                ++_count;
                if (w < _next_word)
                    _next_word = w; // Backtrack cursor so smallest SID reuse is preferred.
            }
        }

        [[nodiscard]] bool
        empty() const noexcept {
            return _count == 0;
        }
        [[nodiscard]] std::size_t
        size() const noexcept {
            return _count;
        }
    };
    using AvailableIdList = ServiceIdPool;

    //! Types

private:
    // Members
    const CoreId             _index;
    const CoreId             _resolved_index;
    SharedCoreCommunication &_engine;
    // event reception
    Mailbox                     &_mail_box;
    std::unique_ptr<EventBuffer> _event_buffer;
    router::memh<Event>          _router;
    // event flush
    PipeMap                      _pipes;
    VirtualPipe                 &_mono_pipe_swap;
    std::unique_ptr<VirtualPipe> _mono_pipe;
    // actors management
    AvailableIdList _ids;
    ActorMap        _actors;
    CallbackMap     _actor_callbacks;
    /**
     * @brief Flat, cache-friendly snapshot of registered callbacks.
     * @details
     * Maintained in sync with `_actor_callbacks` on register / unregister so the
     * workflow loop can iterate without rebuilding a thread-local vector on every
     * iteration (finding 2.6). Each entry pairs the callback pointer (owned by the
     * actor instance) with the actor id, so the workflow loop can skip the
     * callback of an actor that was killed earlier in the *same* dispatch pass
     * (consistent with the event-kill path, which skips the whole callback phase
     * via `_actor_to_remove`).
     */
    struct CallbackEntry {
        ICallback *cb;
        ActorId    id;
    };
    std::vector<CallbackEntry> _callback_list;
    RemoveActorList            _actor_to_remove;
    /**
     * @brief Scratch buffer the reap loop drains `_actor_to_remove` into.
     * @details
     * `removeActor()` destroys the actor, which runs arbitrary user code (its destructor,
     * and any referenced actor it owns). That code may `kill()` a *different* actor, which
     * re-enters `killActor()` → `_actor_to_remove.insert()`. Iterating `_actor_to_remove`
     * directly therefore mutates the container mid-iteration: under NDEBUG it is the ska flat
     * hash set, whose growth rehash REALLOCATES the entry array and invalidates the live
     * iterator (release-only — the sanitizer presets use a node-based `std::unordered_set`
     * and cannot see it), and even where the iterator survives, an id landing behind the
     * cursor is silently dropped by the subsequent `clear()`, leaving a `!is_alive()` actor
     * in `_actors` forever so the core never terminates. The loop swaps into this buffer
     * instead and repeats until no new kill appears. Member (not `thread_local`) because the
     * reap block is a `goto` target — jumping across a block-scope thread_local's
     * initialization is best avoided.
     */
    RemoveActorList _actor_remove_batch;

    // --- Asynchronous actor initialization (the *Activating* phase) ----------
    //
    // An `onInit()` that performs a `co_await` suspends; the actor is then
    // *Activating* until its coroutine resumes to completion. While Activating:
    //   - its suspended `onInit()` frame is owned here (kept alive across the await,
    //     so the actor object safely outlives a `co_await` — deferred destroy),
    //   - inbound unicast *business* events are byte-copied into `stash` and replayed
    //     FIFO once it becomes active (the dispatch gate in `__receive_events__`),
    //   - an activation deadline bounds the window (mutual-init deadlock-proof).
    // The common case — `onInit()` completes synchronously (no `co_await`) — never
    // enters this map: it is the `empty()`-guarded slow path, mirroring the
    // `_actor_to_remove` fast-path discipline so non-async actors pay nothing.
    struct Activation {
        qb::io::async::task<bool>             init;                ///< owns the suspended onInit frame
        std::uint64_t                         deadline_ns = 0;     ///< wall-clock deadline (0 = none)
        bool                                  cancelling  = false; ///< deadline fired → scope cancelled, awaiting unwind
        std::vector<std::vector<EventBucket>> stash;               ///< FIFO of byte-copied inbound unicast events
    };
    qb::unordered_map<ActorId, Activation> _activating;
    RemoveActorList                        _dying_with_frame; ///< killed while their onInit frame was still suspended

    /// Per-actor stash cap: a wedged-in-init actor must not OOM the core.
    static constexpr std::size_t kActivationStashCap = 4096u;

public:
    /**
     * @brief Activation deadline in nanoseconds — bounds a suspended `onInit()`.
     * @details An actor whose async `onInit()` does not complete within this window is failed
     *          and removed (its coroutine scope is cancelled so the frame unwinds). This makes
     *          mutual-init deadlocks impossible and clamps no-timeout in-init asks. Default 5 s.
     *          Set it (in ns) **before** `qb::Main::start()` to tune it (e.g. lower in tests).
     *          A `0` value disables the bound — not recommended, it removes the deadlock guard.
     */
    QB_ABI_ANCHOR static inline std::uint64_t activation_deadline_ns =
        5ull * 1000u * 1000u * 1000u; // 5 s

private:
    // --- loop

    /**
     * @struct Metrics
     * @brief Per-core event-loop instrumentation and adaptive idle-backoff state.
     * @details
     * Tracks activity counters (events received, sent, I/O, bucket sizes, etc.) plus
     * an integer `_spin_credit` used to amortize the cost of blocking waits on an
     * empty mailbox. `_spin_credit` is seeded with the total work observed during
     * the previous iteration whenever the loop made progress (see `carry_over()`),
     * so a burst of activity buys a few additional lock-free polls before the core
     * is allowed to park itself on `mailbox.wait()`.
     */
    struct Metrics {
        std::uint64_t _spin_credit        = 0; ///< Remaining lock-free polls before blocking wait.
        std::uint64_t _nb_event_io        = 0;
        std::uint64_t _nb_event_received  = 0;
        std::uint64_t _nb_bucket_received = 0;
        std::uint64_t _nb_event_sent_try  = 0;
        std::uint64_t _nb_event_sent      = 0;
        std::uint64_t _nb_bucket_sent     = 0;
        std::uint64_t _nanotimer          = 0;

        /**
         * @brief Any evidence of work performed or attempted during this iteration?
         */
        [[nodiscard]] bool
        had_activity() const noexcept {
            return (_nb_event_sent + _nb_event_received + _nb_event_io + _nb_event_sent_try) != 0;
        }

        /**
         * @brief Refresh the spin credit and clear per-iteration event counters.
         * @details
         * Keeps `_spin_credit` at the total work observed this iteration (plus any
         * leftover credit), so a busy loop always stays on the lock-free fast path.
         * `_nanotimer` is intentionally preserved across iterations.
         */
        void
        carry_over() noexcept {
            const auto activity = _nb_event_sent + _nb_event_received + _nb_event_io + _nb_event_sent_try;
            const auto ts       = _nanotimer;
            const auto credit   = _spin_credit + activity;
            *this               = {};
            _spin_credit        = credit;
            _nanotimer          = ts;
        }
    } _metrics;
    unsigned int _last_signal_generation =
        0; ///< `Main::_signal_generation` value at this core's last SignalEvent synthesis; a newer value (a fresh signal or `Main::stop()`)
           ///< re-triggers delivery. Replaces the old single-shot `_signal_consumed` latch that dropped every signal after the first.
    bool _stop_delivered = false; ///< One-way latch: whether this core has delivered the cooperative `stop_token`'s synthetic SIGINT (the token
                                  ///< cannot be un-requested).
    /// Monotonic count of event-loop passes; surfaced to callbacks via `qb::LoopEvent::iteration`.
    std::uint64_t _loop_count = 0;
    /**
     * @brief Optional C++20 cancellation token wired from `qb::Main::_stop_source`.
     * @details
     * When a cancellation is requested (either via `~Main()` or an explicit
     * `qb::stop_source::request_stop()`), the workflow synthesises a virtual
     * `SIGINT` in the next iteration, providing a signal-free shutdown path
     * that also works on platforms without a POSIX signal mechanism.
     */
    qb::stop_token _stop_token;
    // !Members

    VirtualCore(CoreId id, SharedCoreCommunication &engine) noexcept;
    ~VirtualCore() noexcept;

    /**
     * @brief Install a cancellation token for this VirtualCore.
     * @param token The `qb::stop_token` produced by the owning `Main`
     *              instance's `qb::stop_source`.
     * @details Must be called by the worker thread function before entering
     *          `__workflow__`. The token is polled once per iteration.
     */
    void __set_stop_token__(qb::stop_token token) noexcept;

    /*!
     * @brief Generate a new actor ID
     * @return Newly generated actor ID for use within this core
     */
    [[nodiscard]] ActorId __generate_id__() noexcept;

    // Event Management
    template <typename _Event, typename _Actor>
    void registerEvent(_Actor &actor) noexcept;
    template <typename _Event, typename _Actor>
    void unregisterEvent(_Actor &actor) noexcept;
    void unregisterEvents(ActorId id) const noexcept;
    /*!
     * @brief Get or create a pipe to a specific core
     * @param core Target core ID
     * @return Reference to the virtual pipe for communication with the target core
     */
    [[nodiscard]] VirtualPipe &__getPipe__(CoreId core) noexcept;
    /**
     * @brief Dispatch a contiguous batch of events from a raw bucket buffer.
     * @param events A `std::span` view of the event buckets to route.
     * @details Accepts a `std::span` rather than a raw pointer + size pair
     *          (C++20, finding 2.17) so call sites benefit from bounds-aware
     *          construction and aggregate iteration without any runtime cost.
     */
    void __receive_events__(std::span<EventBucket> events);
    void __receive__();
    bool __flush_all__() noexcept;
    //! Shutdown residual drain helper: dispose the events queued in this core's outbound
    //! pipes whose destination core has already left __workflow__ (published its "stopped"
    //! flag) and will never drain its mailbox again — those events can never be delivered, so
    //! free their non-trivial QoS-2 payloads via the global disposer registry. Pipes to
    //! still-live (backpressured) peers are left untouched for retry. Returns true if any
    //! non-empty pipe still targets a live core. Shutdown path only.
    [[nodiscard]] bool __dispose_residual_to_stopped_cores__() noexcept;
    //! Event Management

    // Workflow
    bool __init__(CoreIdSet const &cores);
    bool __init__actors__();
    void __workflow__();
    //! Workflow

    // Actor Management
    /*!
     * @brief Initialize a new actor
     * @param actor Actor to initialize
     * @param doInit Whether to call the actor's init method
     * @return ID of the initialized actor or Invalid ID if initialization failed
     */
    [[nodiscard]] ActorId initActor(Actor &actor, bool doInit) noexcept;
    /*!
     * @brief Add an actor to the core
     * @param actor Actor to add
     * @param doInit Whether to call the actor's init method
     * @return ID of the added actor or Invalid ID if addition failed
     */
    [[nodiscard]] ActorId appendActor(std::unique_ptr<Actor> actor, bool doInit = false) noexcept;
    void                  removeActor(ActorId id) noexcept;

    // --- Asynchronous initialization driver (the *Activating* phase) ---------
    /// Outcome of driving an actor's `onInit()` coroutine once.
    enum class InitOutcome { ReadyTrue, ReadyFalse, Suspended };
    /**
     * @brief Drive `actor.onInit()` to its first `co_await` (or to completion).
     * @details Resumes the freshly created coroutine exactly once (`task`'s
     *          `initial_suspend` is `suspend_always`). If it completed synchronously
     *          (the common case) reports `ReadyTrue`/`ReadyFalse` and the caller frees
     *          the frame; if it suspended on a `co_await` reports `Suspended` and the
     *          caller hands the still-live frame to `__begin_activation__`. An uncaught
     *          exception is reported as `ReadyFalse` (init failure).
     */
    [[nodiscard]] InitOutcome __drive_init__(Actor &actor, qb::io::async::task<bool> &init) noexcept;
    /// Move a suspended `onInit()` frame into the Activating set (+arm deadline, flip phase).
    void __begin_activation__(Actor &actor, qb::io::async::task<bool> &&init) noexcept;
    /// True iff `id` is currently Activating (or dying with a still-live onInit frame).
    [[nodiscard]] bool __is_activating__(ActorId id) const noexcept;
    /// Byte-copy a gated inbound unicast event into the destination actor's FIFO stash.
    /// @return true if the event was taken into the stash (ownership transferred — the caller must
    ///         NOT dispose the original); false if it was dropped (cap overflow) and the caller
    ///         must `_router.dispose()` the original to free a non-trivial payload.
    [[nodiscard]] bool __stash_event__(ActorId dest, Event *event) noexcept;
    /// Per-iteration pump: complete finished inits, replay stashes, enforce deadlines.
    void __pump_activations__() noexcept;
    //! Actor Management

private:
    /*!
     * @brief Create and add a new actor to this core
     * @tparam _Actor Type of actor to create
     * @tparam _Init Types of initialization parameters
     * @param init Parameters for actor initialization
     * @return Pointer to the newly created actor or nullptr if creation failed
     */
    template <typename _Actor, typename... _Init>
    [[nodiscard]] _Actor *addReferencedActor(_Init &&...init) noexcept;
    /*!
     * @brief Get a service actor of specified type
     * @tparam _ServiceActor Type of service actor to get
     * @return Pointer to the service actor or nullptr if not found
     */
    template <typename _ServiceActor>
    [[nodiscard]] _ServiceActor *getService() const noexcept;

    /*!
     * @brief Safely resolve an `ActorId` to a live actor pointer on this core.
     * @tparam _Actor Expected dynamic type of the resolved actor.
     * @param id The actor identifier to look up.
     * @return Pointer to the live actor if `id` matches an alive actor of the
     *         expected type on this core, `nullptr` otherwise.
     * @details Backs `qb::RefActorHandle<T>::get()` (finding 2.9).
     */
    template <typename _Actor>
    [[nodiscard]] _Actor *findActor(ActorId id) const noexcept;

    /*!
     * @brief Untyped, phase-aware liveness query for an `ActorId` on this core.
     * @param id The actor identifier to test.
     * @return true iff `id` names an actor that is alive **and** active (its `onInit()` has
     *         completed) on this VirtualCore.
     * @details The type-erased sibling of `findActor<T>()`: one hash lookup, no `dynamic_cast`.
     *          It exists for bookkeeping that holds bare ids — a subscriber list, a routing
     *          table — and needs to drop entries whose actor has been destroyed. The framework
     *          does the same thing for its own subscription map in `removeActor()`
     *          (`unregisterEvents`), but a user-space mirror of that map has no such hook.
     *          Backs `Actor::is_actor_alive()`.
     */
    [[nodiscard]] bool isActorAlive(ActorId id) const noexcept;

    void killActor(ActorId id) noexcept;

    template <typename _Actor>
    void registerCallback(_Actor &actor) noexcept;
    void __unregisterCallback(ActorId id) noexcept;
    void unregisterCallback(ActorId id) noexcept;

private:
    // Event Api
    /*!
     * @brief Get a proxy pipe between two actors
     * @param dest Destination actor ID
     * @param source Source actor ID
     * @return Pipe connecting the source and destination actors
     */
    [[nodiscard]] Pipe getProxyPipe(ActorId dest, ActorId source) noexcept;
    /*!
     * @brief Attempt to send an event immediately
     * @param event Event to send
     * @return true if the event was sent successfully, false otherwise
     */
    [[nodiscard]] bool try_send(Event const &event) const noexcept;
    void               send(Event const &event) noexcept;
    /*!
     * @brief Push an event to the event queue
     * @param event Event to push
     * @return Reference to the pushed event in the queue
     */
    Event &push(Event const &event) noexcept;
    void   reply(Event &event) noexcept;
    void   forward(ActorId dest, Event &event) noexcept;

    template <typename T>
    static inline void fill_event(T &data, ActorId dest, ActorId source) noexcept;
    template <typename T, typename... _Init>
    void send(ActorId dest, ActorId source, _Init &&...init) noexcept;
    template <typename T, typename... _Init>
    void broadcast(ActorId source, _Init &&...init) noexcept;
    /*!
     * @brief Build and push an event to the event queue
     * @tparam T Type of event to create
     * @tparam _Init Types of initialization parameters
     * @param dest Destination actor ID
     * @param source Source actor ID
     * @param init Parameters for event initialization
     * @return Reference to the created and pushed event
     */
    template <typename T, typename... _Init>
    T &push(ActorId dest, ActorId source, _Init &&...init) noexcept;
    //! Event Api

public:
    VirtualCore() = delete;

    /*!
     * @brief Get the core's index.
     * @ingroup Engine
     * @return `CoreId` (unsigned short) representing the unique index of this VirtualCore.
     * @details This ID is assigned during engine initialization and is used in `ActorId` construction.
     */
    [[nodiscard]] CoreId getIndex() const noexcept;

    /*!
     * @brief Get the set of cores this VirtualCore is configured to communicate with.
     * @ingroup Engine
     * @return Const reference to a `CoreIdSet`.
     * @details This set typically includes all other VirtualCores in the system, allowing
     *          this core to send events to actors on those cores.
     */
    [[nodiscard]] const CoreIdSet &getCoreSet() const noexcept;

    /*!
     * @brief Get the current cached time for this VirtualCore's processing loop.
     * @ingroup Engine
     * @return `uint64_t` timestamp in nanoseconds since epoch.
     * @details
     * This timestamp is updated once at the beginning of each iteration of the VirtualCore's
     * main processing loop. All actors running on this core during that single iteration
     * will see the same value when calling `Actor::time()` (which internally calls this).
     * This is optimized for performance within a loop iteration but means it does not update
     * with true nanosecond precision *during* a single actor's event handling.
     * For a continuously updating high-precision clock, use `qb::wall_now()`.
     */
    [[nodiscard]] uint64_t time() const noexcept;
};
#ifdef QB_WITH_LOGGING
qb::io::log::stream &operator<<(qb::io::log::stream &os, qb::VirtualCore const &core);
#endif
std::ostream &operator<<(std::ostream &os, qb::VirtualCore const &core);

// ============================================================================================
// VirtualCore -- TEMPLATE BODIES.  TEMPLATES ONLY BELOW THIS LINE.
//
// Event routing, actor management and core communication: the definitions of the member
// templates declared in `class VirtualCore` above, plus the `ServiceActor<Tag>::ServiceIndex`
// static data member. They shipped as `core/VirtualCore.tpp` through 2.6.0 and moved here in
// 3.0 -- `.h` is now the only header extension in qb.
//
// The `.tpp` extension used to be the visible signal that ONLY TEMPLATES GO HERE. That signal
// is gone; the hazard it guarded is not. This header is reached by TWO disjoint populations --
// `Actor.cpp` (compiled once, into libqb-core.a) and every consumer TU that pulls an umbrella
// -- and the include guard stops the double inclusion only WITHIN one TU. It does nothing
// across TUs, and nothing at all between a consumer TU and the archive. Every definition below
// is a template, which gives it vague linkage; one non-template, non-`inline` definition added
// here is an instant duplicate symbol. `inline` is NOT the workaround -- it makes the link
// succeed and leaves N definitions of one entity in the program, the identity-duplication class
// this release spent a whole step fixing (see qb/utility/abi.h). Anything non-template belongs
// in VirtualCore.cpp.
// ============================================================================================

template <typename _Event, typename _Actor>
void
VirtualCore::registerEvent(_Actor &actor) noexcept {
    // Never subscribe an actor that failed id allocation (service-id pool
    // exhausted -> __generate_id__ returned NotFound). Its constructor still
    // runs registerEvent for the default events, but appendActor() rejects it
    // and destroys it without unsubscribing; a subscription keyed by the
    // NotFound id would then dangle and route to freed memory if any event is
    // sent to a default/NotFound ActorId.
    if (unlikely(!actor.id().is_valid()))
        return;
    QB_LOG_INFO("Actor(" << actor.id() << ") subscribed to " << ActorProxy::getName<_Event>());
    _router.subscribe<_Event>(actor);
}

template <typename _Event, typename _Actor>
void
VirtualCore::unregisterEvent(_Actor &actor) noexcept {
    QB_LOG_INFO("Actor(" << actor.id() << ") unsubscribed to " << ActorProxy::getName<_Event>());
    _router.unsubscribe<_Event>(actor);
}

template <typename _Actor, typename... _Init>
_Actor *
VirtualCore::addReferencedActor(_Init &&...init) noexcept {
    // Route through the same allocation customization point used by TActorFactory so
    // users who override qb::allocate_actor<_Actor> get consistent behaviour for
    // both engine-created and dynamically-added actors (PMR/pool support, 2.13).
    auto                   *raw_actor = qb::allocate_actor<_Actor>(std::forward<_Init>(init)...);
    std::unique_ptr<_Actor> actor_ptr(raw_actor);
    // Use the ActorProxy customization point so dynamically created actors get the
    // same demangled name and typed id_type as factory-created ones.
    ActorProxy::setTypeInfo<_Actor>(*raw_actor);
    if (appendActor(std::move(actor_ptr), true).is_valid())
        return raw_actor;
    return nullptr;
}

template <typename _Actor>
_Actor *
VirtualCore::findActor(ActorId const id) const noexcept {
    if (!id.is_valid())
        return nullptr;
    const auto it = _actors.find(id);
    if (it == _actors.end())
        return nullptr;
    Actor *raw = it->second.get();
    // Phase-aware: an actor whose async `onInit()` is still in flight (Activating) is NOT
    // yet handed out — a handle resolves it only once `is_active()`. For the sync-init
    // majority `is_active() == is_alive()`, so this is unchanged for them.
    if (!raw->is_active())
        return nullptr;
    // Fast path: id_type set by ActorProxy::setTypeInfo matches the requested type.
    if (raw->id_type == ActorProxy::getType<_Actor>())
        return static_cast<_Actor *>(raw);
    // Slow path: support downcasts to base classes derived from Actor.
    return dynamic_cast<_Actor *>(raw);
}

template <typename _ServiceActor>
_ServiceActor *
VirtualCore::getService() const noexcept {
    const auto &it = _actors.find(ActorId(_ServiceActor::ServiceIndex, _index));
    if (it == _actors.end()) {
        QB_LOG_CRIT("Failed to get Service[" << typeid(_ServiceActor).name() << "]"
                                          << " in Core(" << _index << ")"
                                          << " : does not exist");
        return nullptr;
    }
    // NOTE: getService is intentionally NOT phase-gated — a service legitimately looks
    // itself (or a peer) up from inside its own `onInit()` (a common bootstrap pattern),
    // where the actor is mid-init by definition. The phase-aware handle path (`ActorHandle`
    // via `findActor`) is where Activating actors are withheld from external callers.
    return dynamic_cast<_ServiceActor *>(it->second.get());
}

template <typename _Actor>
void
VirtualCore::registerCallback(_Actor &actor) noexcept {
    // Same guard as registerEvent: an actor that failed id allocation must not
    // leave a callback entry keyed by its NotFound id (it is about to be
    // destroyed by appendActor()).
    if (unlikely(!actor.id().is_valid()))
        return;
    auto [it, inserted] = _actor_callbacks.insert({actor.id(), &actor});
    if (inserted) {
        // Maintain the flat snapshot for the workflow loop (2.6).
        _callback_list.push_back({&actor, actor.id()});
    }
}

// Event API
template <typename T>
inline void
VirtualCore::fill_event(T &data, ActorId const dest, ActorId const source) noexcept {
    data.id     = data.template type_to_id<T>();
    data.dest   = dest;
    data.source = source;

    // C++20: use event_qos0_type and service_event_type concepts
    if constexpr (event_qos0_type<T>) {
        static_assert(std::is_trivially_destructible_v<T>, "EventQOS < 2 require to be trivially destructible");
    }

    if constexpr (service_event_type<T>) {
        data.forward = source;
        std::swap(data.id, data.service_event_id);
    }

    data.bucket_size = static_cast<uint16_t>(allocator::getItemSize<T, EventBucket>());
}

template <typename T, typename... _Init>
void
VirtualCore::send(ActorId const dest, ActorId const source, _Init &&...init) noexcept {
    router::ensure_disposer<Event, T>(); // no-op for the trivially-destructible events send requires
    auto &pipe = __getPipe__(dest._core_id);
    auto &data = pipe.template allocate<T>(std::forward<_Init>(init)...);

    fill_event(data, dest, source);

    if (dest._core_id != _index && try_send(data))
        pipe.free(data.bucket_size);
}

template <typename T, typename... _Init>
void
VirtualCore::broadcast(ActorId const source, _Init &&...init) noexcept {
    // Each core needs its OWN copy of the payload. Never `std::forward` the same
    // pack into more than one `send`: forwarding an rvalue moves it into the
    // first core's event and leaves every subsequent core constructing `T` from
    // moved-from arguments (e.g. an empty string). Pass the arguments as lvalues
    // so every `send` copy-constructs `T`.
    for (const auto it : _engine._core_set.raw())
        send<T>(BroadcastId(it), source, init...);
}

template <typename T, typename... _Init>
T &
VirtualCore::push(ActorId const dest, ActorId const source, _Init &&...init) noexcept {
    // THE enqueue funnel for Actor::push / to().push / reply / forward: guarantee this type can
    // be freed on any drop path even if no actor ever subscribes to it. See Pipe.h.
    router::ensure_disposer<Event, T>();
    auto &pipe = __getPipe__(dest._core_id);
    auto &data = pipe.template allocate_back<T>(std::forward<_Init>(init)...);

    fill_event(data, dest, source);

    return data;
}
//! Event Api

template <typename Tag>
inline const ServiceId ServiceActor<Tag>::ServiceIndex = Actor::registerIndex<Tag>();

} // namespace qb

// ============================================================================================
// Actor -- TEMPLATE BODIES.  HAZARD: READ BEFORE ADDING ANYTHING BELOW THIS LINE.
//
// Event handling, actor creation and inter-actor communication: the definitions of the member
// templates declared by `qb::Actor`, `Actor::EventBuilder`, `qb::CoroContext` and
// `qb::ActorHandle<T>`, plus the two `qb::detail` coroutine wrappers. They shipped as
// `core/Actor.tpp` through 2.6.0 and moved here in 3.0 -- `.h` is now the only header
// extension in qb.
//
// THEY ARE HERE, AND NOT AT THE TAIL OF Actor.h, AND THAT IS STRUCTURAL. Sixteen of these
// bodies name `VirtualCore::` in a nested-name-specifier, which requires a COMPLETE
// qb::VirtualCore. VirtualCore.h includes Actor.h at its line 61, so an `#include
// "VirtualCore.h"` placed at Actor.h's tail is a guard no-op and the class is still
// incomplete there -- `incomplete type 'qb::VirtualCore' named in nested name specifier`.
// This header is the one that CLOSES the cycle, so this is the position, and the position is
// the whole point: the fix a `.tpp` was reaching for was never a file, it was an ordering.
//
// That failure is ENTRY-POINT DEPENDENT and this is the trap for anyone re-testing it: a TU
// entering through <qb/actor.h> compiles the broken arrangement clean. Only <qb/main.h>
// (-> Main.h -> ... -> VirtualCore.h -> Actor.h) sees it. A verification matrix without a
// main.h-first entry point passes a broken tree. qb_main_h.cpp in
// scripts/installed-entry-points is that entry point, and it LINKS rather than parsing,
// because -fsyntax-only and -c both pass on a template whose definition ships in no TU the
// consumer can see.
//
// TEMPLATES ONLY. Every definition below is a template, and that is the only thing keeping
// the program linkable. This header is reached by TWO disjoint populations:
//   * Actor.cpp:29                          -> compiled once, into libqb-core.a
//   * qb/actor.h, qb/main.h, qb/patterns.h,
//     qb/core/patterns.h, and (since 3.0)
//     core/patterns/{discovery,supervisor}.h -> compiled in EVERY consumer TU
// The QB_CORE_H guard stops the double inclusion *within one TU*. It does nothing across TUs,
// and nothing at all between a consumer TU and the archive.
//
// Add ONE non-template function here and every consumer that includes an umbrella from two of
// its own TUs fails to link. Measured, on this file, with two consumer TUs + the archive:
//
//     namespace qb { int actor_tpp_helper(int x) { return x + 1; } }
//     ->  duplicate symbol 'qb::actor_tpp_helper(int)' in: t2.o / t1.o
//         ld: 1 duplicate symbols
//
// The structure has existed since 2019-02-26 and has never fired, purely because nobody has
// added a non-template. Through 2.6.0 the `.tpp` extension was the guard, and it was a social
// one; 3.0 retired the extension and this banner is what replaces it. The hazard outlives the
// file name (see dev/analysis/TEMPLATE-LINKAGE-AUDIT-3.0 and TPP-INVESTIGATION). Anything that
// must be non-template belongs in Actor.cpp.
//
// `inline` is NOT a workaround. It makes the link succeed and leaves N definitions of one
// entity in the program, which is the identity-duplication class this release spent a whole
// step fixing (see qb/utility/abi.h).
// ============================================================================================
#include <cassert> // carried from Actor.tpp:27; vestigial there too, kept so the move drops nothing

namespace qb {

template <callback_type _Actor>
void
Actor::registerCallback(_Actor &actor) const noexcept {
    VirtualCore::_handler->registerCallback(actor);
}

template <callback_type _Actor>
void
Actor::unregisterCallback(_Actor &actor) const noexcept {
    VirtualCore::_handler->unregisterCallback(actor.id());
}

template <event_type _Event, actor_type _Actor>
void
Actor::registerEvent(_Actor &actor) const noexcept {
    VirtualCore::_handler->registerEvent<_Event>(actor);
}

template <event_type _Event, actor_type _Actor>
void
Actor::unregisterEvent(_Actor &actor) const noexcept {
    VirtualCore::_handler->unregisterEvent<_Event>(actor);
}

template <typename _Event>
void
Actor::unregisterEvent() const noexcept {
    VirtualCore::_handler->unregisterEvent<_Event>(*this);
}

template <typename _Actor, typename... _Args>
ActorHandle<_Actor>
Actor::addRefActor(_Args &&...args) const {
    return ActorHandle<_Actor>(VirtualCore::_handler->template addReferencedActor<_Actor>(std::forward<_Args>(args)...));
}

template <typename _Actor>
_Actor *
ActorHandle<_Actor>::get() const noexcept {
    if (!_id.is_valid())
        return nullptr;
    // Must be resolved from the owning VirtualCore's worker thread.
    auto *handler = VirtualCore::_handler;
    if (!handler)
        return nullptr;
    // findActor is phase-aware: nullptr while the actor is still Activating, and after it
    // failed init / died. So `get()` returns a usable pointer only for an *active* actor.
    auto *resolved = handler->template findActor<_Actor>(_id);
    // Keep the cache fresh if it drifted (e.g. handle copied after kill).
    const_cast<ActorHandle<_Actor> *>(this)->_cached = resolved;
    return resolved;
}

template <typename _Event, typename... _Args>
_Event &
Actor::push(ActorId const &dest, _Args &&...args) const noexcept {
    return VirtualCore::_handler->template push<_Event>(dest, id(), std::forward<_Args>(args)...);
}

template <typename _Event, typename... _Args>
void
Actor::send(ActorId const &dest, _Args &&...args) const noexcept {
    VirtualCore::_handler->template send<_Event, _Args...>(dest, id(), std::forward<_Args>(args)...);
}

template <typename _Event, typename... _Args>
_Event
Actor::build_event(ActorId const source, _Args &&...args) const noexcept {
    _Event event{std::forward<_Args>(args)...};
    VirtualCore::fill_event(event, id(), source);
    return event;
}

template <typename _Required>
bool
Actor::require_type() const noexcept {
    broadcast<PingEvent>(type_id<_Required>());
    return true;
}

template <typename... _Types>
bool
Actor::require() const noexcept {
    return (require_type<_Types>() && ...);
}

template <typename _Event, typename... _Args>
void
Actor::broadcast(_Args &&...args) const noexcept {
    VirtualCore::_handler->template broadcast<_Event, _Args...>(id(), std::forward<_Args>(args)...);
}

template <typename Tag>
ActorId
Actor::getServiceId(CoreId const index) noexcept {
    // Route through `registerIndex<Tag>()` so that callers hitting this path
    // *before* `ServiceActor<Tag>::ServiceIndex` has been touched (rare, but
    // possible when DSOs are loaded lazily) still observe a valid, unique
    // service id rather than the `0` stamped by `unordered_map::operator[]`
    // on a fresh key (2.3).
    return {registerIndex<Tag>(), index};
}

template <typename _ServiceActor>
_ServiceActor *
Actor::getService() const noexcept {
    return VirtualCore::_handler->getService<_ServiceActor>();
}

template <event_type _Event, typename... _Args>
Actor::EventBuilder &
Actor::EventBuilder::push(_Args &&...args) noexcept {
    dest_pipe.push<_Event>(std::forward<_Args>(args)...);
    return *this;
}

template <typename Tag>
ServiceId
Actor::registerIndex() noexcept {
    // Magic static — the C++ standard guarantees that the initializer runs
    // exactly once, even if multiple threads race on first call. This fully
    // serialises the `_nb_service` increment and the insertion into the
    // registration map, fixing the non-atomic mutation reported in 2.3.
    static const ServiceId idx = [] {
        const auto                  new_id = VirtualCore::_nb_service.fetch_add(1, std::memory_order_relaxed) + 1;
        std::lock_guard<std::mutex> lk(VirtualCore::servicesMutex());
        VirtualCore::getServices()[type_id<Tag>()] = new_id;
        return new_id;
    }();
    return idx;
}

// CoroContext implementation (needs VirtualCore access)

template <typename _Event, typename... Args>
void
CoroContext::push(Args &&...args) const {
    VirtualCore::_handler->template push<_Event>(actor_id_, actor_id_, std::forward<Args>(args)...);
}

template <typename _Event, typename... Args>
void
CoroContext::push_to(ActorId dest, Args &&...args) const {
    VirtualCore::_handler->template push<_Event>(dest, actor_id_, std::forward<Args>(args)...);
}

template <typename _Event, typename... Args>
void
CoroContext::broadcast(Args &&...args) const {
    VirtualCore::_handler->template broadcast<_Event>(actor_id_, std::forward<Args>(args)...);
}

template <typename E>
bool
Actor::resolve_ask(E &e) const noexcept {
    return qb::detail::ask_deliver(e.correlation_id, id(), e);
}

// Coroutine support implementation

namespace detail {

/**
 * @brief RAII guard that decrements the actor's active-coroutine counter when a
 *        wrapper frame completes or is destroyed (even after the owning actor is
 *        gone — the shared_ptr keeps the counter alive). Shared by both spawn
 *        wrappers so the decrement logic lives in one place.
 */
struct coro_count_guard {
    std::shared_ptr<std::atomic<std::size_t>> c;
    ~coro_count_guard() {
        c->fetch_sub(1, std::memory_order_relaxed);
    }
};

/**
 * Wrapper coroutine that owns the user's callable and the CoroContext
 * by value inside the coroutine frame. This prevents the "dangling lambda"
 * problem: when spawn_detached() receives a temporary lambda, the closure is
 * destroyed after the call — but the coroutine frame (which holds `func`
 * and `ctx` as value parameters) keeps them alive across all suspension
 * points.
 *
 * The shared_ptr<atomic> counter is decremented via RAII when the wrapper
 * completes or is destroyed (even if the owning actor has been deleted).
 *
 * Finding 2.D.6 — decision log:
 *   Two coroutine frames per spawn_detached are inherent to the design
 *   (wrapper frame + user body frame). We **intentionally** keep the
 *   wrapper because:
 *     1. Frame allocation cost is already amortised by the thread-local
 *        pooled allocator (Finding 2.A.9) — a steady-state spawn/despawn
 *        cycle burns zero `malloc`/`free`.
 *     2. Removing the wrapper would require either embedding a completion
 *        hook in every `task<void>::promise_type` (taxes all coroutines
 *        in the codebase, even non-actor ones) or a scheduler-side
 *        completion map (extra hash lookup per frame teardown).
 *     3. The wrapper is the single point that enforces actor lifetime
 *        safety (counter RAII + value-capture of func/ctx).
 */
template <typename Func>
qb::io::async::task<void>
actor_coro_wrapper(Func func, CoroContext ctx, std::shared_ptr<std::atomic<std::size_t>> counter) {
    coro_count_guard guard{std::move(counter)};
    co_await func(ctx);
}

/**
 * Wrapper for `spawn`. Same ownership / counter-RAII semantics as
 * `actor_coro_wrapper`, but it **swallows `qb::io::async::cancelled_error`** — the
 * expected signal when the actor's coroutine scope is cancelled on kill/destroy, so a
 * scoped coroutine being torn down is not surfaced as an error. Templated on the
 * context type so it accepts a `ScopedCoroContext` by value (kept alive in the frame).
 */
template <typename Func, typename Ctx>
qb::io::async::task<void>
actor_scoped_coro_wrapper(Func func, Ctx ctx, std::shared_ptr<std::atomic<std::size_t>> counter) {
    coro_count_guard guard{std::move(counter)};
    try {
        co_await func(ctx);
    } catch (const qb::io::async::cancelled_error &) {
        // Expected on actor-scope cancellation (kill/destroy) — swallow.
    }
}

} // namespace detail

template <typename Func>
void
Actor::spawn_detached(Func &&func) const {
    __resolve_coro_scheduler__();

    active_coroutines_->fetch_add(1, std::memory_order_relaxed);
    CoroContext ctx(this);

    // actor_coro_wrapper takes func BY VALUE → stored in the coroutine frame.
    // The scheduler takes ownership of the wrapper task's handle via spawn().
    coro_scheduler_->spawn(detail::actor_coro_wrapper(std::forward<Func>(func), ctx, active_coroutines_));
}

template <typename Func>
void
Actor::spawn(Func &&func) const {
    __resolve_coro_scheduler__();
    __ensure_coro_scope__(); // lazily allocate the real cancellation token on first use.

    active_coroutines_->fetch_add(1, std::memory_order_relaxed);
    // ScopedCoroContext carries the actor id + a copy of the scope token; the wrapper
    // stores it by value in the coroutine frame, so the token's shared state safely
    // outlives the actor.
    ScopedCoroContext ctx(this, _coro_scope);

    coro_scheduler_->spawn(detail::actor_scoped_coro_wrapper(std::forward<Func>(func), std::move(ctx), active_coroutines_));
}

} // namespace qb
#endif // QB_CORE_H
