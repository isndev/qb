
#include <qb/system/cpu.h>
#include <qb/system/parse.h>

#if defined(__APPLE__)
#include <sys/sysctl.h>

#elif defined(unix) || defined(__unix) || defined(__unix__)
#include <fstream>
#include <regex>
#include <unistd.h>

#elif defined(_WIN32) || defined(_WIN64)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winreg.h>
#endif

#include <cstdlib>
#include <string>
#include <utility>

namespace {

#if defined(_WIN32) || defined(_WIN64)
DWORD
CountSetBits(ULONG_PTR bit_mask) {
    DWORD     left_shift    = static_cast<DWORD>(sizeof(ULONG_PTR) * 8 - 1);
    DWORD     bit_set_count = 0;
    ULONG_PTR bit_test      = (static_cast<ULONG_PTR>(1) << left_shift);

    for (DWORD i = 0; i <= left_shift; ++i) {
        bit_set_count += ((bit_mask & bit_test) ? 1u : 0u);
        bit_test /= 2;
    }

    return bit_set_count;
}
#endif

} // namespace

namespace qb {

std::string
CPU::Architecture() {
#if defined(__APPLE__)
    char   result[1024] = {};
    size_t size         = sizeof(result);

    if (sysctlbyname("machdep.cpu.brand_string", result, &size, nullptr, 0) == 0) {
        return std::string(result);
    }

    return "<unknown>";

#elif defined(unix) || defined(__unix) || defined(__unix__)
    static const std::regex pattern(R"(model name(.*): (.*))");

    std::ifstream stream("/proc/cpuinfo");
    std::string   line;

    while (std::getline(stream, line)) {
        std::smatch matches;
        if (std::regex_match(line, matches, pattern)) {
            return matches[2].str();
        }
    }

    return "<unknown>";

#elif defined(_WIN32) || defined(_WIN64)
    HKEY hkey_processor = nullptr;
    LONG error          = RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &hkey_processor);
    if (error != ERROR_SUCCESS) {
        return "<unknown>";
    }

    auto key = resource(hkey_processor, [](HKEY key_handle) { RegCloseKey(key_handle); });

    CHAR  buffer[_MAX_PATH] = {};
    DWORD buffer_size       = sizeof(buffer);

    error = RegQueryValueExA(key.get(), "ProcessorNameString", nullptr, nullptr, reinterpret_cast<LPBYTE>(buffer), &buffer_size);
    if (error != ERROR_SUCCESS) {
        return "<unknown>";
    }

    return std::string(buffer);

#else
#error Unsupported platform
#endif
}

int
CPU::Affinity() {
#if defined(__APPLE__)
    int    logical      = 0;
    size_t logical_size = sizeof(logical);

    if (sysctlbyname("hw.logicalcpu", &logical, &logical_size, nullptr, 0) != 0) {
        return -1;
    }

    return logical;

#elif defined(unix) || defined(__unix) || defined(__unix__)
    const long processors = sysconf(_SC_NPROCESSORS_ONLN);
    return processors > 0 ? static_cast<int>(processors) : -1;

#elif defined(_WIN32) || defined(_WIN64)
    SYSTEM_INFO sysinfo;
    GetSystemInfo(&sysinfo);
    return static_cast<int>(sysinfo.dwNumberOfProcessors);

#else
#error Unsupported platform
#endif
}

int
CPU::LogicalCores() {
    return TotalCores().first;
}

int
CPU::PhysicalCores() {
    return TotalCores().second;
}

std::pair<int, int>
CPU::TotalCores() {
#if defined(__APPLE__)
    int    logical      = 0;
    size_t logical_size = sizeof(logical);
    if (sysctlbyname("hw.logicalcpu", &logical, &logical_size, nullptr, 0) != 0) {
        logical = -1;
    }

    int    physical      = 0;
    size_t physical_size = sizeof(physical);
    if (sysctlbyname("hw.physicalcpu", &physical, &physical_size, nullptr, 0) != 0) {
        physical = -1;
    }

    return {logical, physical};

#elif defined(unix) || defined(__unix) || defined(__unix__)
    const long processors = sysconf(_SC_NPROCESSORS_ONLN);
    const int  count      = processors > 0 ? static_cast<int>(processors) : -1;
    return {count, count};

#elif defined(_WIN32) || defined(_WIN64)
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION buffer = nullptr;
    DWORD                                 length = 0;

    for (;;) {
        const BOOL result = GetLogicalProcessorInformation(buffer, &length);
        if (result != FALSE) {
            break;
        }

        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            std::free(buffer);
            return {-1, -1};
        }

        std::free(buffer);
        buffer = static_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION>(std::malloc(length));
        if (buffer == nullptr) {
            return {-1, -1};
        }
    }

    std::pair<int, int> result{0, 0};
    auto               *current = buffer;
    DWORD               offset  = 0;

    while (offset + sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) <= length) {
        switch (current->Relationship) {
            case RelationProcessorCore:
                result.first += static_cast<int>(CountSetBits(current->ProcessorMask));
                result.second += 1;
                break;

            case RelationNumaNode:
            case RelationCache:
            case RelationProcessorPackage:
                break;

            default:
                std::free(buffer);
                return {-1, -1};
        }

        offset += sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
        ++current;
    }

    std::free(buffer);
    return result;

#else
#error Unsupported platform
#endif
}

std::int64_t
CPU::ClockSpeed() {
#if defined(__APPLE__)
    std::uint64_t frequency = 0;
    size_t        size      = sizeof(frequency);

    if (sysctlbyname("hw.cpufrequency", &frequency, &size, nullptr, 0) == 0) {
        return static_cast<std::int64_t>(frequency);
    }

    return -1;

#elif defined(unix) || defined(__unix) || defined(__unix__)
    static const std::regex pattern(R"(cpu MHz(.*): (.*))");

    std::ifstream stream("/proc/cpuinfo");
    std::string   line;

    while (std::getline(stream, line)) {
        std::smatch matches;
        if (std::regex_match(line, matches, pattern)) {
            const std::string mhz = matches[2].str();
            return static_cast<std::int64_t>(qb::to_number_prefix<double>(mhz).value_or(0.0) * 1000000.0);
        }
    }

    return -1;

#elif defined(_WIN32) || defined(_WIN64)
    HKEY hkey_processor = nullptr;
    LONG error          = RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &hkey_processor);
    if (error != ERROR_SUCCESS) {
        return -1;
    }

    auto key = resource(hkey_processor, [](HKEY key_handle) { RegCloseKey(key_handle); });

    DWORD mhz         = 0;
    DWORD buffer_size = sizeof(mhz);

    error = RegQueryValueExA(key.get(), "~MHz", nullptr, nullptr, reinterpret_cast<LPBYTE>(&mhz), &buffer_size);
    if (error != ERROR_SUCCESS) {
        return -1;
    }

    return static_cast<std::int64_t>(mhz) * 1000000;

#else
#error Unsupported platform
#endif
}

bool
CPU::HyperThreading() {
    const auto [logical, physical] = TotalCores();
    return logical > 0 && physical > 0 && logical != physical;
}

} // namespace qb