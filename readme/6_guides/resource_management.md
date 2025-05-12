@page guides_resource_management_md QB Framework: Effective Resource Management
@brief Best practices for managing resources like memory, file handles, and network connections within QB actors, leveraging RAII and actor lifecycle hooks.

# QB Framework: Effective Resource Management

Proper resource management is crucial for building stable and leak-free applications. The QB Actor Framework leverages standard C++ RAII (Resource Acquisition Is Initialization) principles and the actor lifecycle to provide robust and largely automatic resource handling.

## 1. RAII: The Cornerstone of Resource Management in Actors

**RAII is the primary and strongly recommended approach for managing all resources within your QB actors.**

*   **How It Works:** You encapsulate resources (dynamically allocated memory, file handles, network sockets, database connections, mutexes, etc.) within C++ objects that manage the resource's lifetime. The resource is acquired in the object's constructor and automatically released/cleaned up in its destructor.
*   **Standard C++ RAII Tools:**
    *   **Memory:**
        *   `std::unique_ptr<T>`: For exclusive ownership of heap-allocated objects or arrays. Automatically calls `delete` or `delete[]`.
        *   `std::shared_ptr<T>`: For shared ownership. The resource is deleted when the last `shared_ptr` to it is destroyed.
        *   Standard Containers: `std::vector`, `std::string`, `std::map`, `qb::string<N>`, `qb::allocator::pipe<T>` all manage their own internal memory.
    *   **File Handles:**
        *   `std::fstream`, `std::ifstream`, `std::ofstream`: Automatically close the file when they go out of scope.
        *   `qb::io::sys::file`: QB's own file wrapper also closes the file descriptor in its destructor.
    *   **Network Sockets:**
        *   `qb::io::tcp::socket`, `qb::io::udp::socket`, `qb::io::tcp::ssl::socket`: These classes manage the underlying system socket descriptor and ensure it's closed upon destruction.
    *   **Locks (for external interactions, if unavoidable):**
        *   `std::lock_guard<std::mutex>`: Acquires a mutex on construction and releases it on destruction (when the guard goes out of scope).
        *   `std::unique_lock<std::mutex>`: Offers more flexible lock management, also ensuring release on destruction if still owned.

*   **Integration with Actor Lifecycle:**
    *   Declare RAII wrappers as member variables of your actor.
    *   Acquire resources in the actor's constructor or, more commonly, in its `onInit()` method (especially if acquisition can fail and you want to prevent the actor from starting).
    *   **Crucially, an actor's destructor (`~MyActor()`) is guaranteed to be called *after* the actor has fully terminated** (i.e., its `kill()` method has been processed and it's removed from the `VirtualCore`). This ensures that all RAII member objects are properly destructed, and thus their managed resources are released automatically and safely.

```cpp
#include <qb/actor.h>
#include <qb/event.h>
#include <qb/io.h>
#include <memory>    // For std::unique_ptr, std::shared_ptr
#include <fstream>   // For std::fstream
#include <vector>    // For std::vector
#include <qb/string.h> // For qb::string

// Hypothetical RAII wrapper for a network connection
class NetworkConnectionWrapper {
public:
    NetworkConnectionWrapper(const char* address) { /* connect */ qb::io::cout() << "NetworkConnectionWrapper: Connected to " << address << ".\n"; }
    ~NetworkConnectionWrapper() { /* disconnect */ qb::io::cout() << "NetworkConnectionWrapper: Disconnected.\n"; }
    void send_data(const qb::string<32>& data) { /* ... */ }
};

class ResourceManagerActor : public qb::Actor {
private:
    // --- Resources Managed by RAII --- 
    std::unique_ptr<int[]> _dynamic_array;      // Dynamically allocated array
    std::fstream _config_file;                  // File handle
    std::vector<qb::string<64>> _item_list;   // Container managing its own memory (and qb::string members)
    std::shared_ptr<NetworkConnectionWrapper> _shared_connection; // Shared network resource

public:
    ResourceManagerActor(const char* config_path, const char* shared_server_address) {
        // Constructor can initialize some members, or defer to onInit
        _dynamic_array = std::make_unique<int[]>(100); // Acquired at construction
        _shared_connection = std::make_shared<NetworkConnectionWrapper>(shared_server_address);
        qb::io::cout() << "ResourceManagerActor [" << id() << "] constructed.\n";
    }

    bool onInit() override {
        _config_file.open("actor_settings.conf", std::ios::in);
        if (!_config_file.is_open()) {
            qb::io::cout() << "ResourceManagerActor [" << id() << "]: Failed to open config file!\n";
            return false; // Initialization failed, actor won't start, destructor will clean up _dynamic_array & _shared_connection
        }
        _item_list.reserve(50);
        qb::io::cout() << "ResourceManagerActor [" << id() << "]: Initialized successfully. Config file opened.\n";
        registerEvent<qb::KillEvent>(*this);
        return true;
    }

    void on(const qb::KillEvent& /*event*/) {
        qb::io::cout() << "ResourceManagerActor [" << id() << "]: KillEvent received. Shutting down.\n";
        // Optional: Perform actions needed *before* RAII takes over in destructor.
        // e.g., if _config_file was open for writing: _config_file.flush();
        kill(); // Signal framework to proceed with termination
    }

    // --- Destructor --- 
    // Called by the framework AFTER kill() completes and the actor is removed from the VirtualCore.
    // All RAII members will have their destructors called automatically here.
    ~ResourceManagerActor() override {
        // _dynamic_array (unique_ptr) goes out of scope -> delete[] is called.
        // _config_file (fstream) goes out of scope -> _config_file.close() is called if open.
        // _item_list (vector) goes out of scope -> its memory is freed.
        // _shared_connection (shared_ptr) decrements ref count; resource freed if it was the last owner.
        qb::io::cout() << "ResourceManagerActor [" << id() << "]: Destructor called. All RAII resources released.\n";
    }

    // ... other methods using these resources ...
};
```
**(Reference Example:** `test-actor-resource-management.cpp` demonstrates various resource acquisition and release scenarios.)**

## 2. Explicit Cleanup in `on(const qb::KillEvent&)`

While RAII is preferred for automatic cleanup, sometimes an actor needs to perform specific actions *during its shutdown sequence but before its destructor is called*.

*   **When to Use This Pattern:**
    *   Notifying other actors that this actor is shutting down (e.g., unregistering from a manager, releasing a lease).
    *   Explicitly closing network connections or flushing buffers if these operations need to complete *before* other system-wide shutdown signals are processed or before the actor object itself is destroyed.
    *   Releasing external system resources (e.g., hardware locks, COM objects) that are not managed by C++ RAII wrappers.
*   **How to Implement:**
    1.  Ensure your actor is registered to handle `qb::KillEvent` (this is done by default by the `qb::Actor` constructor, but good to be aware of).
    2.  Override `void on(const qb::KillEvent& event)`.
    3.  Perform your specific cleanup actions within this handler.
    4.  **Crucially, end your `on(const qb::KillEvent&)` handler by calling the base `this->kill()` method.** This signals the framework to proceed with the actor's termination, eventually leading to its destructor call.

```cpp
// Inside an actor that manages a subscription with a remote service
// Assume _service_manager_id is the ActorId of a service it's registered with.

// void on(const qb::KillEvent& event) {
//     qb::io::cout() << "MySubscriberActor [" << id() << "]: Received KillEvent. Unsubscribing...\n";
// 
//     // Notify the service manager that this actor is stopping
//     if (_service_manager_id.is_valid()) {
//         push<UnsubscribeMeEvent>(_service_manager_id, id());
//     }
// 
//     // If holding a raw socket or non-RAII resource explicitly:
//     // if (_raw_connection_handle != INVALID_HANDLE) {
//     //     close_raw_connection(_raw_connection_handle);
//     //     _raw_connection_handle = INVALID_HANDLE;
//     // }
// 
//     this->kill(); // Essential: proceed with framework-managed termination
// }
```
**(Reference Example:** `test-actor-lifecycle-hooks.cpp` shows various hooks, including `on(KillEvent&)`.)**

## 3. Resource Management for Referenced Actors (`addRefActor`)

*   When an actor (parent) creates a child actor using `addRefActor<ChildType>(...)`, the parent receives a raw pointer to the child but **does not own it** in the C++ sense (i.e., the parent's destructor will not automatically delete the child).
*   The referenced child actor is responsible for its own lifecycle and must terminate itself via `kill()` or be killed by an event.
*   If the parent actor needs to ensure its referenced children are terminated when the parent itself stops, the parent should explicitly send `qb::KillEvent`s to the `ActorId`s of its referenced children. This is typically done in the parent's `on(const qb::KillEvent&)` handler or, less commonly, its destructor (though sending messages from destructors is generally discouraged if it relies on other actors still being fully operational).

## 4. `qb-io` Component Resource Management

*   **Automatic Cleanup:** I/O components from `qb-io` (like `qb::io::tcp::socket`, `qb::io::tcp::listener`, `qb::io::sys::file`) are designed with RAII. They manage their underlying system handles (file descriptors, SOCKETs) and close them automatically upon destruction.
*   **Actor Integration (`qb::io::use<>`):** When an actor inherits from `qb::io::use<>` base classes (e.g., `qb::io::use<MyClient>::tcp::client`), the networking transport component (which contains the socket) is typically a member of the `use<>` base. Its lifetime becomes tied to the actor's lifetime. When the actor is destroyed, the `use<>` base class member (and thus the transport and its socket) is also destroyed, ensuring resources are released.
*   **SSL Contexts (`SSL_CTX*`):** If you manually create `SSL_CTX*` objects (e.g., using `qb::io::ssl::create_client_context` or `create_server_context`), **you are responsible for freeing them using `SSL_CTX_free()`** when they are no longer needed. This is often done in the destructor of the actor that owns the `SSL_CTX*`.
*   **Explicit `close()`/`disconnect()`:** While RAII handles eventual cleanup, you might call `this->transport().close()` or `this->transport().disconnect()` explicitly in your actor's `on(KillEvent&)` handler if you need to ensure the network connection is shut down *before* other cleanup actions or notifications occur.

By consistently applying RAII and understanding the actor lifecycle, you can effectively manage resources and prevent leaks in your QB applications.

**(Next:** Review the `[QB Framework: Error Handling & Resilience Strategies](./error_handling.md)` for strategies on building fault-tolerant actor systems.**) 