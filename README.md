# Cube Framework

Cube provides technology solutions and services dedicated to high performance real-time complex processing, enabling low and predictable latency, perfect scalability and high throughput. 
It's a complete development framework for multicore processing that has been specifically designed for low latency and low footprint on multicore processors. 

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
#### Introduction
Our CPUs are not getting any faster. What’s happening is that we now have multiple cores on them. If we want to take advantage of all this hardware we have available now, we need a way to run our code concurrently. Decades of untraceable bugs and developers’ depression have shown that threads are not the way to go.

The Actor model is a concurrent model of computation that treats "actors" as the universal primitives of concurrent computation.

#### Workflow
- The Actor sends event messages to be received by another Actor, which is then treated by an Event handler.
- The Event handler can execute a local function, create more actors, and send events to other Actors. In Cube programming semantics, Actors shall be mono-threaded and non-blocking.
- The Event communication between Actors is done with an unidirectional communication channel called a Pipe. Hence, the Actor programming model is completely asynchronous and event-driven. 

<p align="center"><img src="./ressources/BasicActorModel.png" width="500px" /></p>

- Cube Workflow is a program consisting of multiple Actors handling one or multiple Events, attached to PhysicalCores linked together with several Pipes. It solves one particular problem, once the Workflow skeleton is designed, the programming is broken down into coding mono-threaded and sequential Event handlers.  
Hence, the Actor model which is scalable and parallel by nature.

Cube runtime will handle all the rest and bridge the gap between parallel programming and hardware multicore complexity.

# Getting Started !
* #### My First Project
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
    set(SOURCE "$(SOURCE) main_intel_2core.cpp")
elseif (INTEL_I7_8CORE)
    set(SOURCE "$(SOURCE) main_intel_8core.cpp")
# etc...
else()
    set(SOURCE "$(SOURCE) main.cpp")
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

// Event example
struct MyEvent : public cube::Event
{ int data; }; 

template <typename CoreHandler>
class MyActor
        : public cube::Actor<CoreHandler>
        , public CoreHandler::ICallback {
public:
    MyActor() = default;
    // will call this function before adding MyActor
    bool onInit() override final {
        this->template registerEvent<MyEvent> (*this);          // will listen MyEvent
        this->registerCallback(*this);                          // each core loop, call onCallback

        // send MyEvent to me ! forever alone ;(
        auto &event = this->template push<MyEvent>(this->id()); // and keep a reference to the event
        event.data = 1337;                                      // set data
        return true;                                            // init ok, MyActor will be added
    }
    
    // will call this function each core loop
    void onCallback() override final {
        // I am a dummy actor, notify the engine to remove me !
        this->kill();
    }
    
    // will call this function when MyActor received MyEvent 
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
    nanolog::initialize(nanolog::GuaranteedLogger(), "./log/", "MyProject.log", 1024);
    nanolog::set_log_level(nanolog::LogLevel::WARN); // log only warning an critical

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

### [Wiki](https://github.com/isnDev/cube/wiki)
*  [Build](https://github.com/isnDev/cube/wiki/Build-Options) - Build Options
*  [Start Sequence](https://github.com/isnDev/cube/wiki/Start-Sequence) - Engine Initializer
*  [Actor](https://github.com/isnDev/cube/wiki/Actor) - Actor Interface
*  [Logger](https://github.com/isnDev/cube/wiki/Logger) - Fast Multithreaded Logger

### Todos
  - [ ] Make Wiki Documentation (33%)
  - [ ] Use Google Test (0%)
  - [ ] Add Examples (0%)
  - [ ] Add Debug metrics report (0%)
  - [ ] Add PhysicalCore throughtput to manage the cpu usage (0%)

License
----

MIT

**ISNDEV Free Software, Hell Yeah!**
