# Example Analysis: `distributed_computing`

*   **Location:** `example/core/example10_distributed_computing.cpp`
*   **Purpose:** Simulates a distributed computing system demonstrating task generation, scheduling with load balancing, parallel execution by workers, result collection, and system monitoring.

## Architecture Overview

This example represents a more complex actor system with distinct roles:

1.  **`TaskGeneratorActor` (`qb::Actor`):
    *   **Role:** Periodically creates `Task` objects.
    *   **Functionality:** Uses `async::callback` with a delay calculated from `TASKS_PER_SECOND` to call `generateTask()`. `generateTask()` creates a `Task` with random type, priority, complexity, and data, then `push`es a `TaskMessage` to the `TaskSchedulerActor`.
2.  **`TaskSchedulerActor` (`qb::Actor`):
    *   **Role:** Central dispatcher and load balancer.
    *   **State:** Manages `_worker_ids`, `_worker_metrics` (map `ActorId` -> `WorkerMetrics`), `_task_queue` (deque of `shared_ptr<Task>`), `_active_tasks` (map `task_id` -> `shared_ptr<Task>`).
    *   **Event Handling:**
        *   `on(TaskMessage&)`: Adds incoming task to `_task_queue`, attempts `scheduleTasks()`.
        *   `on(WorkerHeartbeatMessage&)`: Updates `_worker_metrics[worker_id].last_heartbeat`.
        *   `on(WorkerStatusMessage&)`: Updates `_worker_metrics[worker_id]` with latest stats (utilization etc.).
        *   `on(ResultMessage&)`: Removes completed task from `_active_tasks`, attempts `scheduleTasks()`.
        *   `on(UpdateWorkersMessage&)`: Receives the initial list of worker IDs from `SystemMonitorActor`.
    *   **Scheduling (`scheduleTasks`):** Sorts `_task_queue` by priority. Iterates through `_worker_ids`, checks `isWorkerAvailable()`. If a worker is available, dequeues the highest priority task, adds it to `_active_tasks`, and `push`es `TaskAssignmentMessage` to the worker.
    *   **Availability Check (`isWorkerAvailable`):** Checks if metrics exist, if the last heartbeat is recent (within `HEARTBEAT_TIMEOUT`), and if `utilization` is below a threshold (e.g., 80%).
    *   **Load Assessment (`assessLoadBalance`):** Periodically scheduled via `async::callback`. Calculates and logs average worker utilization and queue sizes.
3.  **`WorkerNodeActor` (`qb::Actor`):
    *   **Role:** Executes assigned tasks.
    *   **State:** `_current_task`, `_metrics` (`WorkerMetrics`), `_is_busy`, `_simulation_start_time`, `_busy_start_time`.
    *   **Initialization:** Sends periodic `WorkerHeartbeatMessage` and `WorkerStatusMessage` to scheduler using `async::callback` (`scheduleHeartbeat`, `scheduleMetricsUpdate`).
    *   **Event Handling:**
        *   `on(TaskAssignmentMessage&)`: If not `_is_busy`, accepts the task, sets `_is_busy = true`, records `_current_task`, updates task status to `IN_PROGRESS`, pushes `TaskStatusUpdateMessage` to scheduler. Schedules `completeCurrentTask()` using `async::callback` with a delay based on `task->complexity`.
    *   **Task Completion (`completeCurrentTask` - called from callback):**
        *   Calculates processing time.
        *   Determines success/failure (randomly).
        *   Updates internal `_metrics`.
        *   Creates `TaskResult`.
        *   `push`es `ResultMessage` to `ResultCollectorActor`.
        *   `push`es `TaskStatusUpdateMessage` (COMPLETED/FAILED) to `TaskSchedulerActor`.
        *   Sets `_is_busy = false`.
4.  **`ResultCollectorActor` (`qb::Actor`):
    *   **Role:** Aggregates results from workers.
    *   **State:** `_results` (map `task_id` -> `TaskResult`).
    *   **Event Handling:**
        *   `on(ResultMessage&)`: Stores the received `result` in the `_results` map and logs it.
        *   `on(ShutdownMessage&)`: Calculates final statistics (success rate, average time) from `_results` and prints them.
5.  **`SystemMonitorActor` (`qb::Actor`):
    *   **Role:** Orchestrates the entire simulation.
    *   **Initialization:** Creates all other actors. Sends `InitializeMessage` to start them. Sends the initial `UpdateWorkersMessage` to the scheduler.
    *   **Monitoring:** Uses `async::callback` to periodically call `schedulePerformanceReport()`. This calculates and logs system-wide stats (`SystemStatsMessage` sent to self) using global atomic counters (`g_total_tasks`, etc.).
    *   **Shutdown:** Uses `async::callback` to schedule `shutdownSystem()` after `SIMULATION_DURATION_SECONDS`. `shutdownSystem()` calculates final stats and broadcasts `ShutdownMessage` to all actors.

## Key Concepts Illustrated

*   **Complex Actor Workflow:** Demonstrates a multi-stage pipeline involving generation, scheduling, execution, collection, and monitoring actors.
*   **Dynamic Load Balancing:** Scheduler uses worker metrics (heartbeat, utilization - though utilization update seems less frequent in `WorkerNodeActor`) to distribute tasks.
*   **Task Prioritization:** `TaskSchedulerActor` sorts its queue before dispatching.
*   **Worker Monitoring:** Workers proactively send heartbeats and status updates.
*   **Asynchronous Simulation:** Heavy use of `async::callback` to simulate processing times and schedule periodic reporting/actions without blocking.
*   **Centralized Coordination & State:** `TaskSchedulerActor` manages tasks/workers; `ResultCollectorActor` manages results; `SystemMonitorActor` manages the simulation lifecycle.
*   **Atomic Counters:** Global `std::atomic` variables used for simple, thread-safe aggregation of high-level statistics across actors/cores. 