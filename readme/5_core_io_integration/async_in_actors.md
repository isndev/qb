# Integrating Core & IO: Async Operations in Actors

Actors can directly leverage `qb-io`'s asynchronous capabilities for timers, callbacks, and file operations without blocking their `VirtualCore`.

## Using `qb::io::async::callback` within Actors

(`qb/include/qb/io/async/io.h`)

This is the primary way to schedule delayed or deferred actions within an actor.

*   **How:** Call `qb::io::async::callback(lambda, delay_in_seconds)` from `onInit`, an `on(Event&)` handler, or `onCallback`.
*   **Execution Context:** The lambda executes on the **same `VirtualCore`** as the actor that scheduled it.
*   **Use Cases:**
    *   **Delayed Actions:** Schedule cleanup, send a follow-up message, or transition state after a pause. (See `example3_lifecycle.cpp::SupervisorActor` scheduling status checks).
    *   **Periodic Actions (Alternative to `ICallback`):** Schedule the next execution from within the callback itself.
    *   **Breaking Tasks:** Divide a long computation into steps, scheduling the next step via callback.
    *   **Simulating Work:** Introduce artificial delays. (See `example8_state_machine.cpp::CoffeeMachineActor` simulating brew/dispense times).
    *   **Retries:** Schedule a connection or operation retry after a failure. (See `chat_tcp/client/ClientActor.cpp::attemptConnect` scheduling retry on failure).
*   **Capturing `this`:** Lambdas often capture `this`. **Crucially, check `is_alive()` at the beginning of the callback** if the actor might be killed before the callback executes.
*   **Sending Events from Callbacks:** Callbacks don't inherit from `Actor`. The cleanest pattern is often to have the callback `push` an event *back to the scheduling actor itself* (using a captured `self_id`), and let that actor's handler then interact with the rest of the system.

```cpp
// Inside MyActor class

struct InternalTimeoutEvent : qb::Event { int request_id; };
std::map<int, bool> _pending_requests;

void sendRequestWithTimeout(qb::ActorId target, int request_id, double timeout_s) {
    // Store request state
    _pending_requests[request_id] = true;

    // Send the actual request
    push<MyRequest>(target, request_id, /*...*/);

    // Schedule the timeout check
    auto self_id = id(); // Capture ID
    qb::io::async::callback([self_id, request_id]() {
        // Push an internal event back to self to handle timeout check
        // Need VirtualCore handler access to push directly
         VirtualCore::_handler->push<InternalTimeoutEvent>(self_id, self_id, request_id);
    }, timeout_s);
}

void on(const MyResponse& response) {
    // Response received, remove from pending map
    _pending_requests.erase(response.request_id);
    // Process response...
}

void on(const InternalTimeoutEvent& event) {
    if (!is_alive()) return;

    // Check if the request is still pending (i.e., response wasn't received)
    auto it = _pending_requests.find(event.request_id);
    if (it != _pending_requests.end()) {
        std::cout << "Request " << event.request_id << " timed out!" << std::endl;
        // Handle timeout: e.g., retry, notify user, etc.
        _pending_requests.erase(it); // Clean up pending entry
    }
    // If not found, the response arrived just before the timeout event was processed
}
```
**(Ref:** `test-actor-delayed-events.cpp`, `example3_lifecycle.cpp`, `example8_state_machine.cpp`, `file_processor/file_worker.h`, `chat_tcp/client/ClientActor.cpp`**)

## Using `qb::io::async::with_timeout` in Actors

While less common for actors than `async::callback`, inheriting `with_timeout` can implement inactivity timeouts.

*   **How:** `class MyActor : public qb::Actor, public qb::io::async::with_timeout<MyActor> { ... };`
*   **Use Case:** Terminating an actor after a period with no incoming messages or specific activity.
*   **Implementation:** Call `updateTimeout()` in relevant `on(Event&)` handlers. Implement `on(event::timer&)` to handle the timeout (e.g., call `kill()`).

```cpp
#include <qb/actor.h>
#include <qb/io/async.h>

struct KeepAliveMsg : qb::Event {};

class InactivityTimeoutActor : public qb::Actor, public qb::io::async::with_timeout<InactivityTimeoutActor> {
public:
    // Set 60 second timeout in constructor
    InactivityTimeoutActor() : with_timeout(60.0) {}

    bool onInit() override {
        registerEvent<KeepAliveMsg>(*this);
        registerEvent<qb::KillEvent>(*this);
        updateTimeout(); // Start initial countdown
        return true;
    }

    void on(const KeepAliveMsg& event) {
        std::cout << "Actor " << id() << ": Activity detected, resetting timeout." << std::endl;
        updateTimeout(); // Reset inactivity timer
    }

    // Called by with_timeout if updateTimeout() isn't called within 60s
    void on(qb::io::async::event::timer const&) {
        std::cout << "Actor " << id() << ": Inactivity timeout! Terminating." << std::endl;
        kill();
    }

     void on(const qb::KillEvent&) { kill(); }
};
```
**(Ref:** `chat_tcp/server/ChatSession.h` (non-actor class example))**

## Asynchronous File Operations in Actors

Avoid blocking `sys::file` operations directly in handlers. Use async patterns:

### Pattern 1: Wrap Blocking Calls in `async::callback`

Suitable for simpler, less frequent operations.

*   **How:**
    1.  Capture necessary data (path, content to write/buffer to read, requestor `ActorId`).
    2.  Schedule `qb::io::async::callback`.
    3.  Inside the lambda:
        *   Perform the `qb::io::sys::file` operation (`open`, `read`/`write`, `close`).
        *   Construct a response event (`FileReadResult`, `FileWriteResult`) with success status and data/error message.
        *   Push the response event back to the original requestor. **This requires access to the VirtualCore handler, making Pattern 2 often cleaner.**
*   **Example (`file_processor/file_worker.h` - simplified concept):**
    ```cpp
    // Inside FileWorker actor
    void on(ReadFileRequest& request) {
        _is_busy = true;
        auto file_content = std::make_shared<std::vector<char>>();
        auto captured_request = request; // Capture necessary info

        qb::io::async::callback([this, captured_request, file_content]() {
            qb::io::sys::file file;
            bool success = false; std::string error_msg;
            // --- Perform blocking read --- 
            if (file.open(captured_request.filepath.c_str(), O_RDONLY) >= 0) {
               // ... read into file_content buffer ...
               success = true; // if read ok
               file.close();
            } else { error_msg = "Open failed"; }
            // --- End blocking read --- 

            // *** Sending response requires VirtualCore access ***
            // Need a clean way to get the handler for captured_request.requestor.index()
            // This example pushes directly, assuming access, but sending an internal
            // event back to FileWorker first is generally better practice.
             VirtualCore::_handler->push<ReadFileResponse>(
                captured_request.requestor, // Original requestor ID
                id(), // Source is this FileWorker
                captured_request.filepath.c_str(),
                file_content,
                success,
                error_msg.c_str(),
                captured_request.request_id
            );

            _is_busy = false;
            notifyAvailable(); // Send WorkerAvailable back to FileManager
        });
    }
    ```

### Pattern 2: Dedicated File Worker/Manager Actors (Recommended)

*   **How:** Create specialized actors (`FileReaderActor`, `FileWriterActor`, `FileManagerActor`).
*   **Workflow:** Client sends request event -> Manager -> Worker -> Worker uses `async::callback` for blocking IO -> Worker sends response event *directly* to original Client.
*   **Benefits:** Better structure, isolates blocking code, enables load balancing.
*   **(Ref:** `example/core_io/file_processor/`**)

### Pattern 3: File Watcher Actor

*   **How:** An actor uses `qb::io::async::directory_watcher` (often via a helper class like `DirectoryMonitor` in the example).
*   **Workflow:** Watcher actor receives `on(event::file&)` notifications -> Determines change type -> Pushes application-level `FileEvent` to subscribed processing actors.
*   **(Ref:** `example/core_io/file_monitor/`**) 