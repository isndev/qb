@page example_analysis_file_monitor_md Case Study: Building a File System Monitor with QB Actors
@brief A detailed walkthrough of the `file_monitor` example, showcasing asynchronous file/directory watching and event-driven processing in QB.

# Case Study: Building a File System Monitor with QB Actors

*   **Location:** `example/core_io/file_monitor/`
*   **Objective:** This example demonstrates how to build a real-time file system monitoring application using QB actors. It showcases how to leverage `qb-io`'s asynchronous file watching capabilities to detect changes (creations, modifications, deletions) in specified directories and process these events through a pipeline of actors.

By examining this example, you will learn about:
*   Integrating asynchronous file/directory watching (`qb::io::async::directory_watcher`) into an actor.
*   Designing an event-driven workflow triggered by file system events.
*   Separating concerns: watching, processing, and client interaction into distinct actors.
*   Using custom events for robust inter-actor communication.
*   Employing `qb::io::async::callback` for simulating external interactions or periodic tasks within an actor.

## Architectural Overview: A Reactive System

The `file_monitor` example is structured around three main actor types:

### 1. `DirectoryWatcher` Actor
*   **Headers:** `watcher.h`, `watcher.cpp`
*   **Core Assignment (Typical):** Core 0 (often suitable for I/O-bound tasks like file watching).
*   **Role:** The primary component responsible for monitoring specified directory paths for any changes to files or subdirectories.
*   **QB Integration & Internal Mechanism:**
    *   Inherits from `qb::Actor`.
    *   It internally manages one or more instances of a helper class, `DirectoryMonitor` (also defined in `watcher.h/.cpp`).
    *   Each `DirectoryMonitor` instance inherits from `qb::io::async::directory_watcher<DirectoryMonitor>`, which is a CRTP base class from `qb-io`. This base class uses `libev`'s `ev_stat` watchers to poll for attribute changes on the specified path.
*   **Request Handling:**
    *   `on(WatchDirectoryRequest&)`: When this event is received (typically from `ClientActor`), the `DirectoryWatcher` initiates monitoring for the requested path. It stores the `requestor`'s `ActorId` as a subscriber for events from this path. If recursive watching is requested, it will also set up watchers for subdirectories.
    *   `on(UnwatchDirectoryRequest&)`: Stops monitoring a previously watched path for a given subscriber.
*   **Event Detection & Propagation:**
    *   The `DirectoryMonitor::on(qb::io::async::event::file const& os_event)` method is invoked by the `qb-io` event loop (`listener::current`) when the OS (via `ev_stat`) detects a change in a monitored file or directory's attributes (e.g., size, modification time, link count).
    *   This handler analyzes `os_event.attr` (current stat data) and `os_event.prev` (previous stat data) to determine the nature of the change, categorizing it into a `FileEventType` (e.g., `CREATED`, `MODIFIED`, `DELETED`, `ATTRIBUTES_CHANGED`).
    *   The `DirectoryWatcher` then calls its `publishFileEvent` method. This method identifies all actors subscribed to the affected path and `push`es an application-level `FileEvent` (containing the path and `FileEventType`) to each of them.
*   **Recursion & State:** Manages a tree-like structure (`WatchInfo`) to handle recursive directory watching and track subscribers per path.

### 2. `FileProcessor` Actor
*   **Headers:** `processor.h`, `processor.cpp`
*   **Core Assignment (Typical):** Core 1 (separating processing logic from I/O watching).
*   **Role:** Receives `FileEvent` notifications from the `DirectoryWatcher` and performs application-specific actions based on these events.
*   **QB Integration:** Standard `qb::Actor`.
*   **State Management:** Maintains a `_tracked_files` map (`std::map<std::string, FileMetadata>`) to store information about files it's aware of, such as path, size, content hash, and last modification time.
*   **Event Handling (`on(FileEvent&)`):
    *   This is the primary entry point for this actor.
    *   Based on `event.event_type`, it dispatches to internal methods like `processFileCreated`, `processFileModified`, or `processFileDeleted`.
*   **Processing Logic:**
    *   `processFileCreated`: Adds new file metadata to `_tracked_files`, potentially calculates an initial hash.
    *   `processFileModified`: Updates metadata for an existing file, recalculates hash if content likely changed (e.g., size or mtime differs).
    *   `processFileDeleted`: Removes the file from `_tracked_files`.
    *   `extractMetadata()`: A helper that uses `qb::io::sys::file` for synchronous read operations to get file size and potentially `qb::crypto::md5` (or another hash) for content hashing. *Note: If hashing large files, this synchronous read could block. In a production system, this might be offloaded to an `async::callback` or a dedicated hashing worker actor if performance is critical.*
    *   Logs detected changes to the console.
*   **Configuration & Stats:** Also handles `SetProcessingConfigRequest` (e.g., to toggle processing of hidden files) and `GetProcessingStatsRequest` (replying with `MonitoringStats`).

### 3. `ClientActor` (Simulation & Interaction)
*   **Header:** `main.cpp` (defined within)
*   **Core Assignment (Typical):** Core 0 (can co-exist with `DirectoryWatcher` for this example).
*   **Role:** Simulates a client application that uses the file monitoring service. It also actively creates, modifies, and deletes files in a test directory to generate file system events for the `DirectoryWatcher` to pick up.
*   **QB Integration:** Standard `qb::Actor`.
*   **Interaction Flow:**
    1.  **`onInit()`:** Sends a `WatchDirectoryRequest` to the `DirectoryWatcher` to start monitoring a specified `_test_directory`.
    2.  `on(WatchDirectoryResponse&)`: Receives confirmation that watching has started. It then begins its file manipulation tests.
    3.  **File Operations (via `qb::io::async::callback`):** Uses `qb::io::async::callback` to schedule a sequence of file operations (`createTestFile`, `modifyTestFile`, `deleteTestFile`). These methods use `std::filesystem` (or `qb::io::sys::file` for content) to perform actual file system modifications. Using `async::callback` ensures these potentially blocking OS calls don't stall the `ClientActor`'s `VirtualCore`.
    4.  `on(FileEvent&)`: Receives `FileEvent` notifications from the `DirectoryWatcher` in response to its own file operations (and any other changes) and logs them.
    5.  **Shutdown:** Manages a test duration. After the duration, it `broadcast<qb::KillEvent>()` to stop all actors and sends an `UnwatchDirectoryRequest` in its own `on(qb::KillEvent&)` handler.

## Key QB Concepts Illustrated

*   **Asynchronous File System Watching:** Demonstrates the practical application of `qb::io::async::directory_watcher` (via the `DirectoryMonitor` helper) integrated within an actor (`DirectoryWatcher`) to react to OS-level file system notifications without blocking.
*   **Event-Driven Pipeline:** Shows a clear flow: OS file system event -> `qb::io::async::event::file` -> `DirectoryMonitor` -> `DirectoryWatcher` (transforms to `FileEvent`) -> `FileProcessor`.
*   **Separation of Concerns:** Responsibilities are well-defined:
    *   `DirectoryWatcher`: Low-level OS interaction and event detection/filtering.
    *   `FileProcessor`: Application-specific logic based on file events.
    *   `ClientActor`: User interaction, simulation, and system orchestration for the demo.
*   **Custom Event Types:** Effective use of application-specific events (`WatchDirectoryRequest`, `FileEvent`, `SetProcessingConfigRequest`, etc.) for clear and type-safe communication between actors.
*   **Asynchronous Task Execution in Actors:** The `ClientActor` uses `qb::io::async::callback` to perform potentially blocking file creation/modification/deletion operations off its main event processing path, maintaining responsiveness.
*   **Resource Management (Watchers):** The `DirectoryWatcher` is responsible for creating (`startWatching`) and cleaning up (`stopWatching`) the underlying `DirectoryMonitor` instances, which in turn manage the `ev_stat` watchers.

This example provides a solid blueprint for building applications that need to react to file system changes in a robust and asynchronous manner using the QB Actor Framework.

**(Next Example Analysis:** `[file_processor Example Analysis](./file_processor_analysis.md)`**) 