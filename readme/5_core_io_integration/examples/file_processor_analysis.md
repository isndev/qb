@page example_analysis_file_processor_md Case Study: Asynchronous File Processing with Manager-Worker Actors
@brief Analyze the `file_processor` example to learn how QB actors can manage and distribute blocking file I/O tasks asynchronously.

# Case Study: Asynchronous File Processing with Manager-Worker Actors

*   **Location:** `example/core_io/file_processor/`
*   **Objective:** This example demonstrates a robust Manager-Worker pattern for handling potentially blocking file I/O operations (reads and writes) in an asynchronous manner within the QB Actor Framework. It showcases how to prevent `VirtualCore`s from stalling by offloading blocking tasks to dedicated worker actors.

By studying this example, you will understand:
*   How to structure a Manager-Worker pattern using actors.
*   The technique of wrapping blocking system calls (like file operations) within `qb::io::async::callback` to maintain system responsiveness.
*   Managing a pool of worker actors and distributing tasks based on availability.
*   A request/response flow where workers can reply directly to the original client actor.

## Architectural Overview: Distributing File I/O

The `file_processor` example uses three main actor types to achieve its goal:

### 1. `FileManager` Actor (`file_manager.h`)
*   **Core Assignment (Typical):** Core 0 (acts as a central dispatcher).
*   **Role:** The central coordinator and dispatcher. It receives file operation requests from `ClientActor`(s), maintains a queue of pending tasks, and assigns these tasks to available `FileWorker` actors.
*   **QB Integration:** Standard `qb::Actor`.
*   **State Management:**
    *   `_available_workers`: An `std::unordered_set<qb::ActorId>` storing the IDs of `FileWorker` actors that are currently idle and ready for tasks.
    *   `_read_requests`, `_write_requests`: `std::queue`s holding pending `ReadFileRequest` and `WriteFileRequest` events respectively, when no workers are immediately available.
*   **Key Event Handling & Logic:**
    *   `on(ReadFileRequest&)` and `on(WriteFileRequest&)`:
        *   Receives a request. Assigns a unique `request_id` to track it (though in this example, the original request event is forwarded, already containing necessary IDs).
        *   Checks `_available_workers`. If a worker is free:
            *   It removes the worker's ID from `_available_workers`.
            *   It `push`es the original request event (which includes the `requestor` ID) directly to the chosen `FileWorker`.
            ```cpp
            // Simplified from FileManager::on(ReadFileRequest& request)
            if (!_available_workers.empty()) {
                qb::ActorId worker_id = *_available_workers.begin();
                _available_workers.erase(_available_workers.begin());
                qb::io::cout() << "FileManager: Assigning Read Request for \"" 
                               << request.filepath.c_str() << "\" to Worker " << worker_id << ".\n";
                push<ReadFileRequest>(worker_id, request.filepath.c_str(), request.requestor, request.request_id);
            } else {
                _read_requests.push(request); // Forwarding the whole event implies copying or careful event design
                qb::io::cout() << "FileManager: No worker available for Read Request. Queued.\n";
            }
            ```
        *   If no worker is available, the request is enqueued in `_read_requests` or `_write_requests` (with reads typically prioritized).
    *   `on(WorkerAvailable&)`:
        *   Called by a `FileWorker` when it finishes a task.
        *   Adds the `worker_id` from the event back to `_available_workers`.
        *   Immediately checks the task queues (reads first, then writes). If a task is pending, it dequeues it and assigns it to the now-available worker (similar logic to handling a new request).
    *   `on(ReadFileResponse&)` and `on(WriteFileResponse&)`:
        *   Receives the response event from a `FileWorker`.
        *   Crucially, it forwards the *entire response event* (which contains the result, original `request_id`, and the original `requestor` ID) to the `requestor` actor that initiated the operation.
        ```cpp
        // Inside FileManager::on(ReadFileResponse& response)
        qb::io::cout() << "FileManager: Forwarding ReadFileResponse for \"" 
                       << response.filepath.c_str() << "\" to original requestor " 
                       << response.requestor << ".\n";
        push<ReadFileResponse>(response.requestor, response.filepath.c_str(), response.data, 
                               response.success, response.error_msg.c_str(), response.request_id);
        ```

### 2. `FileWorker` Actor (`file_worker.h`)
*   **Core Assignment (Typical):** Cores 1+ (can have multiple instances distributed across cores for parallelism).
*   **Role:** Executes a single file read or write operation. Designed to handle potentially blocking I/O without stalling its `VirtualCore`.
*   **QB Integration:** Standard `qb::Actor`.
*   **Initialization (`onInit()`):
    *   Stores its manager's ID (`_manager_id`).
    *   Sends an initial `WorkerAvailable` message to the `FileManager` to signal its readiness.
*   **Key Event Handling & Logic (`on(ReadFileRequest&)` and `on(WriteFileRequest&)`):
    1.  Receives a task request from `FileManager`.
    2.  Sets an internal `_is_busy = true;` flag (though not explicitly used to prevent new tasks in this example, as `FileManager` manages availability).
    3.  **Asynchronous I/O Offload:** This is the core of its non-blocking nature. It captures the necessary request details (filepath, data for write, original requestor ID, request ID) into a lambda and schedules this lambda for execution using `qb::io::async::callback([this, captured_request, ...]() { ... });`.
        ```cpp
        // Simplified from FileWorker::on(ReadFileRequest& request)
        auto captured_request = request; // Capture needed data
        auto file_content = std::make_shared<std::vector<char>>(); // Buffer for read

        qb::io::async::callback([this, captured_request, file_content]() {
            if (!this->is_alive()) return; // Actor might have been killed

            qb::io::sys::file file_op;
            bool success = false;
            qb::string<256> error_message;
            // --- PERFORM BLOCKING FILE I/O --- 
            if (file_op.open(captured_request.filepath.c_str(), O_RDONLY) >= 0) {
                // ... read file into file_content using file_op.read() ...
                // ... handle potential read errors ...
                success = true; 
                file_op.close();
            } else { error_message = "Failed to open file for reading"; }
            // --- END OF BLOCKING I/O ---

            // Send response DIRECTLY to the original client actor
            this->push<ReadFileResponse>(captured_request.requestor, 
                                       captured_request.filepath.c_str(), 
                                       file_content, 
                                       success, 
                                       error_message.c_str(), 
                                       captured_request.request_id);
            notifyAvailable(); // Tell FileManager this worker is free
        });
        ```
    4.  **Inside the `async::callback` Lambda:**
        *   Performs the actual, potentially blocking, file operation using `qb::io::sys::file` (`open`, `read`/`write`, `close`).
        *   Constructs a response event (`ReadFileResponse` or `WriteFileResponse`) containing the result (data read, bytes written, success status, error message), the original `request_id`, and importantly, the `requestor` ID from the captured request.
        *   **Direct Reply to Client:** `push`es the response event *directly* to the original `requestor` (`captured_request.requestor`). This bypasses the `FileManager` for the response path, reducing load on the manager.
        *   Calls `notifyAvailable()`, which `push`es a `WorkerAvailable` event (with its own `id()`) back to the `FileManager`.

### 3. `ClientActor` (`main.cpp`)
*   **Core Assignment (Typical):** Core 0 (can co-exist with `FileManager`).
*   **Role:** Simulates one or more clients that make requests for file operations.
*   **QB Integration:** Standard `qb::Actor`.
*   **Interaction Flow:**
    1.  In `onInit()` or other handlers, sends `ReadFileRequest` or `WriteFileRequest` events to the `FileManager`. It includes its own `id()` as the `requestor` field in these requests.
    2.  Receives `ReadFileResponse` or `WriteFileResponse` events *directly from the `FileWorker`* that processed its specific request.
    3.  Processes the response (e.g., prints content to console, checks status).
    4.  Tracks pending requests and can initiate a system shutdown when all its simulated operations are complete.

## Key QB Concepts Illustrated

*   **Manager-Worker Pattern:** A common and effective pattern for distributing tasks and managing a pool of worker components.
*   **Asynchronicity for Blocking Operations:** Demonstrates the crucial technique of wrapping potentially blocking system calls (like synchronous file I/O) inside `qb::io::async::callback`. This prevents the `FileWorker` actor from stalling its `VirtualCore`, maintaining overall system responsiveness.
*   **Task Queuing & Basic Load Balancing:** The `FileManager` acts as a simple load balancer by assigning tasks to the next free worker or by queueing requests if all workers are busy.
*   **Decoupled Request/Response Flow (Direct Reply):** The pattern where a client sends a request to a manager, the manager delegates to a worker, and the worker sends the response *directly* back to the original client is a powerful way to reduce load on central manager actors and improve response latency. The original `requestor` ID is passed along through the events to enable this.
*   **State Management for Workers:** `FileWorker`s manage their busy/available state implicitly by only notifying the `FileManager` when they complete a task.
*   **Multi-Core Parallelism:** Multiple `FileWorker` actors can be instantiated on different `VirtualCore`s, allowing for concurrent file I/O operations (though actual parallelism will be limited by the underlying disk subsystem's performance).
*   **Custom Event Design:** Shows practical examples of events for requests (`ReadFileRequest`), responses (`ReadFileResponse`), and internal coordination (`WorkerAvailable`).

This example provides a clear blueprint for how to integrate blocking tasks into a QB actor system without sacrificing the benefits of asynchronous processing.

**(Next Example Analysis:** `[message_broker Example Analysis](./message_broker_analysis.md)`**) 