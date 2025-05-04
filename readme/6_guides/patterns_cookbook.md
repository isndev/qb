# QB Framework: Advanced Usage & Patterns Cookbook

This guide provides practical implementation examples for common patterns used in actor-based systems, drawing from the QB framework's examples and tests.

## Pattern: Finite State Machine (FSM)

Actors naturally model FSMs: actor state = FSM state, events = FSM inputs.

*   **How:**
    1.  Define states: `enum class MyState { STATE_A, STATE_B, ... };`
    2.  Store current state: `MyState _current_state = MyState::INITIAL;`
    3.  Define events corresponding to FSM inputs: `struct InputX : qb::Event {};`
    4.  In `on(InputX&)` handlers, use `switch (_current_state)` or `if` statements to check current state.
    5.  Perform actions based on state + event.
    6.  Update `_current_state` to transition.
    7.  Use `async::callback` for timed transitions or actions.

*   **Example (`example/core/example8_state_machine.cpp`):**
    ```cpp
    // Inside CoffeeMachineActor
    MachineState _current_state;

    void on(const ButtonPressed& event) {
        if (_current_state == MachineState::IDLE) {
            _selected_coffee = event.coffee_type;
            changeState(MachineState::SELECTING, "Selected coffee");
        } else if (_current_state == MachineState::SELECTING) {
            _selected_coffee = event.coffee_type;
            // Update display, stay in SELECTING
        }
        // ... other states ignore ButtonPressed ...
    }

    void startBrewingTimer() {
        auto self_id = id();
        qb::io::async::callback([self_id](){
             VirtualCore::_handler->push<BrewFinished>(self_id, self_id);
        }, 3.0); // 3 second brew time
    }

    void changeState(MachineState next_state, const std::string& reason) {
        std::cout << "State: " << stateToString(_current_state) << " -> " << stateToString(next_state) << " Reason: " << reason << std::endl;
        // Optional: Perform exit action for _current_state
        _current_state = next_state;
        // Optional: Perform entry action for next_state
    }
    ```

## Pattern: Publish/Subscribe

Decouple message senders (publishers) from receivers (subscribers) via a central broker.

*   **How:**
    1.  **Broker Actor (`TopicManagerActor`):**
        *   Maintains state: `map<string topic, set<ActorId> subscribers>`.
        *   Handles `Subscribe(topic)` event: Add `event.getSource()` to `subscribers[topic]`.
        *   Handles `Unsubscribe(topic)` event: Remove sender from `subscribers[topic]`.
        *   Handles `Publish(topic, data)` event: Iterate `subscribers[topic]` and `push<MessageDelivery(data)>` to each subscriber ID.
        *   Handles `DisconnectEvent`: Remove the disconnected actor ID from all subscriber sets.
    2.  **Publisher Actor:** Sends `Publish` event to Broker.
    3.  **Subscriber Actor:** Sends `Subscribe`/`Unsubscribe` to Broker; handles `MessageDelivery` event.

*   **Optimization (`message_broker` example):** Use `MessageContainer` with `shared_ptr` semantics in the Broker's `Publish` handler and `SendMessageEvent` to avoid copying the message payload for each subscriber.

**(Ref:** `example/core/example7_pub_sub.cpp`, `example/core_io/message_broker/`**)

## Pattern: Request/Response with Timeout

Manage asynchronous request-response interactions reliably.

*   **How:**
    1.  **Define Events:** `Request(req_id, ...)`, `Response(req_id, ...)`.
    2.  **Requester:**
        *   Generate unique `req_id`.
        *   Store context: `map<RequestId, Context> _pending_requests;`
        *   `push<Request>(target_id, req_id, ...);`
        *   Schedule timeout: `async::callback([self_id, req_id]{ handleTimeout(req_id); }, timeout_s);`
        *   `on(Response&)`: Find context using `req_id`, process, remove from `_pending_requests`. // *Need to potentially cancel timeout here if possible*
        *   `handleTimeout(req_id)`: Check if `req_id` still in `_pending_requests`. If yes, handle timeout (retry, error), remove entry.
    3.  **Responder:**
        *   `on(Request&)`: Process, then `push<Response>(event.getSource(), event.request_id, result);`
*   **Note:** Cancelling the `async::callback` timer is not directly supported easily. The timeout handler must check if the request is still pending.

**(Ref:** `example/core_io/file_processor/` (implicit timeout not shown but pattern fits), `example8_state_machine.cpp`**)

## Pattern: Shared Resource Manager

Safely manage access to resources not inherently thread-safe within the actor model.

*   **How:**
    1.  **Manager Actor:**
        *   Owns the resource (e.g., `DatabaseConnection _db;`, `std::fstream _file;`, `std::deque<WorkItem> _queue;`).
        *   Initializes resource in `onInit`.
        *   Handles request events (`DbQuery`, `WriteToFile`, `GetWorkItem`).
        *   Performs operations **sequentially** on the resource within its handlers.
        *   Sends response events back to requesters.
        *   Cleans up resource in destructor/`on(KillEvent&)`. // Corrected
    2.  **Client Actors:** Interact *only* by sending request events to the Manager Actor and handling response events.

*   **Example (`example6_shared_queue.cpp` - adapted):** The example uses an external `SharedQueue<T>` (which is thread-safe), but the *pattern* of actors interacting via a central point is similar. A pure actor version would encapsulate a non-thread-safe `std::deque` inside the `Supervisor` actor.
    ```cpp
    // Conceptual Pure Actor Queue Manager
    class QueueManagerActor : public qb::Actor {
    private:
        std::deque<WorkItem> _work_queue;
        std::set<qb::ActorId> _waiting_workers;
    public:
        bool onInit() override {
            registerEvent<AddItem>(*this);
            registerEvent<RequestItem>(*this);
            return true;
        }
        void on(const AddItem& event) {
            _work_queue.push_back(event.item);
            tryAssignWork();
        }
        void on(const RequestItem& event) {
            _waiting_workers.insert(event.getSource());
            tryAssignWork();
        }
        void tryAssignWork() {
            if (!_work_queue.empty() && !_waiting_workers.empty()) {
                qb::ActorId worker_id = *_waiting_workers.begin();
                _waiting_workers.erase(_waiting_workers.begin());
                WorkItem item = _work_queue.front();
                _work_queue.pop_front();
                push<AssignWork>(worker_id, item);
            }
        }
        // ... handle worker disconnects to remove from _waiting_workers ...
    };
    ```

**(Ref:** `example6_shared_queue.cpp`, `[Guides: Advanced Usage - Managing Shared Resources](./../6_guides/advanced_usage.md)`**) 