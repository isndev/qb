#
# qb - C++ Actor Framework
# Copyright (C) 2011-2020 isndev (www.qbaf.io). All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#         http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
#         limitations under the License.
#
# CMakeLists.txt file
cmake_minimum_required(VERSION 3.10)
project(qb-sample-client-text)

# qb minimum requirements
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SOURCE
        src/ClientActor.cpp
        src/CmdActor.cpp
        main.cpp)

add_executable(${PROJECT_NAME} ${SOURCE})
# Add include directory
target_include_directories(${PROJECT_NAME} PRIVATE include)

# Link project with qb-core library
target_link_libraries(${PROJECT_NAME} qb-core)