/**
 * @file cube.h
 * @brief Main include file for the QB Actor Framework
 *
 * This is the main include file for the QB Actor Framework, which provides
 * a simple way to include the entire framework in a project. It includes
 * all necessary headers and definitions for using the framework.
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
 */

#ifndef QB_QB_H
#define QB_QB_H

/**
 * @mainpage QB Actor Framework
 *
 * @section intro Introduction
 *
 * QB is a C++ Actor Framework designed for high-performance concurrent
 * and distributed applications. It provides a comprehensive set of tools
 * for building systems based on the Actor Model paradigm.
 *
 * The framework consists of several modules:
 * - Core: The fundamental actor system components
 * - IO: Input/output operations and network communication
 * - System: System-level facilities and abstractions
 * - Utility: General-purpose utility libraries
 *
 * @section usage Usage
 *
 * To use the framework, include this header file:
 *
 * ```cpp
 * #include <qb/cube.h>
 * ```
 *
 * Or include the specific components you need:
 *
 * ```cpp
 * #include <qb/actor.h>
 * #include <qb/io.h>
 * ```
 */

// Include main components
#include <qb/actor.h>
#include <qb/io.h>
#include <qb/main.h>

/*!
 * @defgroup Core Core
 * @defgroup IO IO
 * @defgroup TCP TCP
 * @ingroup IO
 * @defgroup UDP UDP
 * @ingroup IO
 */

#endif // QB_QB_H
