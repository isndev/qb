# QB Framework: Error Handling & Resilience Guide

Building robust concurrent systems requires careful consideration of error handling and failure recovery. The Actor Model provides inherent isolation, but developers must still implement strategies to handle failures gracefully.

## 1. Actor-Level Error Handling (Internal)

Actors should strive to handle expected errors internally.

*   **Standard C++ Exceptions (`try...catch`):** Use within `on(Event&)` or `onCallback()` for operations that might throw exceptions (e.g., parsing invalid data, I/O errors from `qb::io::sys::file` if used directly, third-party library calls).
    ```cpp
    #include <stdexcept>
    #include <qb/actor.h>
    #include <qb/io.h>

    struct ProcessCommand : qb::Event { std::string command; };
    struct CommandResult : qb::Event { bool success; std::string info; };

    class CommandProcessor : public qb::Actor {
    public:
        bool onInit() override { registerEvent<ProcessCommand>(*this); return true; }

        void on(const ProcessCommand& event) {
            try {
                if (event.command == "FAIL") {
                    throw std::runtime_error("Deliberate failure requested");
                }
                std::string result = process(event.command);
                push<CommandResult>(event.getSource(), true, result);
            } catch (const std::invalid_argument& ia) {
                LOG_WARN("Invalid command format: " << ia.what());
                push<CommandResult>(event.getSource(), false, "Invalid Format");
            } catch (const std::exception& e) {
                LOG_ERROR("Error processing command '" << event.command << "': " << e.what());
                push<CommandResult>(event.getSource(), false, "Internal Error");
                // Consider if this error means the actor is now in an invalid state
                // if (isUnrecoverable(e)) { kill(); }
            }
        }
        std::string process(const std::string& cmd) { /* ... */ return "OK"; }
    };
    ```
*   **Status/Result Events:** For operations that can fail predictably (e.g., resource unavailable, validation failure), return status information in dedicated response events rather than throwing exceptions.
*   **Self-Termination (`kill()`):** If an actor encounters an error from which it cannot recover or which leaves it in an inconsistent state, it should call `kill()` to terminate itself cleanly.

## 2. Unhandled Exceptions & Core Termination

*   **Behavior:** If an exception is thrown from an `on()` handler or `onCallback()` and is *not* caught within the actor, the **`VirtualCore`** executing that actor will catch it.
*   **Default Action:** The `VirtualCore` logs the unhandled exception and then **terminates itself**. This is a fail-fast approach to prevent a potentially corrupted actor from continuing.
*   **Impact:** **All other actors running on the same `VirtualCore` will also stop processing** when the core terminates.
*   **Detection:** The `qb::Main` engine detects core termination. After `main.join()` returns, `main.hasError()` will return `true`.

**(Ref:** `test-main.cpp` checks `hasError()`, `test-actor-error-handling.cpp` simulates errors.**)

## 3. Supervision Strategies (Application Level)

QB Core does not provide built-in supervisor hierarchies like Akka or Erlang/OTP. Supervision must be implemented using actor patterns.

*   **Monitoring Liveness:**
    *   **Ping/Pong with Timeout:** A supervisor actor periodically `push`es a `Ping` event to child actors. Child actors `reply` with `Pong`. The supervisor uses `async::callback` to schedule a check after a timeout. If the `Pong` hasn't been received (requiring the supervisor to track pending pings), the child is considered unresponsive or dead.
    *   **`RequireEvent`:** Less suitable for continuous monitoring, but `require<T>` can be used initially or periodically to discover *currently* live actors of a specific type.
*   **Failure Detection:**
    *   **Timeout:** As above, failure to respond to pings indicates a likely failure.
    *   **Explicit Error Events:** A child actor, upon encountering an unrecoverable internal error, can `push` a specific `ChildFailedEvent(reason)` to its designated supervisor before calling `kill()`.
*   **Recovery Actions (by Supervisor):**
    *   **Restart:** The supervisor can create a new instance of the failed child actor (`addActor` or `addRefActor`). The new actor might need logic in `onInit` to recover state (e.g., load from persistence, request from peers).
    *   **Delegate:** The supervisor can redirect the failed actor's workload to other actors.
    *   **Escalate:** The supervisor can notify its own supervisor or a system manager about the failure.
    *   **Stop:** The supervisor might decide to stop itself or other related actors if the failure is critical.

```cpp
// --- Conceptual Supervisor ---
struct Ping : qb::Event {};
struct Pong : qb::Event {};
struct ChildFailed : qb::Event { std::string reason; };

class Supervisor : public qb::Actor {
private:
    std::map<qb::ActorId, bool> _child_status; // true = awaiting pong
    qb::ActorId _child_to_restart;

public:
    bool onInit() override {
        registerEvent<Pong>(*this);
        registerEvent<ChildFailed>(*this);
        registerEvent<InternalTimeoutCheck>(*this); // Event sent by async::callback

        // Create children and start monitoring
        auto child_id = addRefActor<MonitoredWorker>();
        _child_status[child_id] = false;
        scheduleHealthCheck(child_id);
        return true;
    }

    void scheduleHealthCheck(qb::ActorId child_id) {
        if (!_child_status.count(child_id)) return; // Child already gone

        push<Ping>(child_id);
        _child_status[child_id] = true; // Mark as awaiting pong

        auto self_id = id();
        double check_delay = 5.0; // Check after 5 seconds
        qb::io::async::callback([self_id, child_id](){
            // Send event back to self to perform check in actor context
             VirtualCore::_handler->push<InternalTimeoutCheck>(self_id, self_id, child_id);
        }, check_delay);
    }

    void on(const Pong& event) {
        if (_child_status.count(event.getSource())) {
            _child_status[event.getSource()] = false; // Mark as responsive
            // Schedule next check after some interval
            scheduleHealthCheck(event.getSource());
        }
    }

    void on(const ChildFailed& event) {
        handleChildFailure(event.getSource(), event.reason);
    }

    void on(const InternalTimeoutCheck& event) {
        if (!is_alive()) return;
        auto it = _child_status.find(event.child_id);
        if (it != _child_status.end() && it->second /* still awaiting pong */) {
            handleChildFailure(event.child_id, "Timeout");
        }
    }

    void handleChildFailure(qb::ActorId child_id, const std::string& reason) {
        qb::io::cout() << "Child " << child_id << " failed: " << reason << ". Restarting...\n";
        _child_status.erase(child_id); // Remove old entry
        // Actual restart logic:
        auto new_child = addRefActor<MonitoredWorker>();
        _child_status[new_child->id()] = false;
        scheduleHealthCheck(new_child->id());
    }

    // ... KillEvent handling ...
};
```

## Handling I/O Errors (`qb-io`)

Actors using `qb::io::use<>` need specific error handling:

*   **`on(event::disconnected const&)`:** Essential for network actors. Clean up state associated with the connection (e.g., remove session from maps), reset protocol state (`protocol()->reset()`), attempt reconnection if applicable.
*   **Protocol Errors:** If `AProtocol::getMessageSize/onMessage` detects invalid data, it should ideally call `reset()`. The I/O component handling the protocol might then call `disconnect()` on the transport, leading to the `disconnected` event in the actor.
*   **Socket Errors:** Low-level socket errors during `read`/`write` usually bubble up and cause the `qb::io::async::io` base to trigger the `dispose()` mechanism, eventually resulting in an `on(event::disconnected&)` call in the actor.

**(Ref:** `chat_tcp/client/ClientActor.cpp` (reconnection logic), `[QB-IO: Async System](./../3_qb_io/async_system.md)`**) 