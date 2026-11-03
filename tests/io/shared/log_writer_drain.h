/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file shared/log_writer_drain.h
 * @brief Leave nanolog's writer idle before a death test forks (Huly QB-1206). In tests/io/shared because the
 *        logger is qb-io's; qb-core's death tests include it from here.
 */

#ifndef QB_IO_TESTS_SHARED_LOG_WRITER_DRAIN_H
#define QB_IO_TESTS_SHARED_LOG_WRITER_DRAIN_H

#include <qb/io.h>

namespace qb::io::test {

// Call right before a death test forks, in a binary whose other tests log. gtest's threadsafe style forks
// (clones, on Linux), then execs the binary. Under TSan on macOS that fork hangs if nanolog's writer thread
// is formatting a queued line at that instant: TSan's fork interceptor takes its slot locks, then
// libSystem_atfork_prepare, inside the real fork(), waits for the locale lock the writer holds, while the
// writer waits for a slot lock (Huly QB-1193). DeathTest suites run before every other suite, so on a
// single pass nothing is queued yet; on a --gtest_repeat pass after the first, the previous pass's tests
// have just queued their lines. Replacing the logger joins the old writer once every queued line is
// written; the new writer, started before that join, has nothing to format. So the fork meets no writer
// inside a line -- provided no other thread logs meanwhile, which would also reach the logger being
// destroyed (Huly QB-1207). "./qb" and 512 MB are LogInitializer's own file and roll size (a binary that
// chose another path is sent back to this one); reopening it truncates the log, and the old writer's last
// lines land at its old offset, as every threadsafe death-test child already does to the file.
inline void
drain_log_writer_before_fork() {
#ifdef QB_WITH_LOGGING
    qb::io::log::init("./qb", 512);
#endif
}

} // namespace qb::io::test

#endif // QB_IO_TESTS_SHARED_LOG_WRITER_DRAIN_H
