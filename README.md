# QB Actor Framework: High-Performance C++17 for Concurrent & Distributed Systems

<p align="center"><img src="./resources/logo.svg" width="180px" alt="QB Actor Framework Logo" /></p>

**Unlock the power of modern C++ for complex concurrent applications. QB is an actor-based framework meticulously engineered for developers seeking exceptional performance, scalability, and a more intuitive way to manage concurrency.**

QB simplifies the art of building responsive, real-time systems, network services, and distributed computations by harmonizing the robust **Actor Model** with a high-efficiency **Asynchronous I/O Engine**. Focus on your application's logic; let QB handle the intricacies of parallelism and non-blocking I/O.
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://en.wikipedia.org/wiki/C%2B%2B17) [![CMake](https://img.shields.io/badge/CMake-3.14+-blue.svg)](https://cmake.org/) [![Cross Platform](https://img.shields.io/badge/Platform-Linux%20%7C%20macOS%20%7C%20Windows-lightgrey.svg)](https://github.com/isndev/qb) [![Architecture](https://img.shields.io/badge/Arch-x86__64%20%7C%20ARM64-lightgrey.svg)](https://github.com/isndev/qb) [![License](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](https://opensource.org/licenses/Apache-2.0)

## Quick Start with QB

### Using the Project Generator

The fastest way to get started with QB is to use our boilerplate project generator:

**Using cURL:**
```bash
curl -o- https://raw.githubusercontent.com/isndev/qb/main/script/qb-new-project.sh | bash /dev/stdin MyProject
```

**Using Wget:**
```bash
wget -qO- https://raw.githubusercontent.com/isndev/qb/main/script/qb-new-project.sh | bash /dev/stdin MyProject
```

**Build and run:**
```bash
cd MyProject
cmake -DCMAKE_BUILD_TYPE=Release -B build
cd build && make
./my_project
```

### Your First Actor in 30 Seconds

```cpp
#include <qb/main.h>
#include <qb/actor.h>

// Define an event
struct GreetingEvent : qb::Event {
    qb::string message;
    GreetingEvent(std::string msg) : message(std::move(msg)) {}
};

// Define an actor
class GreeterActor : public qb::Actor {
public:
    bool onInit() final {
        registerEvent<GreetingEvent>(*this); // register event
        push<GreetingEvent>(id(), "Hello !"); // send to self 
        return true;
    }

    void on(const GreetingEvent& event) {
        qb::io::cout() << "Received: " << event.message << std::endl;
        kill(); // Job done, kill me
    }
};

int main() {
    qb::Main engine;
    auto actor_id = engine.addActor<GreeterActor>(0); // add actor to core 0
    
    engine.start();
    engine.join();
    return 0;
}
```

That's it! No mutexes, no thread management, no race conditions. Just pure actor communication.

## Why QB Changes Everything

### Multithreading Without the Pain
Traditional multithreading is complex, error-prone, and hard to debug. QB's actor model eliminates these problems:

```cpp
// Traditional approach: Complex threading with mutexes
std::mutex mtx;
std::queue<Task> shared_queue;
std::condition_variable cv;

// QB approach: Clean actor communication
push<TaskEvent>(worker_actor, task_data);
```

**Benefits:**
- **No Shared State**: Each actor owns its data, eliminating race conditions
- **No Mutexes**: Actors communicate via messages, not shared memory
- **No Deadlocks**: Sequential message processing per actor
- **Automatic Scaling**: Distribute actors across CPU cores effortlessly

### Cross-Platform by Design
QB runs everywhere your C++17 compiler can reach:

**Platforms:**
- **Linux** (Ubuntu 18.04+, RHEL 7+, Alpine Linux)
- **macOS** (10.14+, Intel & Apple Silicon)
- **Windows** (Windows 10+, MSVC 2017+, MinGW)

**Architectures:**
- **x86_64** (Intel/AMD 64-bit)
- **ARM64** (ARMv8, Apple M1/M2, Raspberry Pi 4+)

**Same code, same performance, everywhere.**

### Performance That Scales
QB is built for speed from the ground up:

```cpp
// Distribute work across all CPU cores automatically
qb::Main engine;

auto num_cores = std::thread::hardware_concurrency();
// Add worker actors to different cores
for (int i = 0; i < num_cores; ++i) {
    engine.addActor<WorkerActor>(i);
}

// Lock-free message passing between cores
// Linear scaling with CPU count
```

### Simplicity Meets Power

**Simple APIs for complex problems:**

```cpp
// HTTP Server in 10 lines
class HttpActor : public qb::Actor, public qb::http::Server<> {
    bool onInit() final {
        router().get("/api/status", [](auto ctx) {
            ctx->response().body() = R"({"status": "ok"})";
            ctx->complete();
        });
        
        router().compile();
        if (!listen({"tcp://0.0.0.0:8080"}))
            return false;

        start(); // start async io
        return true;
    }
};
```

## QB Modules Ecosystem

Extend QB's capabilities with our official modules:

### Network & Communication
- **[qbm-http](https://github.com/isndev/qbm-http)** - HTTP/1.1 & HTTP/2 client/server with routing, middleware, authentication
- **[qbm-websocket](https://github.com/isndev/qbm-websocket)** - WebSocket protocol implementation (RFC 6455 compliant)

### Database Integration  
- **[qbm-pgsql](https://github.com/isndev/qbm-pgsql)** - Asynchronous PostgreSQL client with prepared statements and transactions
- **[qbm-redis](https://github.com/isndev/qbm-redis)** - Comprehensive Redis client supporting all data types and clustering

### Adding Modules

```bash
# Add any module as a submodule
git submodule add https://github.com/isndev/qbm-http qbm/http
```

```cmake
# CMakeLists.txt - Automatic module discovery
qb_load_modules("${CMAKE_CURRENT_SOURCE_DIR}/qbm")
target_link_libraries(my_app PRIVATE qbm::http)
```

```cpp
// Use immediately
#include <http/http.h>
```

## Core Features

**Actor System (`qb-core`):**
- Lightweight actors with automatic lifecycle management
- Type-safe event system with guaranteed message ordering
- Efficient inter-core communication using lock-free queues
- Built-in patterns: Service actors, periodic tasks, supervisors

**Asynchronous I/O (`qb-io`):**
- Non-blocking TCP, UDP, and SSL/TLS networking
- Extensible protocol framework with built-in parsers
- File system watching and OS signal handling
- Cross-platform event loop powered by libev

**Performance & Scalability:**
- Multi-core distribution with CPU affinity control
- Zero-copy message passing where possible
- Cache-friendly data structures and minimal allocations

**Developer Experience:**
- Modern C++17 with clean, expressive APIs
- Extensive utility library (time, crypto, compression, containers)
- Comprehensive documentation and examples

## Build Information

### Build Requirements
- **C++17** compatible compiler (GCC 7+, Clang 5+, MSVC 2017+)
- **CMake 3.14+**
- **Git** (for submodules)

### Optional Dependencies
- **OpenSSL 1.1+** - SSL/TLS support, cryptographic functions
- **Argon2** - Password hashing and key derivation functions
- **Zlib** - Compression features available

### Building from Source

```bash
# Clone with submodules
git clone --recursive https://github.com/isndev/qb.git
cd qb

# Configure build
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release \
      -DQB_IO_WITH_SSL=ON \
      -DQB_IO_WITH_ZLIB=ON \
      ..

# Build (parallel)
make -j$(nproc)  # Linux/macOS
# or
cmake --build . --parallel  # Cross-platform
```

### CMake Configuration Options

| Option | Description | Default |
|--------|-------------|---------|
| `QB_IO_WITH_SSL` | Enable SSL/TLS support | `ON` (if found) |
| `QB_IO_WITH_ZLIB` | Enable compression support | `ON` (if found) |
| `QB_LOGGER` | Enable high-performance logging | `ON` |
| `QB_BUILD_TEST` | Build unit tests | `OFF` |
| `QB_BUILD_BENCHMARK` | Build benchmark tests | `OFF` |

### Platform-Specific Notes

**Windows (Visual Studio):**
```bash
cmake -G "Visual Studio 16 2019" -A x64 ..
cmake --build . --config Release
```

**Windows (MinGW):**
```bash
cmake -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release ..
mingw32-make -j4
```

**macOS (Xcode):**
```bash
cmake -G Xcode ..
cmake --build . --config Release
```

## Learn with Examples

### Practical Examples Repository

```bash
# Add examples to any QB project
git submodule add https://github.com/isndev/qb-examples examples
add_subdirectory(examples)
```

**Example categories:**
- **Basic Patterns** - Actor communication, lifecycle, timers
- **Network Applications** - TCP servers, HTTP APIs, WebSocket chat
- **Database Integration** - PostgreSQL and Redis patterns
- **Performance Examples** - High-throughput systems, load balancing
- **Real-World Projects** - Trading systems, distributed computing

### Documentation Structure

The documentation is organized in these main sections:

- `readme/1_introduction/` - Philosophy and design principles
- `readme/2_core_concepts/` - Actors, events, async I/O, concurrency
- `readme/3_qb_io/` - Asynchronous I/O library details
- `readme/4_qb_core/` - Actor engine and messaging
- `readme/5_core_io_integration/` - How core and I/O work together
- `readme/6_guides/` - Practical patterns and performance tuning
- `readme/7_reference/` - API reference and FAQ

## Advanced Documentation

For comprehensive technical documentation, implementation details, and in-depth framework guides:

**📖 [Complete QB Framework Documentation](./readme/README.md)**

This detailed documentation covers:
- **[Introduction](./readme/1_introduction/)** - Framework philosophy, design principles, and architectural overview
- **[Core Concepts](./readme/2_core_concepts/)** - Actor model, event system, concurrency patterns, and async I/O fundamentals
- **[QB-IO Module](./readme/3_qb_io/)** - Asynchronous I/O library, networking, protocols, streams, and utilities
- **[QB-Core Module](./readme/4_qb_core/)** - Actor engine, messaging system, lifecycle management, and patterns
- **[Core-IO Integration](./readme/5_core_io_integration/)** - How the actor system integrates with async I/O operations
- **[Practical Guides](./readme/6_guides/)** - Best practices, performance tuning, error handling, and real-world patterns
- **[Technical Reference](./readme/7_reference/)** - Complete API reference, build system, testing, and FAQ

The advanced documentation provides:
- **Architecture Deep Dive** - Internal framework mechanics and design decisions
- **Performance Optimization** - Multi-core scaling, memory management, and profiling techniques
- **Actor Patterns** - State machines, supervision, dependency injection, and service discovery
- **Network Programming** - TCP/UDP servers, SSL/TLS, protocol design, and client implementations
- **System Integration** - File I/O, signals, timers, and cross-platform considerations
- **Production Deployment** - Configuration, monitoring, debugging, and troubleshooting

## Real-World Applications

QB is production-ready and powers:
- **Financial Trading Systems** - Low-latency order processing
- **IoT Gateways** - Device management and data aggregation  
- **Game Servers** - Player session management and real-time communication
- **Microservices** - Scalable HTTP APIs and message processing
- **Data Pipelines** - Stream processing and ETL systems

## Contributing

We welcome contributions! Please see our [Contributing Guidelines](./CONTRIBUTING.md) for details on:
- Code style and standards
- Testing requirements
- Pull request process
- Issue reporting

## License

QB Actor Framework is licensed under the Apache License, Version 2.0. See the [LICENSE](./LICENSE) file for details.

## Acknowledgments

QB Actor Framework builds upon the excellent work of several open-source projects. We extend our gratitude to:

- **[OpenSSL](https://www.openssl.org/)** - For SSL/TLS encryption and cryptographic functions
- **[Argon2](https://github.com/P-H-C/phc-winner-argon2)** - For the secure password hashing algorithm
- **[Zlib](https://zlib.net/)** - For efficient data compression capabilities
- **[libev](http://libev.schmorp.de/)** - For the robust event loop foundation
- **[stduuid](https://github.com/mariusbancila/stduuid)** - For the comprehensive UUID v4 implementation
- **[nlohmann/json](https://github.com/nlohmann/json)** - For the outstanding modern C++ JSON library
- **[nanolog](https://github.com/Iyengar111/NanoLog)** - For the high-performance logging system
- **[ska_hash](https://github.com/skarupke/flat_hash_map)** - For the fast hash table implementations

These libraries enable QB to deliver exceptional performance and functionality while maintaining clean, modern C++ APIs.

---

*Built for developers who demand both simplicity and performance in concurrent C++ applications.*

