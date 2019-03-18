/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2019 isndev (www.qbaf.io). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 *         limitations under the License.
 */

/*!
 * @mainpage Home
 * <p align="center"><img src="./logo.png" width="180px" ></p>
 * <br>
 * Welcome to the official **qb** documentation. Here you will find a detailed
 * view of all the **qb** classes, functions and modules. <br>
 *
 * **qb** provides technology solutions and services dedicated to high performance real-time complex processing, enabling low and predictable latency, perfect scalability and high throughput.
 * It's a complete development framework for multicore processing that has been specifically designed for low latency and low footprint on multicore processors.
 *
 * **qb** is a thin-layer multicore-optimized runtime that enable users to build their own business-driven, jitter-free, low-latency, and elastic Reactive software based on the Actor model.
 *
 * #### Requirements
 *   - C++17 compiler, (gcc7, clang4, msvc19.11)
 *   - (Recommended) cmake
 *   - (Recommended) Disable the HyperThreading to optimize your Physical Cores Cache
 *   - Build Status
 *     |              | linux | Windows | Coverage |
 *     |:------------:|:-----:|:-------:|:--------:|
 *     |    master    | ![Build Status](https://travis-ci.org/isndev/qb.svg?branch=master) | ![Build status](https://ci.appveyor.com/api/projects/status/aern7ygl63wa3c9b/branch/master?svg=true) | ![Codecov branch](https://img.shields.io/codecov/c/github/isndev/qb/master.svg) |
 *     |    develop   | ![Build Status](https://travis-ci.org/isndev/qb.svg?branch=develop) | ![Build status](https://ci.appveyor.com/api/projects/status/aern7ygl63wa3c9b/branch/develop?svg=true) | ![Codecov branch](https://img.shields.io/codecov/c/github/isndev/qb/develop.svg) |
 *     | experimental | ![Build Status](https://travis-ci.org/isndev/qb.svg?branch=experimental) | ![Build status](https://ci.appveyor.com/api/projects/status/aern7ygl63wa3c9b/branch/experimental?svg=true) | ![Codecov branch](https://img.shields.io/codecov/c/github/isndev/qb/experimental.svg) |
 *
 *
 * #### Pros
 *   - Opensource
 *   - Cross-platform (Linux|Windows)
 *   - Easy to use
 *   - CPU cache friendly
 *   - Very fast and low-latency
 *   - Reusable code from a project to another
 *   - Forget everything about multi-threading concurrency issues
 *
 * #### Coins
 *   - Strong CPU usage
 *   - ...
 *
 * #### License
 *   - Apache Version 2
 *
 * @section Introduction
 * Our CPUs are not getting any faster. What’s happening is that we now have multiple cores on them. If we want to take advantage of all this hardware we have available now, we need a way to run our code concurrently. Decades of untraceable bugs and developers’ depression have shown that threads are not the way to go.
 *
 * #### Definition
 * The Actor model is a concurrent model of computation that treats "actors" as the universal primitives of concurrent computation.
 * - The Actor sends event messages to be received by another Actor, which is then treated by an Event handler.
 * - The Event handler can execute a local function, create more actors, and send events to other Actors. In **qb** programming semantics, Actors shall be mono-threaded and non-blocking.
 * - The Event communication between Actors is done with an unidirectional communication channel called a Pipe. Hence, the Actor programming model is completely asynchronous and event-driven.
 *
 * <p align="center"><img class="center" src="./BasicActorModel.png" width="500px" ></p>
 *
 * #### qb + Actor Model
 * A **program** developed with **qb** is consisting of multiple **actors** handling one or multiple **events** attached to several **cores** linked together with several **pipes**.
 * Once designed, the programming is broken down into coding **mono-threaded** and sequential event handlers.
 * Hence, the Actor model which is scalable and parallel by nature.
 *
 * **qb** runtime will handle all the rest and bridge the gap between parallel programming and hardware multicore complexity.
 *
 * @section GetStarted Getting Started !
 * #### Example ping-pong project
 *
 * - First, you'll have to create the project directory and cd into it
 *
 * ```bash
 * $> mkdir pingpong && cd pingpong
 * ```
 * - Then clone the **qb** framework by doing:
 *
 * ```bash
 * $> git clone git@github.com:isndev/qb.git
 * ```
 * - Next, create CMakeLists.txt file and paste the content below
 *
 * ```cmake
 * # CMakeLists.txt file
 * cmake_minimum_required(VERSION 3.10)
 * project(pingpong)
 *
 * # qb minimum requirements
 * set(CMAKE_CXX_STANDARD 17)
 * set(CMAKE_CXX_STANDARD_REQUIRED ON)
 * set(QB_PATH "${CMAKE_CURRENT_SOURCE_DIR}/qb")
 *
 * # Add qb framework
 * add_subdirectory(${QB_PATH})
 *
 * # Define your project source
 * set(SOURCE main.cpp)
 *
 * add_executable(pingpong ${SOURCE})
 * # Link target with qb-core library
 * target_link_libraries(pingpong qb-core)
 * ```
 *
 *
 * - Define your first event with its custom data <br>
 *   MyEvent.h :
 *
 * ```cpp
 * // MyEvent.h
 * #include <vector>
 * #include <qb/event.h>
 * #ifndef MYEVENT_H_
 * # define MYEVENT_H_
 * // Event example
 * struct MyEvent
 *  : public qb::Event // /!\ should inherit from qb event
 * {
 *     int data; // trivial data
 *     std::vector<int> container; // dynamic data
 *     // /!\ an event must never store an address of it own data
 *     // /!\ ex : int *ptr = &data;
 *     // /!\ avoid using std::string, instead use :
 *     // /!\ - fixed cstring
 *     // /!\ - pointer of std::string
 *     // /!\ - or compile with old ABI '-D_GLIBCXX_USE_CXX11_ABI=0'
 * };
 * #endif
 * ```
 *
 * - Let's define the PingActor <br>
 *   PingActor will send MyEvent to PongActor, receive the response and kill himself <br>
 *   PingActor.h :
 *
 * ```cpp
 * // PingActor.h file
 * #include <qb/actor.h>
 * #include "MyEvent.h"
 * #ifndef PINGACTOR_H_
 * # define PINGACTOR_H_
 *
 * class PingActor
 *         : public qb::Actor // /!\ should inherit from qb actor
 * {
 *     const qb::ActorId _id_pong; // Pong ActorId
 * public:
 *     PingActor() = delete; // PingActor requires PongActor Actorid
 *     // /!\ never call any qb::Actor functions in constructor
 *     // /!\ use onInit function
 *     explicit PingActor(const qb::ActorId id_pong)
 *       : _id_pong(id_pong) {}
 *
 *     // /!\ the engine will call this function before adding PingPongActor
 *     bool onInit() override final {
 *         registerEvent<MyEvent>(*this);         // will listen MyEvent
 *         auto &event = push<MyEvent>(_id_pong); // push MyEvent to PongActor and keep a reference to the event
 *         event.data = 1337;                     // set trivial data
 *         event.container.push_back(7331);       // set dynamic data
 *
 *         // debug print
 *         qb::io::cout() << "PingActor id(" << id() << ") has sent MyEvent" << std::endl;
 *         return true;                           // init ok
 *     }
 *     // will call this function when PingActor receives MyEvent
 *     void on(MyEvent &event) {
 *         // debug print
 *         qb::io::cout() << "PingActor id(" << id() << ") received MyEvent" << std::endl;
 *         kill(); // then notify engine to kill PingActor
 *     }
 * };
 *
 * #endif
 * ```
 *
 * - Let's define the PongActor <br>
 *   PongActor will just listen on MyEvent, reply the event and kill himself <br>
 *   PongActor.h :
 *
 * ```cpp
 * // PongActor.h file
 * #include <qb/actor.h>
 * #include "MyEvent.h"
 * #ifndef PONGACTOR_H_
 * # define PONGACTOR_H_
 *
 * class PongActor
 *         : public qb::Actor // /!\ should inherit from qb actor
 * {
 * public:
 *     // /!\ never call any qb::Actor functions in constructor
 *     // /!\ use onInit function
 *     PongActor() = default;
 *
 *     // /!\ the engine will call this function before adding PongActor
 *     bool onInit() override final {
 *         registerEvent<MyEvent>(*this);         // will just listen MyEvent
 *
 *         return true;                           // init ok
 *     }
 *     // will call this function when PongActor receives MyEvent
 *     void on(MyEvent &event) {
 *         // debug print
 *         qb::io::cout() << "PongActor id(" << id() << ") received MyEvent" << std::endl;
 *         reply(event); // reply the event to SourceActor
 *         // debug print
 *         qb::io::cout() << "PongActor id(" << id() << ") has replied MyEvent" << std::endl;
 *         kill(); // then notify engine to kill PongActor
 *     }
 * };
 *
 * #endif
 * ```
 *
 * - Then finally create the main.cpp
 *
 * ```cpp
 * // main.cpp file
 * #include <qb/main.h>
 * #include "PingActor.h"
 * #include "PongActor.h"
 *
 * int main (int argc, char *argv[]) {
 *     // (optional) initialize the qb logger
 *     qb::io::log::init(argv[0]); // filename
 *
 *     // configure the Engine
 *     // Note : I will use only the core 0 and 1
 *     qb::Main main({0, 1});
 *
 *     // Build Pong Actor to core 0 and retrieve its unique identifier
 *     auto id_pong = main.addActor<PongActor>(0); // default constructed
 *     // Build Ping Actor to core 1 with Pong ActorId as parameter
 *     main.addActor<PingActor>(1, id_pong); // constructed with parameters
 *
 *     main.start();  // start the engine asynchronously
 *     main.join();   // wait for the running engine
 *     // if all my actors had been destroyed then it will release the wait
 *     return 0;
 * }
 * ```
 *
 * Let's compile the project !
 * ```sh
 * $> cmake -DCMAKE_BUILD_TYPE=Release -B[Build Directory Path] -H[CMakeList.txt Path]
 * $> cd [Build Directory Path] && make
 * ```
 * Run it
 * ```sh
 * $> ./pingpong
 * ```
 * it should print
 * ```
 * PingActor id(XXXXXX) has sent MyEvent
 * PongActor id(XXXXXX) received MyEvent
 * PongActor id(XXXXXX) has replied MyEvent
 * PingActor id(XXXXXX) received MyEvent
 * ```
 * Done !
 */