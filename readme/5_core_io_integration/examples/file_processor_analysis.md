# Example Analysis: `file_processor`

*   **Location:** `example/core_io/file_processor/`
*   **Purpose:** Demonstrates a pattern for distributing potentially blocking file I/O operations across multiple worker actors, managed by a central dispatcher.

## Architecture Overview

This example showcases a Manager-Worker pattern for handling asynchronous file operations:

1.  **`FileManager` (Actor, `file_manager.h`, Core 0):
    *   **Role:** Central dispatcher. Receives client requests, manages worker availability, queues tasks, and forwards responses.
    *   **Inheritance:** `qb::Actor`.
    *   **State:**
        *   `_available_workers`: `std::unordered_set<qb::ActorId>` storing IDs of idle workers.
        *   `_read_requests`, `_write_requests`: `std::queue`s for pending file operations.
    *   **Event Handling:**
        *   `on(ReadFileRequest&)`, `on(WriteFileRequest&)`: Receives requests from clients. Assigns a `request_id`. If a worker is in `_available_workers`, removes it and forwards the request event (including original `requestor` ID) to the worker. Otherwise, pushes the request onto the appropriate queue (`_read_requests` prioritized).
        *   `on(WorkerAvailable&)`: Adds the `worker_id` back to `_available_workers`. Checks queues (reads first, then writes) and immediately assigns a pending task to this worker if any exist.
        *   `on(ReadFileResponse&)`, `on(WriteFileResponse&)`: Receives results from `FileWorker`. Forwards the *entire* response event (including data/status) to the original `requestor` ID stored within the response event.
2.  **`FileWorker` (Actor, `file_worker.h`, Cores 1+):
    *   **Role:** Executes a single file read or write operation asynchronously.
    *   **Inheritance:** `qb::Actor`.
    *   **Initialization:** In `onInit`, sends `WorkerAvailable` to its `_manager_id`.
    *   **Event Handling:**
        *   `on(ReadFileRequest&)`, `on(WriteFileRequest&)`: Receives a task from `FileManager`. Sets `_is_busy = true`.
        *   **Asynchronous I/O:** Schedules a `qb::io::async::callback` to perform the actual file operation.
        *   **Inside the Callback:**
            *   Uses `qb::io::sys::file` to perform the blocking `open`, `read`/`write`, `close` operations.
            *   Constructs a `ReadFileResponse` or `WriteFileResponse` event containing the result (data/bytes written, success status, error message), the original `request_id`, and the path.
            *   **Crucially, `push`es the response event *directly* back to the original `requestor` (`captured_request.requestor`)**, bypassing the `FileManager` for the response path.
            *   Sets `_is_busy = false`.
            *   Calls `notifyAvailable()` which `push`es `WorkerAvailable` to the `FileManager`.
3.  **`ClientActor` (Actor, `main.cpp`, Core 0):
    *   **Role:** Simulates clients making file I/O requests.
    *   **Inheritance:** `qb::Actor`.
    *   **Interaction:**
        *   Sends `ReadFileRequest`/`WriteFileRequest` to `FileManager`, including its own `id()` as the `requestor`.
        *   Receives `ReadFileResponse`/`WriteFileResponse` directly from the `FileWorker` that processed the request.
        *   Processes the response (e.g., prints content/status).
        *   Tracks pending requests and initiates shutdown when all responses are received.

## Key Concepts Illustrated

*   **Manager-Worker Pattern:** A common pattern for distributing tasks.
*   **Asynchronicity for Blocking Operations:** Demonstrates wrapping potentially blocking system calls (`sys::file::read/write`) inside `async::callback` within an actor (`FileWorker`) to prevent stalling the `VirtualCore`.
*   **Load Balancing (Queue-Based):** `FileManager` acts as a simple load balancer by assigning tasks to the next available worker or queueing them.
*   **Request/Response with Forwarding:** The client sends a request to the manager, the manager forwards it to a worker, the worker performs the task and sends the response *directly* back to the client.
The `requestor` ID is passed along through the events.
*   **State Management:** `FileManager` manages worker availability and task queues. `FileWorker` manages its busy state.
*   **Multi-Core Parallelism:** Multiple `FileWorker` actors on different cores can perform file I/O concurrently (limited by disk subsystem performance). 