/**
 * @file qb/io/src/protocol/text.cpp
 * @brief Implementation of text-based protocol handlers
 *
 * This file contains the implementation of text-based protocol parsing and handling
 * in the QB framework. It provides functionality for processing line-based and
 * character-based protocols such as HTTP, SMTP, or custom text protocols.
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
#include <qb/io/protocol/text.h>