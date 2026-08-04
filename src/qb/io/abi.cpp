/**
 * @file qb/io/abi.cpp
 * @brief The archive half of the link-time configuration fingerprint.
 *
 * @details
 * `qb/utility/abi.h` makes every consumer translation unit *reference* one symbol per ABI axis,
 * named after the value that translation unit is compiled with. This file *defines* those
 * symbols with the names the **archive** was compiled with. Matching configuration resolves;
 * differing configuration is an undefined symbol at link. See `qb/utility/abi.h` for which axes
 * are in the fingerprint, and why each one is or is not.
 *
 * It lives in **qb-io**, not qb-core, because two of the fingerprinted axes reach consumers that
 * never touch qb-core: `QB_LOCKFREE_CACHELINE_BYTES` sets `CoroutineFrameAllocator::kAlign`
 * (`qb/io/async/coroutine/task.h`) and `QB_DEBUG_COROUTINES` grows `task<T>::promise_type`. Every
 * qb consumer links qb-io -- `qb::core` names `qb::io` in its own `INTERFACE_LINK_LIBRARIES` --
 * so a definition here is reachable from every consumer, while one in qb-core would not be.
 *
 * The five objects are one byte each and are never read; only their *names* carry information.
 * `qb_abi_fingerprint` is the same information as text, so that
 * `strings libqb-io.a | grep '^qb-abi '` answers "how was this archive built?" without a
 * demangler, a symbol table, or a qb checkout.
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

#include <qb/utility/abi.h>

// Each name expands to the archive's own axis value, e.g. `qb_abi_cacheline_64`. The `extern "C"`
// declarations in qb/utility/abi.h are what give these definitions external linkage -- a const
// object at namespace scope would otherwise be internal.
extern "C" {
const char QB_ABI_SYM_VERSION    = 0;
const char QB_ABI_SYM_CACHELINE  = 0;
const char QB_ABI_SYM_EXCEPTIONS = 0;
const char QB_ABI_SYM_CORO_DEBUG = 0;
const char QB_ABI_SYM_STD_JTHREAD = 0;
const char qb_abi_fingerprint[]  = QB_ABI_FINGERPRINT_TEXT;
}
