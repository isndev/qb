@page guides_getting_started_md Getting Started with the QB Actor Framework
@brief Your first steps to building high-performance concurrent applications with QB.

# Getting Started with the QB Actor Framework

Welcome to the QB Actor Framework! This guide will walk you through setting up your environment, building the framework, and running your first simple actor-based application. Our goal is to get you from zero to a running QB application quickly.

## 1. What You'll Need (Prerequisites)

Before you begin, ensure your development environment meets these requirements:

*   **C++17 Compliant Compiler:** GCC 7+, Clang 5+, or MSVC 2017+.
*   **CMake:** Version 3.14 or higher.
*   **Git:** For cloning the QB Framework repository.
*   **(Optional but Recommended for Full Features)**
    *   **OpenSSL Development Libraries:** If you plan to use SSL/TLS for secure networking or QB's cryptography features (`qb::crypto`, `qb::jwt`).
    *   **Zlib Development Libraries:** If you intend to use data compression features (`qb::compression`).

## 2. Obtain the QB Framework Code

First, clone the QB Actor Framework repository to your local machine:

```bash
git clone <your_repository_url> qb-framework
cd qb-framework
```

If the QB framework uses Git submodules for dependencies (check for a `.gitmodules` file), initialize and update them:

```bash
# If submodules are present:
git submodule update --init --recursive
```

## 3. Build the QB Framework Libraries

QB uses CMake for its build system. Here's a typical out-of-source build process:

```bash
# From the root of the qb-framework directory

# 1. Create a build directory
mkdir build
cd build

# 2. Configure the build (from within the 'build' directory)
#    This example creates a Release build and enables tests.
#    Adjust QB_IO_WITH_SSL and QB_IO_WITH_ZLIB if you have OpenSSL/Zlib and need those features.
cmake .. -DCMAKE_BUILD_TYPE=Release -DQB_BUILD_TEST=ON -DQB_IO_WITH_SSL=OFF -DQB_IO_WITH_ZLIB=OFF

# 3. Build the framework (libraries, examples, tests)
cmake --build . --config Release
# On Linux/macOS, you can often speed this up with: make -j$(nproc) or make -j<number_of_cores>
```

This process compiles the `qb-io` and `qb-core` libraries. For more detailed build options and installation instructions, please refer to the [Reference: Building the QB Framework](./../7_reference/building.md).

## 4. Your First QB Application: "PingPongActor"

Let's create a minimal application with two actors: one sends a "Ping", and the other replies with a "Pong".

**a) Create a Project Directory:**

Outside the `qb-framework` directory, create a new directory for your project, for example, `my_qb_app`.

```bash
mkdir my_qb_app
cd my_qb_app
```

**b) `ping_pong_main.cpp`:**

Create a file named `ping_pong_main.cpp` in your `my_qb_app` directory with the following content:

```cpp
#include <qb/main.h>     // QB Engine: qb::Main
#include <qb/actor.h>    // Actor base class: qb::Actor, qb::ActorId
#include <qb/event.h>    // Event base class: qb::Event
#include <qb/io.h>       // For qb::io::cout (thread-safe console output)
#include <iostream>      // For std::endl

// --- 1. Define Events ---
// Event sent from Pinger to Ponger
struct PingEvent : qb::Event {
    qb::ActorId pinger_id; // So Ponger knows who to reply to
    int ping_value;

    PingEvent(qb::ActorId sender, int val) : pinger_id(sender), ping_value(val) {}
};

// Event sent from Ponger back to Pinger
struct PongEvent : qb::Event {
    int pong_value;

    explicit PongEvent(int val) : pong_value(val) {}
};

// --- 2. Define Pinger Actor ---
class PingerActor : public qb::Actor {
private:
    qb::ActorId _ponger_actor_id;
    int _pings_sent = 0;
    const int _max_pings = 3;

public:
    // Constructor: Takes the ID of the Ponger actor
    explicit PingerActor(qb::ActorId ponger_id) : _ponger_actor_id(ponger_id) {}

    bool onInit() override {
        qb::io::cout() << "PingerActor [" << id() << "] initialized. Target Ponger: " << _ponger_actor_id << ".\n";
        registerEvent<PongEvent>(*this); // Pinger needs to handle Pong replies
        registerEvent<qb::KillEvent>(*this);

        // Send the first Ping
        sendNextPing();
        return true;
    }

    void on(const PongEvent& event) {
        qb::io::cout() << "PingerActor [" << id() << "] received Pong with value: " << event.pong_value << ".\n";
        if (_pings_sent < _max_pings) {
            sendNextPing();
        } else {
            qb::io::cout() << "PingerActor [" << id() << "] finished sending pings. Requesting Ponger to stop.\n";
            push<qb::KillEvent>(_ponger_actor_id); // Tell Ponger to stop
            kill(); // Pinger stops itself
        }
    }

    void on(const qb::KillEvent& /*event*/) {
        qb::io::cout() << "PingerActor [" << id() << "] shutting down.\n";
        kill();
    }

private:
    void sendNextPing() {
        _pings_sent++;
        qb::io::cout() << "PingerActor [" << id() << "] sending Ping #" << _pings_sent << " to " << _ponger_actor_id << ".\n";
        push<PingEvent>(_ponger_actor_id, id(), _pings_sent); // Send my ID and ping value
    }
};

// --- 3. Define Ponger Actor ---
class PongerActor : public qb::Actor {
public:
    bool onInit() override {
        qb::io::cout() << "PongerActor [" << id() << "] initialized and ready for Pings.\n";
        registerEvent<PingEvent>(*this); // Ponger handles Ping requests
        registerEvent<qb::KillEvent>(*this);
        return true;
    }

    void on(const PingEvent& event) {
        qb::io::cout() << "PongerActor [" << id() << "] received Ping #" << event.ping_value << " from " << event.pinger_id << ".\n";
        // Reply with a Pong, echoing the ping value
        push<PongEvent>(event.pinger_id, event.ping_value);
    }

    void on(const qb::KillEvent& /*event*/) {
        qb::io::cout() << "PongerActor [" << id() << "] shutting down.\n";
        kill();
    }
};

// --- 4. Main Application Logic ---
int main() {
    qb::io::cout() << "--- QB Ping-Pong Actor Example ---\n";

    // Create the QB Engine instance
    qb::Main engine;

    // Add the PongerActor to core 0. We need its ID to pass to PingerActor.
    qb::ActorId ponger_id = engine.addActor<PongerActor>(0);
    if (!ponger_id.is_valid()) {
        qb::io::cout() << "Error: Failed to add PongerActor!\n";
        return 1;
    }
    qb::io::cout() << "PongerActor created on core 0 with ID: " << ponger_id << ".\n";

    // Add the PingerActor to core 0, providing it with the PongerActor's ID.
    qb::ActorId pinger_id = engine.addActor<PingerActor>(0, ponger_id);
    if (!pinger_id.is_valid()) {
        qb::io::cout() << "Error: Failed to add PingerActor!\n";
        return 1;
    }
    qb::io::cout() << "PingerActor created on core 0 with ID: " << pinger_id << ".\n";

    // Start the engine. This call will block until all actors have terminated.
    qb::io::cout() << "Starting QB engine...\n";
    engine.start(false); // false = run synchronously in this thread

    qb::io::cout() << "QB engine has stopped.\n";

    // Check if any VirtualCore encountered an error during execution
    if (engine.hasError()) {
        qb::io::cout() << "Error: Engine stopped due to an error in a VirtualCore!\n";
        return 1;
    }

    qb::io::cout() << "--- Ping-Pong Example Finished Successfully ---\n";
    return 0;
}

```

**c) `CMakeLists.txt` for Your Application:**

Create a `CMakeLists.txt` file in your `my_qb_app` directory:

```cmake
cmake_minimum_required(VERSION 3.14)
project(MyQBApp CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find the QB package. 
# You might need to adjust CMAKE_PREFIX_PATH if QB was installed to a custom location,
# or if you are building QB alongside your project using add_subdirectory.
# Example for an installed QB:
# list(APPEND CMAKE_PREFIX_PATH "/path/to/your/qb-framework/build" "/path/to/your/qb-install-dir")
find_package(qb REQUIRED CONFIG)

# If QB was built as part of the same CMake project (e.g. via add_subdirectory)
# ensure you link to the targets directly (qb::qb-core, qb::qb-io)

add_executable(ping_pong_app ping_pong_main.cpp)

# Link your application against the qb-core library (which includes qb-io)
target_link_libraries(ping_pong_app PRIVATE qb::core)

```

**d) Build and Run Your Application:**

Navigate to your `my_qb_app` directory, then create a build directory, configure, and build:

```bash
# From within my_qb_app directory
mkdir build
cd build

# Configure - Point to your QB build or install directory if needed
# If QB was installed to /usr/local, CMake should find it.
# If QB was built in ../qb-framework/build, you might use:
# cmake .. -DCMAKE_PREFIX_PATH=../../qb-framework/build
cmake .. 

# Build
cmake --build . --config Release

# Run
./ping_pong_app 
# On Windows, the executable might be in a subdirectory like ./Release/ping_pong_app.exe
```

You should see output from both actors, demonstrating the ping-pong message exchange!

## 5. Exploring Further

Congratulations on running your first QB application!

*   **Included Examples:** The QB framework comes with many more examples in its `example/` directory (e.g., `example/core/`, `example/io/`, `example/core_io/`). Build them as part of the main QB build and explore their code to see various features in action.
*   **Core Concepts:** Dive deeper into the [Core Concepts](./../2_core_concepts/README.md) to solidify your understanding of actors, events, and asynchronous I/O.
*   **Module Documentation:** Explore the detailed documentation for [QB-IO Module](./../3_qb_io/README.md) and [QB-Core Module](./../4_qb_core/README.md).
*   **Other Guides:** Check out other guides for patterns, performance tuning, and error handling.

Happy coding with the QB Actor Framework! 