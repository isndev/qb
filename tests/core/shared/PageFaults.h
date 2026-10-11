/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file shared/PageFaults.h
 * @brief Page-fault counters for the engine-restart witnesses (Huly QB-1012).
 *
 *   - `process_page_faults()` — the faults the whole process has taken so far, every thread
 *                               counted, the exited ones included: `getrusage(RUSAGE_SELF)`'s
 *                               minor faults on POSIX, `PageFaultCount` on Windows (which counts
 *                               every fault the process took, soft ones included).
 *   - `thread_page_faults()`  — the calling thread's minor faults (`RUSAGE_THREAD`), Linux only;
 *                               -1 elsewhere.
 *   - `page_bytes()`          — the page size the counts are in.
 *   - `hugepage_mode()`       — Linux's transparent-huge-page setting, for the record: a region
 *                               the kernel backs with a huge page faults once per 2 MiB, so a
 *                               count read on an `[always]` host undercounts re-touched memory.
 *
 * Counts, not times: a loaded host moves a fault count only through memory pressure (reclaim),
 * which the witnesses absorb by taking the smallest of several runs or the calling thread's own
 * count. All helpers live in namespace `qb::test`.
 */

#ifndef QB_CORE_TESTS_SHARED_PAGE_FAULTS_H
#define QB_CORE_TESTS_SHARED_PAGE_FAULTS_H

#include <cstddef>
#include <string>

#if defined(_WIN32) || defined(_WIN64)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#if defined(_MSC_VER)
#pragma comment(lib, "psapi.lib")
#endif
#else
#include <fstream>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace qb::test {

[[nodiscard]] inline long long
process_page_faults() noexcept {
#if defined(_WIN32) || defined(_WIN64)
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (!::GetProcessMemoryInfo(::GetCurrentProcess(), &counters, sizeof(counters)))
        return -1;
    return static_cast<long long>(counters.PageFaultCount);
#else
    rusage usage{};
    if (::getrusage(RUSAGE_SELF, &usage) != 0)
        return -1;
    return static_cast<long long>(usage.ru_minflt);
#endif
}

[[nodiscard]] inline long long
thread_page_faults() noexcept {
#if defined(__linux__) && defined(RUSAGE_THREAD)
    rusage usage{};
    if (::getrusage(RUSAGE_THREAD, &usage) != 0)
        return -1;
    return static_cast<long long>(usage.ru_minflt);
#else
    return -1;
#endif
}

[[nodiscard]] inline std::size_t
page_bytes() noexcept {
#if defined(_WIN32) || defined(_WIN64)
    SYSTEM_INFO info{};
    ::GetSystemInfo(&info);
    return static_cast<std::size_t>(info.dwPageSize);
#else
    long const page = ::sysconf(_SC_PAGESIZE);
    return page > 0 ? static_cast<std::size_t>(page) : 4096u;
#endif
}

[[nodiscard]] inline std::string
hugepage_mode() {
#if defined(__linux__)
    std::ifstream in("/sys/kernel/mm/transparent_hugepage/enabled");
    std::string   mode;
    std::getline(in, mode);
    return mode.empty() ? std::string("unknown") : mode;
#else
    return "n/a";
#endif
}

} // namespace qb::test

#endif // QB_CORE_TESTS_SHARED_PAGE_FAULTS_H
