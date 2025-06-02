@page example_analysis_dist_comp_md Case Study: Simulating Distributed Computing with QB Actors
@brief An in-depth look at the `distributed_computing` example, showcasing task distribution, parallel processing, load balancing, and monitoring in a multi-actor QB system.

# Case Study: Simulating Distributed Computing with QB Actors

*   **Location:** `example/core/example10_distributed_computing.cpp`
*   **Objective:** This example constructs a simulation of a distributed computing environment. It effectively demonstrates how multiple QB actors can collaborate to manage a workflow involving task generation, intelligent scheduling, parallel execution by worker nodes, result aggregation, and overall system monitoring. It's a valuable case study for understanding more complex, multi-stage actor interactions and asynchronous process simulation.

Through this analysis, you will see how QB facilitates:
*   Complex, multi-actor workflows.
*   Dynamic task scheduling and basic load balancing concepts.
*   Simulation of work and delays using asynchronous callbacks.
*   Centralized coordination and state management by dedicated actors.
*   System-wide monitoring and statistics gathering.

## Architectural Overview: A Distributed Workflow

The simulation is orchestrated by several types of actors, each with distinct responsibilities, potentially running across multiple `VirtualCore`s:

### 1. `TaskGeneratorActor`
*   **Role:** Periodically creates new computational `Task` objects (defined in the example with properties like type, priority, complexity, and data).
*   **QB Integration:** Standard `qb::Actor`.
*   **Functionality:**
    *   Uses `qb::io::async::callback` to trigger `generateTask()` at intervals determined by a `TASKS_PER_SECOND` constant.
    *   `generateTask()`: Constructs a `Task` object with randomized attributes.
    *   Encapsulates the `Task` within a `TaskMessage` event and `push`es it to the `TaskSchedulerActor`.
    *   Ceases generation upon receiving a `ShutdownMessage`.

### 2. `TaskSchedulerActor`: The Central Dispatcher & Load Balancer
*   **Role:** Receives tasks, queues them, and dispatches them to available `WorkerNodeActor`s based on a scheduling strategy (prioritization and basic load awareness).
*   **QB Integration:** Standard `qb::Actor`.
*   **State Management:**
    *   `_worker_ids`: A list of available `WorkerNodeActor` IDs.
    *   `_worker_metrics`: A map (`qb::ActorId` to `WorkerMetrics`) storing the last known status (heartbeat, utilization) of each worker.
    *   `_task_queue`: A `std::deque` of `std::shared_ptr<Task>`, acting as the pending task buffer.
    *   `_active_tasks`: A map (`task_id` to `std::shared_ptr<Task>`) tracking tasks currently assigned to workers.
*   **Event Handling & Logic:**
    *   `on(TaskMessage&)`: Adds the new task to `_task_queue` and attempts to schedule it immediately via `scheduleTasks()`.
    *   `on(WorkerHeartbeatMessage&)`: Updates the `last_heartbeat` timestamp for the reporting worker in `_worker_metrics`.
    *   `on(WorkerStatusMessage&)`: Updates the full `WorkerMetrics` (including utilization) for the reporting worker.
    *   `on(ResultMessage&)`: Receives `TaskResult` from a `ResultCollectorActor` (indirectly signaling task completion by a worker), removes the task from `_active_tasks`, and attempts to `scheduleTasks()` to dispatch new work if workers are free.
    *   `on(UpdateWorkersMessage&)`: Receives the initial list of `WorkerNodeActor` IDs from the `SystemMonitorActor`.
*   **Scheduling (`scheduleTasks()`):
    1.  Sorts `_task_queue` by `Task::priority` (higher priority first).
    2.  Iterates through known `_worker_ids`.
    3.  For each worker, calls `isWorkerAvailable()` to check its status.
    4.  If a worker is available and tasks are pending, dequeues the highest priority task, moves it to `_active_tasks`, and `push`es a `TaskAssignmentMessage` (containing the `Task`) to that worker.
*   **Worker Availability (`isWorkerAvailable()`):** Checks if metrics for the worker exist, if its `last_heartbeat` is recent (within `HEARTBEAT_TIMEOUT`), and if its reported `utilization` is below a defined threshold (e.g., 80%), indicating it has capacity.
*   **Load Assessment (`assessLoadBalance()`):** Periodically scheduled via `qb::io::async::callback`. Calculates and logs average worker utilization and current task queue sizes for monitoring purposes.

### 3. `WorkerNodeActor`: The Task Executor
*   **Role:** Receives `TaskAssignmentMessage`s and simulates the execution of the assigned `Task`.
*   **QB Integration:** Standard `qb::Actor`.
*   **State Management:** `_current_task`, `_metrics` (its own `WorkerMetrics`), `_is_busy` flag, `_simulation_start_time`, `_busy_start_time`.
*   **Initialization & Reporting (`onInit()`, `scheduleHeartbeat()`, `scheduleMetricsUpdate()`):
    *   In `onInit()`, after receiving `InitializeMessage`, it starts two recurring `qb::io::async::callback` chains:
        *   `scheduleHeartbeat()`: Periodically sends a `WorkerHeartbeatMessage` (containing its ID, current time, and busy status) to the `TaskSchedulerActor`.
        *   `scheduleMetricsUpdate()`: Periodically calculates its own utilization and other metrics, then sends a `WorkerStatusMessage` to the `TaskSchedulerActor`.
*   **Task Execution (`on(TaskAssignmentMessage&)`):
    1.  If not already `_is_busy`, accepts the task.
    2.  Sets `_is_busy = true`, stores `_current_task`, updates the task's status to `IN_PROGRESS`.
    3.  `push`es a `TaskStatusUpdateMessage` back to the `TaskSchedulerActor`.
    4.  **Simulates Work:** Schedules a call to its own `completeCurrentTask()` method using `qb::io::async::callback`. The delay for this callback is determined by the `task->complexity` (using `generateProcessingTime()`).
*   **Task Completion (`completeCurrentTask()` - invoked by the scheduled callback):
    1.  Calculates simulated processing time.
    2.  Randomly determines if the task succeeded or failed.
    3.  Updates its internal `_metrics` (total tasks processed, processing time, success/failure count).
    4.  Creates a `TaskResult` object.
    5.  `push`es a `ResultMessage` (containing the `TaskResult`) to the `ResultCollectorActor`.
    6.  `push`es a final `TaskStatusUpdateMessage` (COMPLETED or FAILED) to the `TaskSchedulerActor`.
    7.  Sets `_is_busy = false`, making itself available for new tasks.

### 4. `ResultCollectorActor`: The Aggregator
*   **Role:** Receives `ResultMessage`s from `WorkerNodeActor`s and stores/logs the outcomes of completed tasks.
*   **QB Integration:** Standard `qb::Actor`.
*   **State Management:** `_results` (a map from `task_id` to `TaskResult`).
*   **Event Handling:**
    *   `on(InitializeMessage&)`: Activates the actor.
    *   `on(ResultMessage&)`: Stores the received `result` in its `_results` map and logs the result to the console.
    *   `on(ShutdownMessage&)`: Calculates and prints final statistics (overall success rate, average processing time across all tasks) based on the collected `_results`.

### 5. `SystemMonitorActor`: The Orchestrator & Overseer
*   **Role:** Sets up the entire simulation, starts the actors, monitors overall system performance, and initiates a graceful shutdown.
*   **QB Integration:** Standard `qb::Actor`.
*   **Initialization (`onInit()` and `on(InitializeMessage&)`):
    *   Creates instances of all other actor types (`TaskGeneratorActor`, `TaskSchedulerActor`, `ResultCollectorActor`, and a pool of `WorkerNodeActor`s), assigning them to different cores (`qb::Main::addActor`).
    *   Stores their `ActorId`s.
    *   `push`es an `InitializeMessage` to all created actors to kickstart their operations.
    *   Sends an initial `UpdateWorkersMessage` (containing all worker IDs) to the `TaskSchedulerActor`.
*   **Performance Monitoring (`schedulePerformanceReport()`):
    *   Uses `qb::io::async::callback` to periodically call itself (by pushing a `SystemStatsMessage` to its own ID after calculation).
    *   Calculates system-wide statistics (e.g., total tasks generated, completed, failed; overall throughput) using global `std::atomic` counters (which are incremented by other actors like `TaskGeneratorActor` and `WorkerNodeActor`).
    *   Logs these system-wide stats.
*   **Simulation Shutdown (`shutdownSystem()`):
    *   Scheduled via `qb::io::async::callback` to run after `SIMULATION_DURATION_SECONDS`.
    *   Performs a final performance report.
    *   `broadcast<ShutdownMessage>()` to all actors to signal them to stop their activities and terminate gracefully.
    *   Finally, calls `qb::Main::stop()` to stop the entire engine.

## Key QB Concepts & Patterns Demonstrated

*   **Complex Multi-Actor Workflow:** Illustrates a sophisticated pipeline with clear separation of concerns: generation, scheduling, execution, collection, and system-level monitoring.
*   **Asynchronous Simulation of Work:** Extensive use of `qb::io::async::callback` to simulate task processing times and to schedule periodic actions (heartbeats, metrics updates, report generation) without blocking any `VirtualCore`.
*   **Centralized Coordination & State:**
    *   `TaskSchedulerActor`: Manages the central task queue and worker availability.
    *   `ResultCollectorActor`: Aggregates all final task outcomes.
    *   `SystemMonitorActor`: Orchestrates the setup and shutdown of the entire simulation.
*   **Dynamic Task Scheduling & Basic Load Balancing:** The `TaskSchedulerActor` uses worker heartbeats and reported metrics (utilization) to make decisions about which worker should receive the next task, also prioritizing tasks from its queue.
*   **Worker Self-Monitoring and Reporting:** `WorkerNodeActor`s proactively send heartbeats and status updates to the scheduler, enabling the scheduler to make informed decisions.
*   **Inter-Actor Event Design:** Shows various custom event types (`TaskMessage`, `WorkerStatusMessage`, `ResultMessage`, `ShutdownMessage`, etc.) designed to carry specific information between collaborating actors.
*   **Graceful Shutdown:** A coordinated shutdown process initiated by the `SystemMonitorActor` via a `ShutdownMessage`, allowing other actors to finalize their work and report statistics before the engine stops.
*   **Global Atomics for High-Level Stats:** Use of `std::atomic` global counters demonstrates a simple way to aggregate high-level system statistics from multiple concurrent actors with thread safety (though for more complex stats, a dedicated statistics actor would be preferable).

This `distributed_computing` example serves as a rich template for understanding how to structure larger, more intricate applications using the QB Actor Framework, particularly those involving distributed workflows and resource management.

**(Next Example Analysis:** [file_monitor Example Analysis](./file_monitor_analysis.md)**) 