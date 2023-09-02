/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2021 isndev (www.qbaf.io). All rights reserved.
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

#include <cerrno>
#include <cstdint>

#ifndef QB_BUILD_MACROS_H_
#    define QB_BUILD_MACROS_H_

#    if defined(_WIN32) || defined(_WIN64)
#        define __WIN__SYSTEM__
#        ifndef WIN32_LEAN_AND_MEAN
#            define WIN32_LEAN_AND_MEAN
#        endif
#        ifndef NOMINMAX
#            define NOMINMAX
#        endif

#    elif defined(__linux__) || defined(__unix) || defined(__unix__) || \
        defined(__APPLE__)
// Linux
#        define __LINUX__SYSTEM__
#    else
#        error "Unsupported Operating System"
#    endif

#    if defined(__WIN__SYSTEM__)
#        define QB_GET __cdecl
#        ifdef QB_DYNAMIC
// Windows platforms
#            ifndef QB_LINKED_AS_SHARED
#                pragma message("WILL EXPORT DYNAMIC")
// From DLL side, we must export
#                define QB_API __declspec(dllexport)
#            else
#                pragma message("WILL IMPORT DYNAMIC")
// From client application side, we must import
#                define QB_GET __cdecl
#                define QB_API __declspec(dllimport)
#            endif
// For Visual C++ compilers, we also need to turn off this annoying C4251 warning.
// You can read lots ot different things about it, but the point is the code will
// just work fine, and so the simplest way to get rid of this warning is to disable it
#            ifdef _MSC_VER
#                pragma warning(disable : 4251)
#                pragma warning(disable : 4250)
#            endif
#        else
// No specific directive needed for static build
#            define QB_API
#        endif
#        define QB_UNUSED_VAR
#    else
// Other platforms don't need to define anything
#        define QB_GET
#        define QB_API
#        ifdef __clang__
#            define QB_UNUSED_VAR __attribute__((unused))
#        else
#            define QB_UNUSED_VAR
#        endif
#    endif

// Tests whether compiler has fully c++11 support
// About preprocessor '_MSC_VER', please see:
// https://docs.microsoft.com/en-us/cpp/preprocessor/predefined-macros?view=vs-2019
#if defined(_MSC_VER)
#  if _MSC_VER < 1900
#    define QB__HAS_FULL_CXX11 0
#  else
#    define QB__HAS_FULL_CXX11 1
#    if _MSC_VER > 1900 // VS2017 or later
#      include <vcruntime.h>
#      include <sdkddkver.h>
#    endif
#  endif
#else
#  define QB__HAS_FULL_CXX11 1
#endif

// Tests whether compiler has c++14 support
#if (defined(__cplusplus) && __cplusplus >= 201402L) || (defined(_MSC_VER) && _MSC_VER >= 1900 && (defined(_MSVC_LANG) && (_MSVC_LANG >= 201402L)))
#  ifndef QB_HAS_CXX14
#    define QB__HAS_CXX14 1
#  endif // C++14 features macro
#endif   // C++14 features check
#if !defined(QB__HAS_CXX14)
#  define QB__HAS_CXX14 0
#endif

// Tests whether compiler has c++17 support
#if (defined(__cplusplus) && __cplusplus >= 201703L) ||                                                                                                        \
    (defined(_MSC_VER) && _MSC_VER > 1900 && ((defined(_HAS_CXX17) && _HAS_CXX17 == 1) || (defined(_MSVC_LANG) && (_MSVC_LANG > 201402L))))
#  ifndef QB_HAS_CXX17
#    define QB__HAS_CXX17 1
#  endif // C++17 features macro
#endif   // C++17 features check
#if !defined(QB__HAS_CXX17)
#  define QB__HAS_CXX17 0
#endif

// Tests whether compiler has c++20 support
#if (defined(__cplusplus) && __cplusplus > 201703L) ||                                                                                                         \
    (defined(_MSC_VER) && _MSC_VER > 1900 && ((defined(_HAS_CXX20) && _HAS_CXX20 == 1) || (defined(_MSVC_LANG) && (_MSVC_LANG > 201703L))))
#  ifndef QB__HAS_CXX20
#    define QB__HAS_CXX20 1
#  endif // C++20 features macro
#endif   // C++20 features check
#if !defined(QB__HAS_CXX20)
#  define QB__HAS_CXX20 0
#endif

// Workaround for compiler without fully c++11 support, such as vs2013
#if QB__HAS_FULL_CXX11
#  define QB__HAS_NS_INLINE 1
#  define QB__NS_INLINE inline
#else
#  define QB__HAS_NS_INLINE 0
#  define QB__NS_INLINE
#  if defined(constexpr)
#    undef constexpr
#  endif
#  define constexpr const
#endif

// Unix domain socket feature test
#if !defined(_WIN32) || defined(NTDDI_WIN10_RS5)
#  define QB__HAS_UDS 1
#else
#  define QB__HAS_UDS 0
#endif

// Test whether sockaddr has member 'sa_len'
#if defined(__linux__) || defined(_WIN32)
#  define QB__HAS_SA_LEN 0
#else
#  if defined(__unix__) || defined(__APPLE__)
#    define QB__HAS_SA_LEN 1
#  else
#    define QB__HAS_SA_LEN 0
#  endif
#endif

#if !defined(_WIN32) || defined(NTDDI_VISTA)
#  define QB__HAS_NTOP 1
#else
#  define QB__HAS_NTOP 0
#endif

// 64bits Sense Macros
#if defined(_M_X64) || defined(_WIN64) || defined(__LP64__) || defined(_LP64) || defined(__x86_64) || defined(__arm64__) || defined(__aarch64__)
#  define QB__64BITS 1
#else
#  define QB__64BITS 0
#endif

// Try detect compiler exceptions
#if !defined(__cpp_exceptions)
#  define QB__NO_EXCEPTIONS 1
#endif

#if !defined(QB__NO_EXCEPTIONS)
#  define QB__THROW(x, retval) throw(x)
#  define QB__THROW0(x) throw(x)
#else
#  define QB__THROW(x, retval) return (retval)
#  define QB__THROW0(x) return
#endif

// Compatibility with non-clang compilers...
#ifndef __has_attribute
#  define __has_attribute(x) 0
#endif
#ifndef __has_builtin
#  define __has_builtin(x) 0
#endif

/*
 * Helps the compiler's optimizer predicting branches
 */
#if __has_builtin(__builtin_expect)
#  ifdef __cplusplus
#    define qb__likely(exp) (__builtin_expect(!!(exp), true))
#    define qb__unlikely(exp) (__builtin_expect(!!(exp), false))
#  else
#    define qb__likely(exp) (__builtin_expect(!!(exp), 1))
#    define qb__unlikely(exp) (__builtin_expect(!!(exp), 0))
#  endif
#else
#  define qb__likely(exp) (!!(exp))
#  define qb__unlikely(exp) (!!(exp))
#endif

#define QB__STD ::std::

#if defined(_MSC_VER)
#define DISABLE_WARNING_PUSH           __pragma(warning( push ))
#define DISABLE_WARNING_POP            __pragma(warning( pop ))
#define DISABLE_WARNING(warningNumber) __pragma(warning( disable : warningNumber ))

#define DISABLE_WARNING_UNREFERENCED_FORMAL_PARAMETER    DISABLE_WARNING(4100)
#define DISABLE_WARNING_UNREFERENCED_FUNCTION            DISABLE_WARNING(4505)
#define DISABLE_WARNING_NARROWING                        DISABLE_WARNING(4245) \
                                                         DISABLE_WARNING(4838)

#define DISABLE_WARNING_DEPRECATED                       DISABLE_WARNING(4996)
#define DISABLE_WARNING_OLD_STYLE_CAST
#define DISABLE_WARNING_IMPLICIT_FALLTHROUGH
// other warnings you want to deactivate...

#elif defined(__GNUC__) || defined(__clang__)
#define DO_PRAGMA(X) _Pragma(#X)
#define DISABLE_WARNING_PUSH           DO_PRAGMA(GCC diagnostic push)
#define DISABLE_WARNING_POP            DO_PRAGMA(GCC diagnostic pop)
#define DISABLE_WARNING(warningName)   DO_PRAGMA(GCC diagnostic ignored #warningName)

#define DISABLE_WARNING_UNREFERENCED_FORMAL_PARAMETER    DISABLE_WARNING(-Wunused-parameter)
#define DISABLE_WARNING_UNREFERENCED_FUNCTION            DISABLE_WARNING(-Wunused-function)
#define DISABLE_WARNING_NARROWING                        DISABLE_WARNING(-Wnarrowing)
#define DISABLE_WARNING_DEPRECATED                       DISABLE_WARNING(-Wdeprecated-declarations)
#define DISABLE_WARNING_OLD_STYLE_CAST                   DISABLE_WARNING(-Wold-style-cast)
#define DISABLE_WARNING_IMPLICIT_FALLTHROUGH             DISABLE_WARNING(-Wimplicit-fallthrough)
// other warnings you want to deactivate...

#else
#define DISABLE_WARNING_PUSH
#define DISABLE_WARNING_POP
#define DISABLE_WARNING_UNREFERENCED_FORMAL_PARAMETER
#define DISABLE_WARNING_UNREFERENCED_FUNCTION
#define DISABLE_WARNING_NARROWING
#define DISABLE_WARNING_DEPRECATED
#define DISABLE_WARNING_OLD_STYLE_CAST
#define DISABLE_WARNING_IMPLICIT_FALLTHROUGH

#endif

#endif