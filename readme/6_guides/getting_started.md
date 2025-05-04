# Getting Started with QB Actor Framework

This guide walks through setting up, building, and running a basic application using the QB framework.

## 1. Prerequisites

Ensure you have:

*   A C++17 compliant compiler (GCC 7+, Clang 5+, MSVC 2017+).
*   CMake (version 3.14+).
*   Git.
*   *(Optional)* OpenSSL development libraries (for SSL/Crypto features).
*   *(Optional)* Zlib development libraries (for Compression features).

## 2. Getting the Code

Clone the repository:

```bash
git clone <repository_url> qb-framework
cd qb-framework
# If submodules are used (check .gitmodules), initialize them:
# git submodule update --init --recursive
```

## 3. Building the Framework

Use CMake to configure and build the libraries, examples, and tests.

```bash
# Create a build directory (out-of-source build recommended)
mkdir build
cd build

# Configure (adjust options as needed)
c# Example: Release build with tests enabled
c# Use -DCMAKE_INSTALL_PREFIX=/path/to/install for installation location
c# Use -DQB_IO_WITH_SSL=ON -DQB_IO_WITH_ZLIB=ON to enable optional features
c# Use -DQB_BUILD_TEST=ON to build tests (often default)
c# Use -DCMAKE_BUILD_TYPE=Release (or Debug)
c
cmake .. -DCMAKE_BUILD_TYPE=Release -DQB_BUILD_TEST=ON

# Build
c# This builds libraries (qb-io, qb-core), examples, and tests
ccmake --build . --config Release # Or use 'make -jN' on Linux/macOS

# Optional: Install headers and libraries
c# cmake --install . --prefix /usr/local --config Release
```

Executables for examples and tests are typically found in `build/bin/<module>/<type>/`.

**(See:** `[Reference: Building](./../7_reference/building.md)` for all options**)

## 4. Your First Actor Application ("Hello Actor")

Let's create a minimal application.

**a) `hello_main.cpp`:**

```cpp
#include <qb/main.h>     // Engine controller
#include <qb/actor.h>    // Actor base class
#include <qb/event.h>    // Event base class
#include <qb/io.h>       // For thread-safe qb::io::cout
#include <iostream>
#include <string>

// --- Define an Event --- 
struct HelloEvent : qb::Event {
    std::string greeting;
    // Constructor to pass data
    explicit HelloEvent(std::string msg) : greeting(std::move(msg)) {}
};

// --- Define an Actor --- 
class HelloActor : public qb::Actor {
public:
    // Called after actor creation and ID assignment
    bool onInit() override {
        qb::io::cout() << "HelloActor [" << id() << "] initialized on core " << getIndex() << ". Waiting for greetings...\n";
        // Subscribe this actor to handle HelloEvent messages
        registerEvent<HelloEvent>(*this);
        // Always good practice to handle shutdown
        registerEvent<qb::KillEvent>(*this);
        return true; // Indicate successful initialization
    }

    // Event handler for HelloEvent
    void on(const HelloEvent& event) {
        qb::io::cout() << "HelloActor [" << id() << "] received: '" << event.greeting << "' from Actor " << event.getSource() << "\n";
        // Terminate after receiving one greeting
        kill();
    }

    // Event handler for KillEvent (for graceful shutdown)
    void on(const qb::KillEvent& event) {
        qb::io::cout() << "HelloActor [" << id() << "] shutting down.\n";
        kill(); // Call base kill()
    }

    // Destructor (called after kill() completes)
    ~HelloActor() override {
        qb::io::cout() << "HelloActor [" << id() << "] destroyed.\n";
    }
};

// --- Main Function --- 
int main() {
    qb::io::cout() << "--- QB Hello Actor Example ---\n";

    // 1. Create the QB Engine
    qb::Main engine;

    // 2. Add Actors to Cores
    // Add HelloActor instance to core 0. Store its ID.
    qb::ActorId hello_actor_id = engine.addActor<HelloActor>(0);

    if (!hello_actor_id.is_valid()) {
        std::cerr << "Failed to add HelloActor!\n";
        return 1;
    }
    qb::io::cout() << "Added HelloActor with ID: " << hello_actor_id << "\n";

    // 3. Start the Engine (Synchronously for this simple example)
    qb::io::cout() << "Starting engine...\n";
    engine.start(false); // false = block until engine stops

    // --- Engine has started and actors are initializing ---
    // --- At this point, we could send the initial event, but since start(false)
    // --- blocks, we need another way. For simplicity, let's assume another actor
    // --- or an external mechanism sends the event. In a real app, you might
    // --- start asynchronously and send from the main thread, or have an
    // --- initializer actor send the first message.

    // --- For demonstration, imagine an event was pushed after start():
    // --- engine.core(0).push<HelloEvent>(hello_actor_id, hello_actor_id, "Hello from Main!");
    // --- Note: Directly accessing core() or push() after start() is not thread-safe
    // ---       if start(true) was used. Send events *from other actors*.

    // 4. Engine stops (because HelloActor calls kill())
    qb::io::cout() << "Engine stopped.\n";

    // 5. Check for Errors
    if (engine.hasError()) {
        std::cerr << "Engine stopped with an error!\n";
        return 1;
    }

    qb::io::cout() << "--- Example Finished ---\n";
    return 0;
}

```

**b) `CMakeLists.txt` (Example):**

```cmake
cmake_minimum_required(VERSION 3.14)
project(hello_actor_example CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find QB package (Adjust PATHS to your QB install location or build directory)
# If building QB alongside your project, you might use add_subdirectory(path/to/qb)
find_package(qb REQUIRED CONFIG PATHS /path/to/qb/install/lib/cmake/qb)

add_executable(hello_actor hello_main.cpp)

# Link to qb::core (which includes qb::io)
target_link_libraries(hello_actor PRIVATE qb::core)
```

**c) Build and Run:**

Compile using CMake as shown in step 3, then run the `hello_actor` executable.

**(Important Note:** The example above doesn't send the initial `HelloEvent`. A more complete example would involve another actor sending the event after `engine.start()`, or starting the engine asynchronously `engine.start(true)` and having the main thread (or another mechanism) send the event *before* calling `engine.join()`.)**

## Running Included Examples

The framework includes numerous examples in the `example/` directory, categorized under `core`, `io`, and `core_io`.

1.  **Build them:** Ensure they were built (`cmake --build .`).
2.  **Locate:** Find the executables in your build directory (e.g., `build/bin/qb-core/examples/example1_basic_actors`).
3.  **Run:** Execute them from the command line.
    ```bash
    # Example: Run the basic actor communication test
    ./build/bin/qb-core/examples/example1_basic_actors

    # Example: Run the chat server (needs client started separately)
    ./build/bin/qb-core/examples/core_io/chat_tcp/server/chat_server
    ```

Explore these examples to see practical implementations of various features. 