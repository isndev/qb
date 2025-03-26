/**
 * @file qb/system/cpu.h
 * @brief CPU information and utilities
 *
 * This file provides platform-independent access to CPU information such as 
 * architecture, core count, and frequency. It also includes utilities for 
 * CPU-specific operations like spinlock pauses.
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
 * @ingroup System
 */

#ifndef FEATURES_CPU_H
#define FEATURES_CPU_H

#include <memory>
#include <thread>
#if defined(__APPLE__)
#    include <sys/sysctl.h>
#elif defined(unix) || defined(__unix) || defined(__unix__) || defined(__APPLE__)
#    include <fstream>
#    include <regex>
#    include <unistd.h>
#elif defined(_WIN32) || defined(_WIN64)
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#    include <windows.h>
#endif
#endif

/**
 * @namespace Internals
 * @brief Internal helper functions for CPU information
 */
namespace Internals {

#if defined(_WIN32) || defined(_WIN64)
/**
 * @brief Counts the number of bits set in a processor mask
 * 
 * Helper function to count the number of logical processors in a processor mask.
 * 
 * @param pBitMask The processor mask to count bits in
 * @return Number of bits set in the mask
 */
DWORD
CountSetBits(ULONG_PTR pBitMask) {
    DWORD dwLeftShift = sizeof(ULONG_PTR) * 8 - 1;
    DWORD dwBitSetCount = 0;
    ULONG_PTR pBitTest = (ULONG_PTR)1 << dwLeftShift;

    for (DWORD i = 0; i <= dwLeftShift; ++i) {
        dwBitSetCount += ((pBitMask & pBitTest) ? 1 : 0);
        pBitTest /= 2;
    }

    return dwBitSetCount;
}
#endif

} // namespace Internals

namespace qb {

/**
 * @brief Creates a unique_ptr with a custom deleter for a resource
 * 
 * This is a helper function to manage resources in a RAII manner
 * with a custom deleter function.
 * 
 * @tparam T Type of the resource
 * @tparam TCleaner Type of the cleaner/deleter function
 * @param handle Resource handle
 * @param cleaner Deleter function
 * @return unique_ptr wrapping the resource with the custom deleter
 */
template <typename T, typename TCleaner>
auto
resource(T handle, TCleaner cleaner) {
    return std::unique_ptr<typename std::remove_pointer<T>::type, TCleaner>(handle,
                                                                            cleaner);
}

/**
 * @brief Creates a unique_ptr with a custom deleter for a void* resource
 * 
 * Specialization for void* resources.
 * 
 * @tparam TCleaner Type of the cleaner/deleter function
 * @param handle Resource handle
 * @param cleaner Deleter function
 * @return unique_ptr wrapping the resource with the custom deleter
 */
template <typename TCleaner>
auto
resource(void *handle, TCleaner cleaner) {
    return std::unique_ptr<void, TCleaner>(handle, cleaner);
}

/**
 * @brief Creates a unique_ptr with a custom deleter using the deleter itself as a handle
 * 
 * This variant is useful for self-contained resources.
 * 
 * @tparam TCleaner Type of the cleaner/deleter function
 * @param cleaner Deleter function
 * @return unique_ptr with the deleter pointing to itself
 */
template <typename TCleaner>
auto
resource(TCleaner cleaner) {
    return std::unique_ptr<void, TCleaner>(&cleaner, cleaner);
}

/**
 * @class CPU
 * @brief Provides platform-independent access to CPU information
 * 
 * This class contains static methods to query CPU information such as
 * architecture, logical/physical core counts, clock speed, and more.
 */
class CPU {
public:
    CPU() = delete;
    CPU(const CPU &) = delete;
    CPU(CPU &&) noexcept = delete;
    ~CPU() = delete;

    CPU &operator=(const CPU &) = delete;
    CPU &operator=(CPU &&) noexcept = delete;

    /**
     * @brief Gets the CPU architecture name/description
     * 
     * @return CPU architecture or model name as a string
     */
    static std::string
    Architecture() {
#if defined(__APPLE__)
        char result[1024];
        size_t size = sizeof(result);
        if (sysctlbyname("machdep.cpu.brand_string", result, &size, nullptr, 0) == 0)
            return result;

        return "<unknown>";
#elif defined(unix) || defined(__unix) || defined(__unix__)
        static std::regex pattern("model name(.*): (.*)");

        std::string line;
        std::ifstream stream("/proc/cpuinfo");
        while (getline(stream, line)) {
            std::smatch matches;
            if (std::regex_match(line, matches, pattern))
                return matches[2];
        }

        return "<unknown>";
#elif defined(_WIN32) || defined(_WIN64)
        HKEY hKeyProcessor;
        LONG lError = RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                                    "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                                    0, KEY_READ, &hKeyProcessor);
        if (lError != ERROR_SUCCESS)
            return "<unknown>";

        // Smart resource cleaner pattern
        auto key = resource(hKeyProcessor, [](HKEY hKey) { RegCloseKey(hKey); });

        CHAR pBuffer[_MAX_PATH] = {0};
        DWORD dwBufferSize = sizeof(pBuffer);
        lError = RegQueryValueExA(key.get(), "ProcessorNameString", nullptr, nullptr,
                                  (LPBYTE)pBuffer, &dwBufferSize);
        if (lError != ERROR_SUCCESS)
            return "<unknown>";

        return std::string(pBuffer);
#else
#    error Unsupported platform
#endif
    }
    /**
     * @brief Gets the number of logical processors available to the process
     * 
     * @return Number of logical processors
     */
    static int
    Affinity() {
#if defined(__APPLE__)
        int logical = 0;
        size_t logical_size = sizeof(logical);
        if (sysctlbyname("hw.logicalcpu", &logical, &logical_size, nullptr, 0) != 0)
            logical = -1;

        return logical;
#elif defined(unix) || defined(__unix) || defined(__unix__)
        long processors = sysconf(_SC_NPROCESSORS_ONLN);
        return processors;
#elif defined(_WIN32) || defined(_WIN64)
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        return si.dwNumberOfProcessors;
#else
#    error Unsupported platform
#endif
    }
    /**
     * @brief Gets the number of logical CPU cores
     * 
     * @return Number of logical cores
     */
    static int
    LogicalCores() {
        return TotalCores().first;
    }
    /**
     * @brief Gets the number of physical CPU cores
     * 
     * @return Number of physical cores
     */
    static int
    PhysicalCores() {
        return TotalCores().second;
    }
    /**
     * @brief Gets both logical and physical core counts
     * 
     * @return Pair of (logical cores, physical cores)
     */
    static std::pair<int, int>
    TotalCores() {
#if defined(__APPLE__)
        int logical = 0;
        size_t logical_size = sizeof(logical);
        if (sysctlbyname("hw.logicalcpu", &logical, &logical_size, nullptr, 0) != 0)
            logical = -1;

        int physical = 0;
        size_t physical_size = sizeof(physical);
        if (sysctlbyname("hw.physicalcpu", &physical, &physical_size, nullptr, 0) != 0)
            physical = -1;

        return std::make_pair(logical, physical);
#elif defined(unix) || defined(__unix) || defined(__unix__)
        long processors = sysconf(_SC_NPROCESSORS_ONLN);
        return std::make_pair(processors, processors);
#elif defined(_WIN32) || defined(_WIN64)
        BOOL allocated = FALSE;
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION pBuffer = nullptr;
        DWORD dwLength = 0;

        while (!allocated) {
            BOOL bResult = GetLogicalProcessorInformation(pBuffer, &dwLength);
            if (bResult == FALSE) {
                if (GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
                    if (pBuffer != nullptr)
                        std::free(pBuffer);
                    pBuffer =
                        (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION)std::malloc(dwLength);
                    if (pBuffer == nullptr)
                        return std::make_pair(-1, -1);
                } else
                    return std::make_pair(-1, -1);
            } else
                allocated = TRUE;
        }

        std::pair<int, int> result(0, 0);
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION pCurrent = pBuffer;
        DWORD dwOffset = 0;

        while (dwOffset + sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) <= dwLength) {
            switch (pCurrent->Relationship) {
            case RelationProcessorCore:
                result.first += Internals::CountSetBits(pCurrent->ProcessorMask);
                result.second += 1;
                break;
            case RelationNumaNode:
            case RelationCache:
            case RelationProcessorPackage:
                break;
            default:
                return std::make_pair(-1, -1);
            }
            dwOffset += sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
            pCurrent++;
        }

        std::free(pBuffer);

        return result;
#else
#    error Unsupported platform
#endif
    }
    /**
     * @brief Gets the CPU clock speed in Hz
     * 
     * @return CPU frequency in Hz, or -1 if unavailable
     */
    static int64_t
    ClockSpeed() {
#if defined(__APPLE__)
        uint64_t frequency = 0;
        size_t size = sizeof(frequency);
        if (sysctlbyname("hw.cpufrequency", &frequency, &size, nullptr, 0) == 0)
            return frequency;

        return -1;
#elif defined(unix) || defined(__unix) || defined(__unix__)
        static std::regex pattern("cpu MHz(.*): (.*)");

        std::string line;
        std::ifstream stream("/proc/cpuinfo");
        while (getline(stream, line)) {
            std::smatch matches;
            if (std::regex_match(line, matches, pattern))
                return (int64_t)(atof(matches[2].str().c_str()) * 1000000);
        }

        return -1;
#elif defined(_WIN32) || defined(_WIN64)
        HKEY hKeyProcessor;
        long lError = RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                                    "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                                    0, KEY_READ, &hKeyProcessor);
        if (lError != ERROR_SUCCESS)
            return -1;

        // Smart resource cleaner pattern
        auto key = resource(hKeyProcessor, [](HKEY hKey) { RegCloseKey(hKey); });

        DWORD dwMHz = 0;
        DWORD dwBufferSize = sizeof(DWORD);
        lError = RegQueryValueExA(key.get(), "~MHz", nullptr, nullptr, (LPBYTE)&dwMHz,
                                  &dwBufferSize);
        if (lError != ERROR_SUCCESS)
            return -1;

        return dwMHz * 1000000;
#else
#    error Unsupported platform
#endif
    }
    /**
     * @brief Checks if hyperthreading is enabled
     * 
     * @return true if hyperthreading is enabled, false otherwise
     */
    static bool
    HyperThreading() {
        std::pair<int, int> cores = TotalCores();
        return (cores.first != cores.second);
    }
};

} // namespace qb

#ifdef __SSE2__
#include <emmintrin.h>
namespace qb {
    /**
     * @brief Executes a CPU pause instruction for spinloops
     * 
     * This function helps improve performance of spin-wait loops by
     * providing a hint to the CPU that the code is in a spin-wait loop.
     * It can reduce power consumption and improve performance by avoiding
     * memory order violations and excessive bus activity.
     * 
     * SSE2 implementation.
     */
    inline void spin_loop_pause() noexcept { _mm_pause(); }
} // namespace qb
#elif defined(_MSC_VER) && _MSC_VER >= 1800 && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
namespace qb {
  /**
   * @brief Executes a CPU pause instruction for spinloops
   * 
   * MSVC implementation for x64/x86.
   */
  inline void spin_loop_pause() noexcept { _mm_pause(); }
} // namespace qb
#else
namespace qb {
  /**
   * @brief Fallback implementation of spin_loop_pause for platforms without CPU pause
   * 
   * Calls std::this_thread::yield() as a fallback.
   */
  inline void spin_loop_pause() noexcept { std::this_thread::yield(); }
} // namespace qb
#endif

#endif // FEATURES_CPU_H
