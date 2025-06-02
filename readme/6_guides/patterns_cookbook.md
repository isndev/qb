@page guides_patterns_cookbook_md QB Actor Framework: Design Patterns Cookbook
@brief Practical recipes and C++ examples for implementing common and effective actor-based design patterns with the QB Framework.

# QB Actor Framework: Design Patterns Cookbook

This cookbook provides practical recipes and conceptual C++ examples for implementing common design patterns in your QB actor-based applications. These patterns help structure your system, manage state, and handle interactions effectively. For information about Service Actors and Periodic Callbacks, please refer to the [QB-Core: Common Actor Patterns & Utilities](./../4_qb_core/patterns.md) page.

## 1. Finite State Machine (FSM)

Actors naturally model entities with distinct states and transitions triggered by events. The [QB-Core: Common Actor Patterns & Utilities](./../4_qb_core/patterns.md#1-finite-state-machine-fsm-with-actors) page provides a detailed explanation and example of implementing FSMs.

**Key Idea:** An actor's member variables hold its current state. Event handlers (`on(Event&)` methods) implement state transition logic based on the current state and the received event.

## 2. Publish/Subscribe (Pub/Sub)

This pattern decouples message senders (publishers) from receivers (subscribers) using a central Broker actor that manages topics and message distribution.

*   **Purpose:** Allows multiple actors to subscribe to specific topics of interest and receive relevant messages without publishers needing to know about individual subscribers.
*   **Components:**
    *   **Broker Actor:** Manages topic subscriptions and routes published messages.
    *   **Publisher Actor(s):** Send messages for a specific topic to the Broker.
    *   **Subscriber Actor(s):** Subscribe to topics with the Broker and receive messages for those topics.

*   **Conceptual Flow:**
    ```text
    +-------------+      +-----------------+      +-------------+
    | Publisher A |----->| PublishReq(T1)  |----->| BrokerActor |
    +-------------+      +-----------------+      +-------------+
          ^                                             |
          | 2. Broker sends                             | 1. Publisher sends message for Topic T1
          |    NotificationMsg(T1)                      | Manages subscriptions for T1, T2...
          |    to relevant subscribers                  |
    +-----|-------+                               +-----|-------+
    | SubscriberX |                               | SubscriberY |
    | (Subscribed |                               | (Subscribed |
    |  to T1)     |                               |  to T1)     |
    +-------------+                               +-------------+

    +-------------+
    | SubscriberZ |
    | (Subscribed |
    |  to T2)     |  <-- No message, not subscribed to T1
    +-------------+
    ```

*   **Conceptual Implementation:**

    **Events:**
    ```cpp
    #include <qb/actor.h>
    #include <qb/event.h>
    #include <qb/string.h>
    #include <string> // For std::string in map keys if needed, or use qb::string
    #include <vector>
    #include <set>
    #include <map>
    #include <memory> // For std::shared_ptr

    // Using qb::string for topic for efficiency and ABI safety in events
    struct SubscribeReq : qb::Event { qb::string<64> topic; };
    struct UnsubscribeReq : qb::Event { qb::string<64> topic; };
    struct PublishReq : qb::Event {
        qb::string<64> topic;
        std::shared_ptr<qb::string<256>> content; // Use shared_ptr for potentially large content
        PublishReq(const char* t, std::shared_ptr<qb::string<256>> c) : topic(t), content(c) {}
    };
    struct NotificationMsg : qb::Event {
        qb::string<64> topic;
        std::shared_ptr<qb::string<256>> content; // Share content efficiently
        NotificationMsg(qb::string<64> t, std::shared_ptr<qb::string<256>> c) : topic(std::move(t)), content(c) {}
    };
    ```

    **Broker Actor:**
    ```cpp
    class BrokerActor : public qb::Actor {
    private:
        // Using std::string as map key here for simplicity in example; 
        // consider qb::string or custom hash for qb::string if performance critical.
        std::map<std::string, std::set<qb::ActorId>> _subscriptions;

    public:
        bool onInit() override {
            registerEvent<SubscribeReq>(*this);
            registerEvent<UnsubscribeReq>(*this);
            registerEvent<PublishReq>(*this);
            // ... KillEvent ...
            return true;
        }

        void on(const SubscribeReq& event) {
            std::string topic_str = event.topic.c_str(); // Convert for map key if needed
            _subscriptions[topic_str].insert(event.getSource());
            qb::io::cout() << "Broker: Actor " << event.getSource() << " subscribed to " << topic_str << ".\n";
        }

        void on(const UnsubscribeReq& event) {
            std::string topic_str = event.topic.c_str();
            if (_subscriptions.count(topic_str)) {
                _subscriptions[topic_str].erase(event.getSource());
                qb::io::cout() << "Broker: Actor " << event.getSource() << " unsubscribed from " << topic_str << ".\n";
            }
        }

        void on(const PublishReq& event) {
            std::string topic_str = event.topic.c_str();
            qb::io::cout() << "Broker: Publishing to topic '" << topic_str << "': " << event.content->c_str() << ".\n";
            if (_subscriptions.count(topic_str)) {
                for (qb::ActorId subscriber_id : _subscriptions[topic_str]) {
                    // Create NotificationMsg once, pass shared_ptr to content
                    push<NotificationMsg>(subscriber_id, event.topic, event.content);
                }
            }
        }
        // ... KillEvent handler ...
    };
    ```

    **Subscriber Actor:**
    ```cpp
    class SubscriberActor : public qb::Actor {
    private:
        qb::ActorId _broker_id;
    public:
        SubscriberActor(qb::ActorId broker) : _broker_id(broker) {}
        bool onInit() override {
            registerEvent<NotificationMsg>(*this);
            // ... KillEvent ...
            // Subscribe to a topic
            push<SubscribeReq>(_broker_id, "news/local");
            return true;
        }
        void on(const NotificationMsg& event) {
            qb::io::cout() << "Subscriber [" << id() << "] on topic '" << event.topic.c_str() 
                           << "' received: " << event.content->c_str() << ".\n";
        }
        // ... KillEvent handler could send UnsubscribeReq ...
    };
    ```
*   **(Fuller Examples:** `example/core/example7_pub_sub.cpp`, `example/core_io/message_broker/` [which uses `MessageContainer` for optimized shared payload delivery].**)

## 3. Request/Response with Timeout

Actors often need to request information or an action from another actor and await a response, potentially with a timeout if the response doesn't arrive promptly.

*   **Purpose:** Manage asynchronous request-response interactions reliably.
*   **Conceptual Flow:**
    ```text
    +-----------------+      +-----------------+      +-----------------+
    | Requester Actor |-(1)->| MyRequestEvent  |-(2)->| Responder Actor |
    | (ReqID: 123)    |      | (ReqID: 123,    |      | (Processes Req) |
    +-----------------+      |  Data)          |      +-----------------+
          |   ^ (4b. Response arrives)                    | (3. Sends Response)
          |   |                                           |
          | (4a. Timeout event arrives first)             |
          |   | MyResponseEvent (ReqID: 123, Result)      |
          |   +-------------------------------------------+
          |
          | (1b. Schedules self-sent RequestTimeoutEvent(ReqID:123)
          |      via qb::io::async::callback)
          |
          +-----> Handles MyResponseEvent OR RequestTimeoutEvent for ReqID 123
    ```
*   **Mechanism:**
    1.  **Define Events:**
        *   `MyRequestEvent { qb::ActorId original_requester; uint32_t request_id; /* request data */ }`
        *   `MyResponseEvent { uint32_t request_id; /* response data or error */ }`
        *   `RequestTimeoutEvent { uint32_t request_id; }` (internal to requester)
    2.  **Requester Actor:**
        *   Generates a unique `request_id` (e.g., an atomic counter or member counter).
        *   Stores context: `std::map<uint32_t, RequestContext> _pending_requests;` (Store timestamp, retry count etc. in `RequestContext`).
        *   Sends request: `push<MyRequestEvent>(target_id, id(), current_request_id, request_payload);`
        *   Schedules timeout: Uses `qb::io::async::callback` to send a `RequestTimeoutEvent` to itself after the desired timeout duration.
            ```cpp
            // Inside RequesterActor, after sending MyRequestEvent
            uint32_t captured_req_id = current_request_id;
            double timeout_duration_s = 5.0;
            qb::io::async::callback([this, captured_req_id]() {
                if (this->is_alive()) { // Crucial check
                    this->push<RequestTimeoutEvent>(this->id(), captured_req_id);
                }
            }, timeout_duration_s);
            ```
        *   Handles `MyResponseEvent`: Looks up `response.request_id` in `_pending_requests`. If found, processes response and removes the entry. *Consider how to cancel/ignore the pending timeout callback if possible, though direct cancellation isn't simple; the timeout handler must re-check.*.
        *   Handles `RequestTimeoutEvent`: Checks if `event.request_id` is still in `_pending_requests`. If yes, the original request timed out. Handles the timeout (e.g., retry, log error, notify user) and removes the pending entry.
    3.  **Responder Actor:**
        *   Handles `MyRequestEvent`: Performs the requested action.
        *   Sends response: `push<MyResponseEvent>(request_event.original_requester, request_event.request_id, response_payload);`
        *   (More efficient) Or, if `MyRequestEvent` was received by non-const reference and can hold the response: `request_event.result_field = result_data; reply(request_event);`

*   **(Reference Examples:** This pattern is foundational to many interactions. `example/core_io/file_processor/` demonstrates request/response where the worker responds directly to the client. The timeout aspect would be an addition to such patterns.)**

## 4. Shared Resource Manager

Safely manage access to resources that are not inherently thread-safe (e.g., a database connection, a file being written sequentially) by encapsulating the resource within a dedicated Manager Actor.

*   **Purpose:** Serialize access to a shared resource, preventing data races and ensuring consistent state, without explicit locking by client actors.
*   **Conceptual Flow:**
    ```text
    +---------------+            +-----------------+            +--------------------+
    | ClientActor A |----(1)---->| ResourceRequestA|----(2)---->|                    |
    +---------------+            +-----------------+            |  ResourceManager   |
                                                                  |  (Owns & Serializes|
    +---------------+            +-----------------+            |  Access to DBConn, |
    | ClientActor B |----(1)---->| ResourceRequestB|----(2)---->|  File, etc.)       |
    +---------------+            +-----------------+            |                    |
                                                                  +---------|----------+
                                                                            | (3) Processes sequentially
                                                                            | (4) Sends Response
                                                                  +---------v----------+
                                                                  | ResourceResponse   |
                                                                  | (to A or B via     |
                                                                  |  event.getSource())|
                                                                  +--------------------+
    ```
*   **Components:**
    1.  **Manager Actor:**
        *   **Owns the Resource:** Holds the resource as a private member (e.g., `std::unique_ptr<DatabaseConnection> _db_conn;`, `std::fstream _file_stream;`).
        *   **Initializes/Releases Resource:** Acquires the resource in its constructor or `onInit()`. Releases it in its `on(KillEvent&)` handler or destructor (RAII is best).
        *   **Defines Request/Response Events:** E.g., `DBQueryRequest`, `DBQueryResult`, `WriteLogRequest`, `WriteLogResponse`.
        *   **Handles Requests Sequentially:** Processes incoming request events one by one. Within each handler, it interacts with the managed resource.
        *   **Sends Responses:** After processing, sends a response event back to the original requester.
    2.  **Client Actors:**
        *   Do **not** access the shared resource directly.
        *   Interact with the resource *only* by sending request events to the Manager Actor.
        *   Handle response events from the Manager Actor.

*   **Conceptual Example (Simplified Logger Manager):**
    ```cpp
    #include <fstream> // For std::ofstream
    // Events
    struct LogLineEvent : qb::Event { qb::string<256> line_to_log; };
    // No response needed for this simple logger

    class FileLoggerManager : public qb::Actor {
    private:
        std::ofstream _log_file;
        qb::string<128> _file_path;
    public:
        FileLoggerManager(const char* file_path) : _file_path(file_path) {}

        bool onInit() override {
            _log_file.open(_file_path.c_str(), std::ios::app);
            if (!_log_file.is_open()) {
                qb::io::cout() << "Logger: Failed to open " << _file_path.c_str() << "!\n";
                return false; // Fail actor initialization
            }
            registerEvent<LogLineEvent>(*this);
            registerEvent<qb::KillEvent>(*this);
            qb::io::cout() << "Logger [" << id() << "] started for file: " << _file_path.c_str() << ".\n";
            return true;
        }

        void on(const LogLineEvent& event) {
            if (_log_file.is_open()) {
                _log_file << event.line_to_log.c_str() << std::endl; // endl flushes by default
            }
        }

        void on(const qb::KillEvent& /*event*/) {
            qb::io::cout() << "Logger [" << id() << "] shutting down. Closing file.\n";
            if (_log_file.is_open()) {
                _log_file.close();
            }
            kill();
        }
        // Destructor will also be called, but explicit close in KillEvent is good for clarity
        ~FileLoggerManager() override {
             if (_log_file.is_open()) { _log_file.close(); }
        }
    };
    ```
*   **(Reference Example:** `example6_shared_queue.cpp` features a `Supervisor` actor managing access to a `SharedQueue`. While the queue in that example *is* thread-safe, the pattern illustrates actors interacting with a central component to access a shared facility. A pure actor version would make the queue itself a private member of the Supervisor and not thread-safe.)**

These patterns offer robust solutions for structuring complex actor interactions. By combining them and adapting them to your specific needs, you can build sophisticated, scalable, and maintainable concurrent applications with the QB Actor Framework.

**(Next:** Explore [QB Framework: Advanced Techniques & System Design](./advanced_usage.md) for more in-depth techniques, or [QB Framework: Performance Tuning Guide](./performance_tuning.md) for optimization strategies.**) 