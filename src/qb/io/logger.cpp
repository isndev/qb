/**
 * @file qb/io/logger.cpp
 * @brief Implementation of the logging system
 *
 * This file contains the implementation of the logging system for the QB framework.
 * It provides functionality for initializing the logger, setting log levels,
 * and thread-safe console output.
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

#include <exception>
#include <filesystem>
#include <string>
#include <system_error>
#include <qb/io.h>
#include <qb/io/async/coroutine/cancellation.h> // cancelled_error — the one exception NOT reported
#include <qb/io/async/coroutine/task.h>         // declares report_detached_coroutine_exception
#ifdef QB_WITH_LOGGING
void
qb::io::log::init(std::string const &file_path, uint32_t const roll_MB) {
    nanolog::initialize(nanolog::GuaranteedLogger(), file_path, roll_MB);
}

void
qb::io::log::setLevel(io::log::Level lvl) {
    nanolog::set_log_level(lvl);
}
#endif
std::mutex qb::io::cout::io_lock;

qb::io::cout::~cout() {
    std::lock_guard<std::mutex> lock(io_lock);
    std::cout << ss.str() << std::flush;
}

std::mutex qb::io::cerr::io_lock;

qb::io::cerr::~cerr() {
    std::lock_guard<std::mutex> lock(io_lock);
    std::cerr << ss.str() << std::flush;
}

// Declared in qb/io/async/coroutine/task.h; see the note there for WHEN it is called. Defined
// here, with the `qb::io::cerr` it reports through, so the policy lives in one place and the
// coroutine headers pull in no I/O machinery — the same arrangement as
// `qb::detail::report_unhandled_coroutine_exception` in Actor.cpp, whose text this mirrors so the
// two paths read alike in a log. `QB_LOG_*` is deliberately NOT used: it compiles to nothing
// unless QB_WITH_LOGGING is on, and a message that disappears in ordinary builds is the very
// failure being fixed. Nothing here is on a hot path — reaching it means a coroutine body threw.
void
qb::io::async::report_detached_coroutine_exception(std::exception_ptr ep) noexcept {
    const auto emit = [](char const *const what) {
        qb::io::cerr() << "CRITICAL: a DETACHED qb coroutine let an exception escape, and it was DISCARDED: " << what
                       << " -- it was spawned with no owner (qb::io::async::coro_scheduler().spawn(...)), so nothing "
                          "awaits it and nothing can receive the exception. Catch it in the coroutine body."
                       << std::endl;
    };
    // noexcept, and reached from a coroutine's final_suspend: it must not throw. The outer try
    // also covers a throw out of the inner HANDLER, which the inner catch clauses cannot take.
    try {
        try {
            std::rethrow_exception(ep);
        } catch (qb::io::async::cancelled_error const &) {
            // Cancellation is the teardown protocol, not a failure — the same exemption
            // `actor_coro_wrapper` makes (VirtualCore.h:1141-1144). A `when_any` loser or a
            // cancelled scope must not print a CRITICAL line.
            return;
        } catch (std::exception const &e) {
            // Emit from INSIDE the handler and never carry `e.what()` out of it: MSVC's
            // `rethrow_exception` throws a fresh COPY, so a `char const *` taken out of this
            // block dangles on Windows only — the platform with no CI.
            emit(e.what());
        } catch (...) {
            emit("<exception not derived from std::exception>");
        }
    } catch (...) {
        // Reporting must never become the failure.
    }
}

#ifdef QB_WITH_LOGGING
// WHAT THIS OBJECT DOES, STATED RATHER THAN IMPLIED
// ------------------------------------------------
// `initializer` is a namespace-scope static, so this constructor runs during STATIC
// INITIALISATION — before `main()`, in every binary that links qb-io with QB_WITH_LOGGING
// (`qbConfig.cmake:147`, default ON). Two visible side effects follow from that, and both are
// deliberate:
//
//   * a log file is created in the process's CURRENT WORKING DIRECTORY. The path is the fixed
//     relative `"./qb"`, to which nanolog appends `.<n>.log` (nanolog.cpp:606-609), so the file
//     is `./qb.1.log`. It is opened with `trunc`.
//   * nanolog's writer THREAD is started, from the NanoLogger constructor (nanolog.cpp:627/635).
//
// Both happen whether or not the program ever logs a line. Calling `qb::io::log::init()` later
// with your own path does not undo them — this file already exists by the time `main()` begins.
//
// THE SILENT FAILURE, MADE LOUD
// -----------------------------
// `FileWriter::roll_file` does `m_os->open(...)` and never checks the result (nanolog.cpp:610),
// so in a read-only or non-existent working directory logging is simply discarded, with no
// diagnostic anywhere and no failure at any later call. That file is VENDORED — the header
// promises it is upstream's — so the check belongs here, on qb's side of the boundary, and this
// is the only place that knows both the path that was requested and that nothing has verified it.
//
// One `stat` at static-init time, and a single line on `qb::io::cerr` when it fails.
// `QB_LOG_*` is not usable for this: it would route the complaint into the log that is not
// working. `std::filesystem` throws nothing here (the `error_code` overloads), and the
// constructor stays `noexcept`.
struct LogInitializer {
    static LogInitializer initializer;
    LogInitializer() noexcept {
        constexpr char const *kLogBase = "./qb";
        qb::io::log::init(kLogBase, 512);
#ifdef NDEBUG
        qb::io::log::setLevel(qb::io::log::Level::INFO);
#else
        qb::io::log::setLevel(qb::io::log::Level::DEBUG);
#endif
        // nanolog names the first roll `<base>.1.log`; if it is not there, the open failed and
        // every line logged by this process would be dropped in silence.
        std::error_code   ec;
        const std::string first_roll = std::string{kLogBase} + ".1.log";
        const bool        present    = std::filesystem::exists(first_roll, ec);
        if (!ec && present)
            return;
        std::error_code cwd_ec;
        const auto      cwd = std::filesystem::current_path(cwd_ec);
        qb::io::cerr() << "WARNING: qb logging is enabled (QB_WITH_LOGGING) but its log file '" << first_roll << "' could not be created in '"
                       << (cwd_ec ? std::string{"<unknown cwd>"} : cwd.string())
                       << "' -- every log line from this process will be DISCARDED. qb opens this file during static "
                          "initialisation, before main(), so run from a writable directory or build with "
                          "QB_WITH_LOGGING=OFF."
                       << std::endl;
    }
};

LogInitializer LogInitializer::initializer = {};
#endif