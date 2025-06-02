@page core_io_example_analyses_readme Case Studies: QB Core & IO Integration Examples
@brief Detailed analyses of example applications demonstrating the integration of QB-Core actors with QB-IO for networking and asynchronous tasks.

# Case Studies: QB Core & IO Integration Examples

This section provides in-depth analyses of several key example applications that showcase how `qb-core` actors and the `qb-io` library work together to build functional, asynchronous, and concurrent systems. Each case study breaks down the architecture, highlights the QB features used, and explains the design patterns employed.

Studying these examples is a practical way to see the concepts from the "Core Concepts," "QB-IO Module," and "QB-Core Module" sections applied in more complex scenarios.

## Available Example Analyses:

*   **[Case Study: Building a TCP Chat System with QB Actors](./chat_tcp_analysis.md)**
    *   Demonstrates a multi-core TCP chat server and client. Covers actor-based TCP client/server development, custom protocol handling, and session management.

*   **[Case Study: Simulating Distributed Computing with QB Actors](./distributed_computing_analysis.md)**
    *   Showcases task distribution, parallel processing by worker actors, basic load balancing concepts, and system monitoring in a simulated distributed environment.

*   **[Case Study: Building a File System Monitor with QB Actors](./file_monitor_analysis.md)**
    *   Illustrates how to use `qb-io`'s asynchronous file/directory watching capabilities within an actor-based system to react to file system changes.

*   **[Case Study: Asynchronous File Processing with Manager-Worker Actors](./file_processor_analysis.md)**
    *   Explains a Manager-Worker pattern for distributing potentially blocking file I/O tasks across multiple worker actors, maintaining system responsiveness.

*   **[Case Study: A Publish/Subscribe Message Broker with QB Actors](./message_broker_analysis.md)**
    *   Details the implementation of a topic-based publish/subscribe message broker using actors for logic and TCP for transport, highlighting efficient message fan-out techniques.

## Learning from Examples

Each analysis aims to:
*   Clarify the **purpose and architecture** of the example.
*   Pinpoint **how specific QB features** (like `qb::io::use<>`, `qb::io::async::callback`, custom events, protocols) are utilized.
*   Discuss the **design patterns** and **QB concepts** being illustrated.

We recommend reviewing these analyses after you have a grasp of the foundational concepts of QB.

**(Return to:** [Core & IO Integration Overview](../README.md)**) 