/**
 * @file qb/io/async/listener.cpp
 * @brief Implementation of asynchronous network listener functionality
 *
 * This file contains the implementation of asynchronous network listener operations
 * in the QB framework. It provides the functionality for accepting incoming connections
 * without blocking, using platform-specific event notification mechanisms to efficiently
 * handle multiple connection attempts.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
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

#include <qb/io/async/listener.h>

namespace qb::io::async {

// `thread_local listener listener::current = {};` used to live HERE, and that is the whole
// content this translation unit ever had. It moved to `qb/io/async/listener.h` in 3.0.0, as an
// `inline` definition annotated `QB_ABI_ANCHOR`, and it must not come back: an out-of-line
// definition emits a `non-external` TLS descriptor, so a host executable and a plugin that each
// statically link `libqb-io.a` get *two* `listener::current` on the same thread and everything
// the plugin registers goes into a loop nobody runs. Measured, both dlopen modes, no diagnostic
// from any tool. The definition site in the header carries the numbers and the symbol-table
// evidence.
//
// The file itself stays: it is a member of `libqb-io.a` (the amalgamation includes it), and it is
// where a future non-inline `listener` member belongs.

} // namespace qb::io::async
