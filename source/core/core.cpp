/**
 * @file qb/core/core.cpp
 * @brief Core implementation of the QB Actor Framework
 *
 * This file contains the central implementation of the QB Actor Framework's core
 * functionality, including actor lifecycle management, event routing, and message
 * passing mechanics. It provides the runtime foundations for the entire actor-based
 * concurrency model.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2025 qb - isndev (cpp.actor)
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
 * limitations under the License.
 * @ingroup Core
 */

#include "src/ActorId.cpp"
#include "src/VirtualCore.cpp"
#include "src/Actor.cpp"
#include "src/CoreSet.cpp"
#include "src/Main.cpp"