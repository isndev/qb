@page guides_error_handling_md QB Framework: Error Handling & Resilience Strategies
@brief A practical guide to designing robust actor systems in QB by effectively handling errors and implementing resilience patterns.

# QB Framework: Error Handling & Resilience Strategies

Building concurrent and distributed systems with actors requires a thoughtful approach to error handling and fault tolerance. While the QB Actor Framework provides isolation that can contain failures, it's up to the developer to implement strategies for detecting, managing, and recovering from errors. This guide outlines key techniques for creating resilient QB applications.

## 1. Actor-Level Error Management (Internal Robustness)

Each actor should be responsible for handling errors that occur within its own operational scope where possible.

*   **Standard C++ Exception Handling (`try...catch`):**
    *   **Use Case:** Within `on(Event&)` handlers or `onCallback()` methods, wrap operations that might throw exceptions. This includes interactions with external libraries, complex parsing, or any logic that could fail under specific conditions.
    *   **Action:** Catch specific exceptions, log the error, potentially send a status/error event back to a requester, or decide if the actor needs to enter a specific error state or terminate itself.
    ```cpp
    #include <qb/actor.h>
    #include <qb/event.h>
    #include <qb/io.h> // For qb::io::cout
    #include <qb/string.h>
    #include <stdexcept> // For std::runtime_error, std::invalid_argument

    struct ProcessCommandReq : qb::Event { qb::string<128> command; };
    struct CommandStatusRes : qb::Event { bool success; qb::string<256> info; };

    class CommandHandlerActor : public qb::Actor {
    public:
        bool onInit() override {
            registerEvent<ProcessCommandReq>(*this);
            registerEvent<qb::KillEvent>(*this);
            return true;
        }

        void on(const ProcessCommandReq& event) {
            try {
                if (event.command == "CRASH_NOW") {
                    throw std::runtime_error("Simulated critical failure in command processing");
                }
                if (event.command.empty()) {
                    throw std::invalid_argument("Command cannot be empty");
                }
                // ... process command ...
                qb::io::cout() << "Actor [" << id() << "] processed command: " << event.command.c_str() << ".\n";
                push<CommandStatusRes>(event.getSource(), true, "Command processed successfully");
            } catch (const std::invalid_argument& ia) {
                qb::io::cout() << "Actor [" << id() << "] Error: Invalid command argument - " << ia.what() << ".\n";
                push<CommandStatusRes>(event.getSource(), false, "Invalid command format");
            } catch (const std::exception& e) {
                qb::io::cout() << "Actor [" << id() << "] Error: Failed to process command '" 
                               << event.command.c_str() << "': " << e.what() << ".\n";
                push<CommandStatusRes>(event.getSource(), false, "Internal processing error");
                // For a severe, unrecoverable error, the actor might decide to terminate:
                // if (isUnrecoverable(e)) { 
                //     qb::io::cout() << "Actor [" << id() << "]: Unrecoverable error, terminating.\n";
                //     kill(); 
                // }
            }
        }
        void on(const qb::KillEvent& /*event*/) { kill(); }
    };
    ```

*   **Status/Result Events for Predictable Failures:** For operations where failure is a common or expected outcome (e.g., validation failing, resource temporarily unavailable), it's often cleaner to communicate this via specific fields in your response events rather than throwing exceptions for control flow.
    ```cpp
    struct ValidateDataReq : qb::Event { int data_to_validate; };
    struct ValidationRes : qb::Event {
        boolis_valid;
        qb::string<128> validation_message;
        // ... constructor ...
    };
    // Responder would check data and push<ValidationRes>(source, true/false, message);
    ```

*   **Self-Termination via `kill()`:** If an actor encounters an internal error that leaves its state inconsistent or prevents it from functioning correctly, it should call `this->kill()` to initiate its own graceful shutdown. This is often the last step in a `catch` block for critical errors or after handling a specific `StopProcessingOrderEvent`.

## 2. Unhandled Exceptions and `VirtualCore` Termination

This is a critical aspect of QB's behavior:

*   **What Happens:** If an exception is thrown from an actor's `on(Event&)` handler or `onCallback()` method and is **not caught within that actor's code**, the `VirtualCore` executing that actor will catch the exception.
*   **Default Framework Action:** Upon catching such an unhandled exception, the `VirtualCore` will log the error (if logging is configured) and then **terminate itself**. This is a "fail-fast" approach designed to prevent a potentially corrupted actor or `VirtualCore` from causing further damage or inconsistent behavior in the system.
*   **Impact on Other Actors:** When a `VirtualCore` terminates, **all other actors assigned to that same `VirtualCore` will also cease processing new events.** Their `qb::ICallback::onCallback()` methods will no longer be called. They become effectively unresponsive from the perspective of the rest of the system.
*   **System-Level Detection:** The `qb::Main` engine monitors its `VirtualCore` threads. After `main.join()` returns (or if `main.start(false)` was used and it unblocks), you can call `main.hasError()`. This method will return `true` if one or more `VirtualCore`s terminated due to an error (like an unhandled exception).

**(Reference:** `test-main.cpp` includes tests for `hasError()` functionality. `test-actor-error-handling.cpp` simulates various actor error conditions.)**

## 3. Application-Level Supervision Strategies

QB-Core does not provide a built-in, Erlang-style supervisor hierarchy. Instead, you implement supervision using standard actor patterns. This gives you flexibility but requires explicit design.

*   **Monitoring Liveness (Health Checks):**
    *   **Pattern:** A supervisor actor periodically sends a `PingHealthEvent` to its child/worker actors. Monitored actors are expected to promptly reply with a `PongHealthResponse`.
    *   **Timeout:** The supervisor uses `qb::io::async::callback` to schedule a check for each `PingHealthEvent`. If a `PongHealthResponse` isn't received for a specific child within the timeout, that child is considered unresponsive or may have terminated silently.
    *   **State:** The supervisor needs to maintain state about pending pings (e.g., a map of `ActorId` to a timestamp or a request ID).
*   **Explicit Failure Reporting by Children:**
    *   A child actor, upon encountering an unrecoverable internal error (even one it catches), can proactively notify its designated supervisor by `push`ing a specific `ChildFailedNotificationEvent(reason, child_id)` before calling `this->kill()`.
*   **Supervisor's Recovery Actions:** Based on detected failures (timeout or explicit notification), a supervisor can take various actions:
    *   **Restart:** Create a new instance of the failed child actor (`addActor` or `addRefActor`). The newly created actor might need its own logic in `onInit()` to recover its state (e.g., load from a persistent store, query sibling actors, or start fresh).
    *   **Delegate:** Reassign pending tasks or responsibilities of the failed actor to other existing, healthy workers.
    *   **Escalate:** If the failure is critical or a child repeatedly fails, the supervisor might notify its own supervisor (if part of a deeper hierarchy) or a system-level manager/alerting actor.
    *   **Stop/Degrade Service:** In some cases, the supervisor might decide to stop itself or other dependent actors, or operate in a degraded mode if a critical dependency is lost.

*   **Conceptual Supervisor Snippet:**
    ```cpp
    // Events for supervision
    struct PingWorkerEvent : qb::Event {};
    struct PongWorkerResponse : qb::Event {};
    struct WorkerTimeoutCheck : qb::Event { qb::ActorId worker_id; }; // Self-sent by supervisor
    struct WorkerErrorReport : qb::Event { qb::string<128> error_details; };

    class WorkerSupervisor : public qb::Actor {
    private:
        std::map<qb::ActorId, qb::TimePoint> _pending_pings; // Worker ID -> Ping Sent Time
        std::vector<qb::ActorId> _worker_pool;
        const qb::Duration PING_TIMEOUT = qb::literals::operator""_s(5); // 5 seconds

    public:
        bool onInit() override {
            // ... create worker actors and store their IDs in _worker_pool ...
            // for (qb::ActorId worker_id : _worker_pool) { sendPingAndScheduleCheck(worker_id); }
            registerEvent<PongWorkerResponse>(*this);
            registerEvent<WorkerTimeoutCheck>(*this);
            registerEvent<WorkerErrorReport>(*this);
            registerEvent<qb::KillEvent>(*this);
            return true;
        }

        void sendPingAndScheduleCheck(qb::ActorId worker_id) {
            if (!isActorKnownAndAlive(worker_id)) return; // Simplified check
            push<PingWorkerEvent>(worker_id);
            _pending_pings[worker_id] = qb::HighResTimePoint::now();
            
            qb::io::async::callback([this, worker_id](){
                if (this->is_alive()) this->push<WorkerTimeoutCheck>(this->id(), worker_id);
            }, PING_TIMEOUT.seconds_float());
        }

        void on(const PongWorkerResponse& event) {
            _pending_pings.erase(event.getSource()); // Pong received, clear pending
            // Optionally, schedule next ping after an interval
            // qb::io::async::callback([this, sid=event.getSource()](){ if(this->is_alive()) sendPingAndScheduleCheck(sid);}, 30.0);
        }

        void on(const WorkerTimeoutCheck& event) {
            if (_pending_pings.count(event.worker_id)) {
                qb::io::cout() << "Supervisor: Worker " << event.worker_id << " timed out!\n";
                _pending_pings.erase(event.worker_id);
                handleWorkerFailure(event.worker_id, "Ping Timeout");
            }
        }
        void on(const WorkerErrorReport& event) {
            qb::io::cout() << "Supervisor: Worker " << event.getSource() << " reported error: " << event.error_details.c_str() << ".\n";
            handleWorkerFailure(event.getSource(), event.error_details.c_str());
        }
        
        void handleWorkerFailure(qb::ActorId failed_worker_id, const qb::string<128>& reason) {
            // Remove from active pool, potentially restart, log, etc.
            // Example: _worker_pool.erase(std::remove(_worker_pool.begin(), _worker_pool.end(), failed_worker_id), _worker_pool.end());
            //          auto new_worker_id = addActor<MyWorkerType>(getIndex(), /*..args..*/); 
            //          _worker_pool.push_back(new_worker_id); sendPingAndScheduleCheck(new_worker_id);
        }
        // ... KillEvent handler to stop pings and workers ...
        bool isActorKnownAndAlive(qb::ActorId /*id*/) { return true; } // Placeholder
    };
    ```

## 4. Handling Asynchronous I/O Errors (from `qb-io`)

Actors that perform network or file I/O using `qb::io::use<>` helpers must be prepared to handle I/O-related errors, typically signaled by `qb-io` events:

*   **`on(qb::io::async::event::disconnected const& event)`:** This is the most common error notification for network actors. It's triggered by:
    *   Graceful peer shutdown.
    *   Network errors (connection reset, host unreachable).
    *   Internal socket errors detected by `qb-io`'s underlying socket operations.
    *   Explicit `this->disconnect()` called by the actor itself.
    *   **Action:** Clean up state associated with the connection (e.g., session data), reset protocol state (`if (this->protocol()) this->protocol()->reset();`), attempt reconnection if it's a client, or notify a manager if it's a server-side session.
*   **Protocol Errors:** If your `AProtocol` implementation detects malformed data (e.g., in `getMessageSize()` or `onMessage()`), it should ideally:
    1.  Call its own `reset()` method to clear its parsing state.
    2.  Optionally, call `this->not_ok()` on the `AProtocol` instance to mark it as invalid.
    3.  The I/O component (your actor) might then check `if (!this->protocol()->ok())` and decide to call `this->disconnect()` on its transport, which will eventually lead to an `on(event::disconnected&)` callback.
*   **Low-Level Socket Errors:** Errors from direct socket calls (`read`, `write`) within the transport layer are generally caught by the `qb::io::async::io` (or `input`/`output`) base classes. These errors typically lead to the `dispose()` mechanism being invoked, which then triggers the `on(event::disconnected&)` handler in your actor.

**(Reference:** Client actors in `chat_tcp/client/ClientActor.cpp` and `message_broker/client/ClientActor.cpp` demonstrate reconnection logic in `on(event::disconnected&)`.)**

By combining these strategies—internal actor robustness, awareness of framework behavior for unhandled exceptions, application-level supervision, and proper handling of I/O events—you can build QB actor systems that are significantly more resilient to failures and easier to maintain.

**(Next:** [QB Framework: Effective Resource Management](./resource_management.md) to learn about managing actor and system resources effectively.**) 