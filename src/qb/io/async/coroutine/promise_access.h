/**
 * @file qb/io/async/coroutine/promise_access.h
 * @brief The two conversions between a coroutine's promise and its handle, correct for an
 *        OVER-ALIGNED promise on every toolchain qb builds with.
 *
 * `std::coroutine_handle<P>::from_promise(p)` and `handle.promise()` are the standard way in and
 * out of a promise, and on GCC, Clang with libstdc++/libc++ and MSVC with its own STL they are
 * right. clang-cl with the MSVC STL is the exception, and qb is exactly the shape that trips it:
 * the STL implements both as `__builtin_coro_promise(ptr, 0, ...)` -- an alignment of ZERO, which
 * MSVC's own compiler ignores because it lays the promise out at offset 16 of the frame whatever
 * its `alignas` (silently under-aligning it) -- while clang lays a promise out at
 * `alignTo(16, alignof(P))`, and, told 0, computes the handle at `promise - 16`. For a promise
 * whose alignment is 16 or less the two agree. For `alignas(64)` the handle lands 48 bytes into
 * the frame, `resume()` calls whatever bytes sit there, and the program dies -- at -O0 as at -O2.
 * Every `qb::Event` is cache-line aligned, so every `task<E>` over an event -- `qb::ask`'s result,
 * every request/reply coroutine -- has an over-aligned promise, and under clang-cl every one of
 * them crashed on its first `co_await` (Huly QB-200: found by the QB-46 compiler A/B, where a
 * clang-cl build ran the dispatch 10-18 % faster than MSVC and could not run an `ask` at all).
 * Reproduced in thirty lines with a bare `alignas(64)` promise; with the real alignment handed to
 * the builtin the handle is at `promise - 64`, and the coroutine resumes.
 *
 * So qb goes through these two functions everywhere it converts, and on clang-cl they hand the
 * builtin `alignof(P)`. Everywhere else they ARE the standard calls: nothing to measure.
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
 * @ingroup Async
 */

#ifndef QB_IO_ASYNC_COROUTINE_PROMISE_ACCESS_H
#define QB_IO_ASYNC_COROUTINE_PROMISE_ACCESS_H

#include <coroutine>

namespace qb::io::async::detail {

/// clang-cl on the MSVC STL: the toolchain whose `from_promise` / `promise()` are wrong for an
/// over-aligned promise (see the file's doc block). Every other one takes the standard calls.
#if defined(__clang__) && defined(_MSC_VER)
#define QB_CORO_PROMISE_VIA_BUILTIN 1
#else
#define QB_CORO_PROMISE_VIA_BUILTIN 0
#endif

/**
 * @brief The handle of the coroutine whose promise is `promise` -- `std::coroutine_handle<P>::from_promise`,
 *        correct for an over-aligned `P` on clang-cl.
 */
template <typename P>
[[nodiscard]] inline std::coroutine_handle<P>
handle_from_promise(P &promise) noexcept {
#if QB_CORO_PROMISE_VIA_BUILTIN
    return std::coroutine_handle<P>::from_address(__builtin_coro_promise(static_cast<void *>(&promise), alignof(P), true));
#else
    return std::coroutine_handle<P>::from_promise(promise);
#endif
}

/**
 * @brief The promise of the coroutine `h` -- `h.promise()`, correct for an over-aligned `P` on clang-cl.
 */
template <typename P>
[[nodiscard]] inline P &
promise_of(std::coroutine_handle<P> h) noexcept {
#if QB_CORO_PROMISE_VIA_BUILTIN
    return *static_cast<P *>(__builtin_coro_promise(h.address(), alignof(P), false));
#else
    return h.promise();
#endif
}

} // namespace qb::io::async::detail

#endif // QB_IO_ASYNC_COROUTINE_PROMISE_ACCESS_H
