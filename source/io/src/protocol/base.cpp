/**
 * @file qb/io/src/protocol/base.cpp
 * @brief Implementation of base protocol functionality
 *
 * This file contains the implementation of the base protocol classes and interfaces
 * in the QB framework. It provides foundational functionality for all protocol
 * implementations, including common methods for header parsing, frame decoding,
 * and message handling.
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
 * @ingroup IO
 */

#include <qb/io/async.h>
#include <qb/io/protocol/base.h>
