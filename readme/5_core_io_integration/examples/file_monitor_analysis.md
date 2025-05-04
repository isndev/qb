# Example Analysis: `file_monitor`

*   **Location:** `example/core_io/file_monitor/`
*   **Purpose:** Demonstrates a real-time file system monitoring system using actors and asynchronous file watching.

## Architecture Overview

This example separates the tasks of watching, processing, and simulating interaction into distinct actors:

1.  **`DirectoryWatcher` (Actor, `watcher.h/.cpp`, Core 0):
    *   **Role:** Monitors specified directories for changes.
    *   **Inheritance:** `qb::Actor`.
    *   **Internal Mechanism:** Manages instances of a helper class `DirectoryMonitor`. Each `DirectoryMonitor` uses `qb::io::async::directory_watcher<DirectoryMonitor>` (CRTP base class from `qb-io`) to interact with the OS's file monitoring API (inotify, kqueue, etc.) via the event loop.
    *   **Requests:** Handles `WatchDirectoryRequest` (to start monitoring a path, storing the requesting `ActorId` as a subscriber) and `UnwatchDirectoryRequest` (to stop monitoring).
    *   **Event Detection:** The `DirectoryMonitor::on(qb::io::async::event::file const&)` method is invoked by the `listener` when a change occurs. It analyzes the `event.attr` and `event.prev` stat data to determine the `FileEventType` (CREATED, MODIFIED, DELETED, ATTRIBUTES_CHANGED).
    *   **Notification:** Calls `publishFileEvent`, which identifies the relevant `WatchInfo` and `push`es an application-level `FileEvent` (containing path and type) to all subscribed actors (`WatchInfo::subscribers`).
    *   **Recursion:** Handles recursive watching by creating nested `WatchInfo` and `DirectoryMonitor` instances for subdirectories.
2.  **`FileProcessor` (Actor, `processor.h/.cpp`, Core 1):
    *   **Role:** Receives `FileEvent`s and performs actions based on file changes.
    *   **Inheritance:** `qb::Actor`.
    *   **State:** Maintains `_tracked_files`, a map storing `FileMetadata` (path, size, hash, mtime) for files it's aware of.
    *   **Event Handling:** `on(FileEvent&)` triggers processing methods (`processFileCreated`, `processFileModified`, `processFileDeleted`).
    *   **Processing Logic:** Updates internal state, potentially calculates file hashes (`extractMetadata` using `qb::io::sys::file`), and logs changes. Includes basic configuration (`SetProcessingConfigRequest`) and stats reporting (`GetProcessingStatsRequest`).
3.  **`ClientActor` (Actor, `main.cpp`, Core 0):
    *   **Role:** Simulates a client using the monitoring system and generates file system activity.
    *   **Inheritance:** `qb::Actor`.
    *   **Interaction:**
        *   Sends `WatchDirectoryRequest` to `DirectoryWatcher` in `onInit`.
        *   Receives `WatchDirectoryResponse` confirmation.
        *   Receives `FileEvent` notifications from `DirectoryWatcher`.
        *   Uses `qb::io::async::callback` to periodically call internal methods (`createTestFile`, `modifyTestFile`, `deleteTestFile`) that use `qb::io::sys::file` or `std::filesystem` to create/modify/delete files in the monitored directory, thus triggering the `DirectoryWatcher`.
        *   Manages the test duration and triggers shutdown via `broadcast<qb::KillEvent>()`.
        *   Sends `UnwatchDirectoryRequest` in its `on(KillEvent&)` handler.

## Key Concepts Illustrated

*   **Asynchronous File Watching:** Practical application of `qb::io::async::directory_watcher` integrated into an actor (`DirectoryWatcher`).
*   **Event-Driven Workflow:** File system events -> `qb::io::async::event::file` -> `DirectoryMonitor::on()` -> `DirectoryWatcher::publishFileEvent()` -> `FileEvent` -> `FileProcessor::on()$.
*   **Separation of Concerns:** Watching (OS interaction), Processing (state/logic), and Simulation (client interaction) are handled by different actors.
*   **Actor Communication:** Uses custom events (`WatchDirectoryRequest`, `FileEvent`, etc.) for clear communication between components.
*   **Asynchronous Task Execution within Actors:** `ClientActor` demonstrates using `async::callback` to perform potentially blocking file operations (or simply to schedule activity) without blocking its core.
*   **Resource Management:** `DirectoryWatcher` is responsible for starting and stopping the underlying `DirectoryMonitor` watchers. 