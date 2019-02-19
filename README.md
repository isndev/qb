# Cube Framework

Cube provides technology solutions and services dedicated to high performance real-time complex processing, enabling low and predictable latency, perfect scalability and high throughput. 
It's a complete development framework for multicore processing that has been specifically designed for low latency and low footprint on multicore processors. 

Cube is a thin-layer multicore-optimized runtime that enable users to build their own business-driven, jitter-free, low-latency, and elastic Reactive software based on the Actor model.

* #### Requirements
  - C++17 standard, (gcc7, clang4, msvc19.11)
  - (Recommended) CMake
  - (Recommended) Disable the HyperThreading to optimize your Physical Cores Cache
* #### Pros
  - Multi platform (Linux|Windows|Apple)
  - Easy to use
  - CPU cache friendly
  - Very fast and low-latency
  - Reusable code from a project to another
  - Forget everything about multi-threading concurrency issues
* #### Coins
  - Strong CPU usage
  - ...
  
# Actor Model
#### Introduction
Our CPUs are not getting any faster. What’s happening is that we now have multiple cores on them. If we want to take advantage of all this hardware we have available now, we need a way to run our code concurrently. Decades of untraceable bugs and developers’ depression have shown that threads are not the way to go.

#### Definition
The Actor model is a concurrent model of computation that treats "actors" as the universal primitives of concurrent computation.  
- The Actor sends event messages to be received by another Actor, which is then treated by an Event handler.
- The Event handler can execute a local function, create more actors, and send events to other Actors. In Cube programming semantics, Actors shall be mono-threaded and non-blocking.
- The Event communication between Actors is done with an unidirectional communication channel called a Pipe. Hence, the Actor programming model is completely asynchronous and event-driven. 

<p align="center"><img src="./ressources/BasicActorModel.png" width="500px" /></p>

#### Cube + Actor Model
A program developed with Cube is consisting of multiple Actors handling one or multiple Events, attached to PhysicalCores linked together with several Pipes.  
Once designed, the programming is broken down into coding mono-threaded and sequential Event handlers.  
Hence, the Actor model which is scalable and parallel by nature.  

Cube runtime will handle all the rest and bridge the gap between parallel programming and hardware multicore complexity.

# Getting Started !
* #### My First Project
CMakeList.txt
```cmake
cmake_minimum_required(VERSION 3.10)
project(MyProject)

# Cube minimum requirements
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add cube framework
add_subdirectory([CUBE_PATH])
include_directories([CUBE_PATH])

# Define your project source
set(SOURCE main.cpp)

add_executable(MyProject ${SOURCE})
# Link with cube
target_link_libraries(MyProject cube)
```
MyActor.h
```cpp
#include "actor.h"
#ifndef MYACTOR_H_
# define MYACTOR_H_

// Event example
struct MyEvent
 : public cube::Event // /!\ should inherit from cube event
{
    int data; // trivial data
    std::vector<int> container; // dynamic data
    // std::string str; /!\ avoid using stl string
    // instead use fixed cstring
    // or compile with old ABI '-D_GLIBCXX_USE_CXX11_ABI=0'
}; 

class MyActor
        : public cube::Actor // /!\ should inherit from cube actor
        , public cube::ICallback // (optional) required to register actor callback
{
public:
    MyActor() = default;
    MyActor(int a, int b) {} // constructor with parameters
    
    // will call this function before adding MyActor
    bool onInit() override final {
        this->template registerEvent<MyEvent> (*this);          // will listen MyEvent
        this->registerCallback(*this);                          // each core loop will call onCallback

        // ex: just send MyEvent to myself ! forever alone ;(
        auto &event = this->template push<MyEvent>(this->id()); // and keep a reference to the event
        event.data = 1337;                                      // set trivial data
        event.container.push(7331);
        return true;                                            // init ok, MyActor will be added
    }
    
    // will call this function each core loop
    void onCallback() override final {
        // ...
    }
    
    // will call this function when MyActor received MyEvent 
    void on(MyEvent const &event) {
        // I am a dummy actor, notify the engine to remove me !
        this->kill();
    }
};

#endif
```
main.cpp
```cpp
#include "cube.h"
#include "MyActor.h"

int main (int argc, char *argv[]) {
    // (optional) initialize the logger
    cube::io::log::init("./", argv[0]); // directory, filename
    cube::io::log::setLevel(cube::io::log::Level::WARN); // log only warning an critical
    // usage
    LOG_INFO << "I will not be logged :(";

    // configure the Engine 
    // Note : I will use only the core 0 and 1 
    cube::Cube main({0, 1});
    
    // My start sequence -> add MyActor to core 0 and 1
    main.addActor<MyActor>(0); // default constructed
    main.addActor<MyActor>(1, 1337, 7331); // constructed with parameters

    main.start();  // start the engine asynchronously
    main.join();   // Wait for the running engine
    // if all my actors had been destroyed then it will release the wait !
    return 0;
}
```
Let's compile MyProject !
```sh
$> cmake -DCMAKE_BUILD_TYPE=Release -B[Build Directory Path] -H[CMakeList.txt Path]
$> make
```
Done !

You want to do more, refer to the wiki to see the full Cube API usage 

### [Wiki](https://github.com/isndev/cube/wiki)
*  [Build](https://github.com/isndev/cube/wiki/Build-Options) - Build Options
*  [Start Sequence](https://github.com/isndev/cube/wiki/Start-Sequence) - Engine Initializer
*  [Actor](https://github.com/isndev/cube/wiki/Actor) - Actor Interface
*  [Logger](https://github.com/isndev/cube/wiki/Logger) - Fast Multithreaded Logger

### Todos
  - [ ] Make Wiki Documentation (37%)
  - [ ] Use Google Test (0%)
  - [ ] Add Examples (0%)
  - [ ] Add Debug metrics report (0%)
  - [ ] Add PhysicalCore throughtput to manage the cpu usage (0%)

License
----

MIT

**isndev Free Software, Hell Yeah!**
