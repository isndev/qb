/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file shared/quiet_log.h
 * @brief The log no lower than WARN for a scope, for a case that logs by the hundred thousand lines in an unoptimised
 *        build (Huly QB-1270). In tests/io/shared because the logger is qb-io's; qb-core's tests include it from here.
 */

#ifndef QB_IO_TESTS_SHARED_QUIET_LOG_H
#define QB_IO_TESTS_SHARED_QUIET_LOG_H

#include <qb/io.h>

namespace qb::io::test {

// A build without NDEBUG logs from DEBUG up by default (LogInitializer, qb/io/logger.cpp), to ./qb.1.log, and nanolog
// drops no line: what a case queues faster than the writer thread formats it is written out when the process ends,
// after the last case. A population of 200 000 actors logs eight lifecycle lines an actor -- 1.6 million lines, 200 MiB
// -- and its binary, 0.06 s in a Release build, took 16 s alone under MSVC Debug and 92 s of the system tier's 120 s
// in a loaded suite (the Windows gate of 2026-11-14). The log is no part of what such a case asserts.
//
// Declare one where the lines are produced, engine threads included (the level is the process's): the level found is
// put back when it goes, so the cases around keep logging -- which is what runs the operands of every other log
// statement, under the sanitizers too. Never raises the verbosity: a level above WARN is left as it is. A build that
// logs to stdout (QB_STDOUT_LOGGING) cannot be filtered and is left as it is; one that does not log has nothing to do.
class quiet_log {
#ifdef QB_WITH_LOGGING
    qb::io::log::Level _found = qb::io::log::Level::CRIT;

public:
    quiet_log() noexcept {
        for (const auto level : {qb::io::log::Level::DEBUG, qb::io::log::Level::VERBOSE, qb::io::log::Level::INFO, qb::io::log::Level::WARN}) {
            if (nanolog::is_logged(level)) {
                _found = level;
                break;
            }
        }
        if (_found < qb::io::log::Level::WARN)
            qb::io::log::setLevel(qb::io::log::Level::WARN);
    }
    ~quiet_log() {
        qb::io::log::setLevel(_found);
    }
#else

public:
    quiet_log() noexcept {} // user-provided, so that a guard declared and never named is no unused variable
#endif
    quiet_log(quiet_log const &)            = delete;
    quiet_log &operator=(quiet_log const &) = delete;
};

} // namespace qb::io::test

#endif // QB_IO_TESTS_SHARED_QUIET_LOG_H
