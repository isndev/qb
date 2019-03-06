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

#elif defined(__linux__) || defined(__unix) || defined(__unix__)
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
#else
// Other platforms don't need to define anything
    #define QB_GET
    #define QB_API
#endif

#endif
