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

#include                <errno.h>
#include                <cstdint>

#ifndef            QB_BUILD_MACROS_H_
#define            QB_BUILD_MACROS_H_

#if defined(_WIN32) || defined(_WIN64)
#define __WIN__SYSTEM__
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#elif defined(__linux__) || defined(__unix) || defined(__unix__) || defined(__APPLE__)
// Linux
#define __LINUX__SYSTEM__
#else
    #error "Unsupported Operating System"
#endif

#if defined(__WIN__SYSTEM__)
#define QB_GET __cdecl
#ifdef QB_DYNAMIC
// Windows platforms
 #ifndef QB_LINKED_AS_SHARED
 #pragma message ("WILL EXPORT DYNAMIC")
// From DLL side, we must export
 #define QB_API __declspec(dllexport)
 #else
  #pragma message ("WILL IMPORT DYNAMIC")
 // From client application side, we must import
 #define QB_GET __cdecl
 #define QB_API __declspec(dllimport)
 #endif
// For Visual C++ compilers, we also need to turn off this annoying C4251 warning.
// You can read lots ot different things about it, but the point is the code will
// just work fine, and so the simplest way to get rid of this warning is to disable it
 #ifdef _MSC_VER
 #pragma warning(disable : 4251)
 #pragma warning(disable : 4250)
 #endif
#else
// No specific directive needed for static build
#define QB_API
#endif
#define QB_UNUSED_VAR
#else
// Other platforms don't need to define anything
#define QB_GET
#define QB_API
    #ifdef __clang__
        #define QB_UNUSED_VAR __attribute__((unused))
    #else
        #define QB_UNUSED_VAR
    #endif
#endif

#endif
