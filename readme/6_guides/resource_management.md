# QB Framework: Resource Management Guide

QB leverages standard C++ RAII (Resource Acquisition Is Initialization) and the actor lifecycle for robust resource management.

## 1. RAII: The Primary Method

This is the **strongly recommended** approach for managing resources within actors.

*   **How it Works:** Resources (memory, file handles, sockets, database connections, locks) are encapsulated within objects (often member variables of the actor). The resource is acquired in the object's constructor and automatically released in its destructor.
*   **Standard Tools:**
    *   **Memory:** `std::unique_ptr` for exclusive ownership, `std::shared_ptr` for shared ownership (use cautiously), standard containers (`std::vector`, `std::string`, `std::map`) manage their own memory.
    *   **Files:** `std::fstream` or `qb::io::sys::file`. Their destructors close the file handle.
    *   **Network Sockets:** `qb::io::tcp::socket`, `qb::io::udp::socket`, `qb::io::tcp::ssl::socket`. Their destructors call `close()`.
    *   **Locks (If needed for external interaction):** `std::lock_guard`, `std::unique_lock` automatically release mutexes.
*   **Actor Lifecycle Integration:** An actor's destructor (`~MyActor()`) is **guaranteed to run after** the actor terminates (via `kill()` or `KillEvent`) and is removed from the `VirtualCore`. Therefore, RAII members are cleaned up automatically and safely.

```cpp
#include <qb/actor.h>
#include <memory>  // For unique_ptr
#include <fstream> // For fstream
#include <vector>

class ResourceManager : public qb::Actor {
private:
    // --- Resources Managed by RAII --- 
    std::unique_ptr<char[]> _buffer; // Dynamically allocated buffer
    std::fstream _data_file;        // File handle
    std::vector<int> _items;        // Container managing its memory
    // NetworkSocketWrapper _connection; // Hypothetical RAII network connection class

public:
    bool onInit() override {
        try {
            // --- Acquire Resources --- 
            _buffer = std::make_unique<char[]>(4096);
            
            _data_file.open("my_actor_data.dat", std::ios::out | std::ios::app);
            if (!_data_file.is_open()) {
                std::cerr << "Failed to open data file!" << std::endl;
                return false; // Initialization failed
            }
            _items.reserve(100);
            // _connection.connect("..."); // Hypothetical

        } catch (const std::exception& e) {
            std::cerr << "Resource acquisition failed: " << e.what() << std::endl;
            return false; // Abort actor start
        }
        registerEvent<qb::KillEvent>(*this);
        return true;
    }

    void on(const qb::KillEvent&) {
        // Optional: Actions needed *before* resources are automatically released.
        // Example: Flush file buffer before closing.
        if (_data_file.is_open()) {
            _data_file.flush();
        }
        kill(); // Signal termination
    }

    // --- Destructor --- 
    // Called AFTER kill() completes and actor is removed.
    // RAII handles cleanup automatically here.
    ~ResourceManager() override {
        // _buffer unique_ptr goes out of scope -> delete[] _buffer
        // _data_file fstream goes out of scope -> _data_file.close()
        // _items vector goes out of scope -> frees memory
        // _connection wrapper goes out of scope -> _connection.close()
        std::cout << "Actor " << id() << ": All RAII resources released." << std::endl;
    }

    // ... methods using _buffer, _data_file, _items ...
};
```
**(Ref:** `test-actor-resource-management.cpp`**)

## 2. Explicit Cleanup in `on(KillEvent&)`

Sometimes, actions must occur *before* RAII cleanup happens in the destructor, often involving interaction with other actors or systems *while the actor is still technically running but stopping*.

*   **When to Use:**
    *   Notifying other actors of impending shutdown (e.g., telling a manager this worker is stopping).
    *   Explicitly closing network connections managed by raw pointers or non-RAII wrappers.
    *   Flushing critical buffers to persistent storage.
    *   Releasing external system locks or resources.
*   **How:**
    1.  Override `void on(const qb::KillEvent& event)`.
    2.  Perform the necessary cleanup actions.
    3.  **Crucially, end the handler by calling the base `kill()` method.** This signals the framework to proceed with the actual termination and eventual destruction.

```cpp
void MyServiceActor::on(const qb::KillEvent& event) {
    std::cout << "Service " << id() << " received KillEvent." << std::endl;

    // Notify dependent actors (if any)
    if (_client_id.is_valid()) {
        push<ServiceStoppingEvent>(_client_id);
    }

    // Explicitly close a manually managed resource (if not using RAII)
    if (_raw_socket != -1) {
        ::close(_raw_socket);
        _raw_socket = -1;
    }

    // --- IMPORTANT --- 
    kill(); // Proceed with termination
}
```
**(Ref:** `test-actor-lifecycle-hooks.cpp`**)

## 3. Referenced Actors (`addRefActor`)

*   The parent actor calling `addRefActor` gets a raw pointer but **does not own** the child actor.
*   The child actor must manage its own termination (`kill()`.
*   If the parent needs to ensure the child is terminated when the parent stops, the parent must explicitly send a `qb::KillEvent` to the child's `ActorId` (obtained from `child_ptr->id()`), typically within the parent's `on(KillEvent&)` handler or destructor.

## 4. `qb-io` Resource Management

*   I/O components like `qb::io::tcp::socket`, `tcp::listener`, `sys::file` manage their underlying handles (file descriptors/SOCKETs) using RAII within their own classes.
*   Asynchronous components inheriting from `qb::io::async::*` bases (often via `qb::io::use<>`) manage their event watchers (`ev::*`). Cleanup usually happens in their destructors or the `dispose()` method.
*   When an actor inherits from a `qb::io::use<>` base, the I/O component's lifetime is tied to the actor's. Termination of the actor (`kill()` followed by destruction) will trigger the cleanup of the associated I/O resources.
*   Explicitly call `close()` on the transport (`this->transport().close()`) within `on(KillEvent&)` if you need to ensure the connection is closed *before* notifying other actors, but RAII will handle it eventually during destruction anyway.

**(Ref:** `chat_tcp`, `message_broker`, `file_monitor`, `file_processor` examples show implicit cleanup via actor termination.**) 