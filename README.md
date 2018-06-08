# Cube Framework

Cube provides technology solutions and services dedicated to high performance real-time complex processing, enabling low and predictable latency, perfect scalability, high throughput and hardware footprint savings. 
We provide a complete development framework for multicore processing that has been specifically designed for low latency and low footprint on multicore processors. 

Cube is a thin-layer multicore-optimized runtime that enable users to build their own business-driven, jitter-free, low-latency, and elastic Reactive software based on the Actor model. 

* #### Requirements
  - C++ standard (17) compiler
  - (Recommended) CMake
  - (Recommended) Disable the HyperThreading to optimize your Physical Cores Cache
* #### Pros
  - Multi plateforme (Linux|Windows|Apple)
  - Compile with a specific hardware configuration
  - CPU cache friendly
  - Very fast and low-latency
  - Easy reusbale code from a project to another
  - Forget everything about multithreading concurrency issues
* #### Coins
  - Almost Everything is deduced at compile time, then it can increase the compilation time
  - Strong CPU usage
  - ...
# Actor Model
Our CPUs are not getting any faster. What’s happening is that we now have multiple cores on them. If we want to take advantage of all this hardware we have available now, we need a way to run our code concurrently. Decades of untraceable bugs and developers’ depression have shown that threads are not the way to go.

- The Actor model is a concurrent model of computation that treats "actors" as the universal primitives of concurrent computation. Hence, an Actor is the main structural element of any actor model. By definition, an Actor sends event messages to be received by another Actor, which is then treated by an Event handler.

- The Event handler can execute a local function, create more actors, and send events to other Actors. In Cube programming semantics, Actors shall be mono-threaded and non-blocking.

- The Event communication between two Actors is done with a dedicated unidirectional communication channel called a Pipe. Hence, the Actor programming model is completely asynchronous and event-driven. 

- A Workflow is a program consisting of multiple Actors linked together with several Pipes and handling one or multiple Events. A Workflow solves one particular business problem. Once the Workflow skeleton is designed, the programming is broken down into coding mono-threaded and sequential Event handlers.

- The business use cases are described as combinations of workflows using the Actor model which is scalable and parallel by nature.
Cube runtime will handle all the rest and bridge the gap between parallel programming and hardware multicore complexity.

# Getting Started !
* ##### My First Project
CMakeList.txt
```cmake
project(MyProject)
# Cube minimum requirements
cmake_minimum_required(VERSION 3.10)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add cube framework
add_subdirectory([CUBE_PATH])
include_directories([CUBE_PATH])

# Define your project
# Multiple hardware configuration example
if (INTEL_CORE_2_DUO)
    set(SOURCE "$(SOURCE) main_intel_2core.cpp)
elseif(INTEL_I7_8CORE)
    set(SOURCE "$(SOURCE) main_intel_8core.cpp)
# etc...
else()
    set(SOURCE "$(SOURCE) main.cpp)
endif()

add_executable(MyProject $(SOURCE))
# Link with cube
target_link_libraries(MyProject cube)
```
MyActor.h
```cpp
#include "cube.h"
#ifndef MYACTOR_H_
# define MYACTOR_H_

struct MyEvent { int data; }; // Event example

template <typename CoreHandler>
class MyActor
        : public cube::Actor<CoreHandler> {
public:
    MyActor() = default;
    // (optional) implementation if has to check initialization
    cube::ActorStatus init() {
        // register MyEvent
        this->template registerEvent<MyEvent> (*this);
        // send MyEvent to myself, forever alone ;(
        auto &event = this->tempalte push<MyEvent>(this->id()); // and keep a reference to the event
        event.data = 1337; // set data to send
        return cube::ActorStatus::Alive; // everything's ok actor will be added to engine
    }
    // (optional) implementation if actor has a looped callback
    cube::ActorStatus main() {
        // I am a dummy actor, notify the engine to remove me !
        return cube::ActorStatus::Dead;
    }
    // this is the callback when MyActor received 
    void onEvent(MyEvent const &event) {
        // ...
    }
};

#endif
```
main.cpp
```cpp
#include "MyActor.h"

int main () {
    // (optional) initialize the logger
    nanolog::initialize(nanolog::GuaranteedLogger(), "./log/", "serge-challenge.log", 1024);
    nanolog::set_log_level(nanolog::LogLevel::WARN);

    // configure the Main Engine 
    // Note : I will use only the core 0 and 1 
    cube::Main<PhysicalCore<0>, PhysicalCore<1>> main;
    
    // My start sequence -> add MyActor to core 0 and 1
    main.addActor<0, MyActor>();
    main.addActor<1, MyActor>();

    main.start();  // start the engine
    main.join();   // Wait for the running engine
    // if all my actors had been destroyed then it will release the wait !
    return 0;
}
```
Let's compile MyProject !
```sh
$> cmake
$> make
```
Done !

You want to do more, refer to the wiki to see the full Cube API usage 

### Wiki
*  [Configuration]() - cmake options
*  [Main]() - Framework Initializer
*  [Actor]() - User defined Object API
*  [Event]() - Actor communication API
*  [Log]() 

### Todos
 - Make Wiki Documentation
 - Use Google Test
 - Add Examples
 - Add Debug metrics report
 - Add PhysicalCore throughtput to manage the cpu usage

License
----

MIT

**ISNDEV Free Software, Hell Yeah!**
