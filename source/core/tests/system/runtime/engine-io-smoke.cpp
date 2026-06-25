/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/runtime/engine-io-smoke.cpp
 * @brief End-to-end smoke of the engine + the qb logging plumbing, written HERMETICALLY.
 *
 * Boots a real `qb::Main`, runs one self-`push`/`kill` actor per core, and proves the qb logging
 * stack actually wrote what the actor logged. This is the lowest-level integration of the file
 * logger (`qb::io::log` → nanolog) with the actor lifecycle, so it deliberately asserts on the
 * REAL log-file CONTENT, not merely that `init()` was called or that `hasError()` is false.
 *
 * Hermeticity (the original test wrote `./test-mono-io.*.log` and `./test.io.*.log` into the
 * build CWD and never read them back):
 *   - every log file is created under a unique per-test directory inside the OS temp dir
 *     (`std::filesystem::temp_directory_path()`), never the CWD;
 *   - the directory is removed in TearDown, so the test leaves no artefacts;
 *   - nanolog is non-guaranteed and async, so before reading we FORCE A FLUSH by re-initialising
 *     the global logger to a throwaway temp path. `qb::io::log::init()` constructs the replacement
 *     logger and then destroys the previous one, whose destructor sets SHUTDOWN, joins its worker,
 *     and drains every queued line to disk — making the original file complete and readable.
 *
 * Log-file naming: `qb::io::log::init("<base>")` rolls files as `<base>.1.log`, `<base>.2.log`, …
 * (see qb/modules/nanolog/nanolog.cpp roll_file()). We read `<base>.1.log`.
 *
 * The actor mirrors its lifecycle into a process-global atom so a never-scheduled actor cannot let
 * the case pass vacuously, and emits a uniquely-tagged CRIT line we then grep out of the log.
 */

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#if defined(unix) || defined(__unix) || defined(__unix__) || defined(__APPLE__)
#include <unistd.h> // ::getpid
#endif

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/io.h>
#include <qb/main.h>
#include <qb/string.h>
#include <qb/system/time.h>

namespace {

// A tag unique to this binary so we can prove THIS actor's line reached the file (and not some
// unrelated framework log). Logged at CRIT so it survives any plausible level filter.
constexpr const char *kLogTag = "ENGINE_IO_SMOKE_TAG_4f1c";

std::atomic<int> g_inits{0};   // count of actor onInit() bodies that ran
std::atomic<int> g_handled{0}; // count of TestEvent handlers that ran

struct TestEvent : public qb::Event {};

class TestActor final : public qb::Actor {
public:
    TestActor() {
        EXPECT_NE(static_cast<std::uint32_t>(id()), 0u);
    }

    qb::io::async::task<bool>
    onInit() final {
        EXPECT_NE(static_cast<std::uint32_t>(id()), 0u);
        g_inits.fetch_add(1, std::memory_order_relaxed);
        // Uniquely-tagged CRIT line — the content oracle greps for this in the flushed log file.
        LOG_CRIT(kLogTag << " init core=" << static_cast<std::uint32_t>(getIndex())
                         << " id=" << static_cast<std::uint32_t>(id()));
        registerEvent<TestEvent>(*this);
        push<TestEvent>(id()); // self-send: drives the handler, then self-kill
        co_return true;
    }

    void
    on(TestEvent const &) {
        g_handled.fetch_add(1, std::memory_order_relaxed);
        kill();
    }
};

// ---------------------------------------------------------------------------
// Per-test hermetic logging fixture: unique temp dir, flush-on-teardown, helpers.
// ---------------------------------------------------------------------------
class EngineIoSmoke : public testing::Test {
protected:
    std::filesystem::path _dir;
    std::filesystem::path _base; // log basename passed to init(); files are _base + ".N.log"

    void
    SetUp() override {
        g_inits.store(0);
        g_handled.store(0);
        // Unique directory: temp / engine-io-smoke-<pid>-<test name>.
        const auto *info = testing::UnitTest::GetInstance()->current_test_info();
        std::ostringstream name;
        name << "qb-engine-io-smoke-" << static_cast<std::uint64_t>(::getpid()) << "-"
             << (info ? info->name() : "anon");
        _dir  = std::filesystem::temp_directory_path() / name.str();
        std::error_code ec;
        std::filesystem::remove_all(_dir, ec); // start clean
        std::filesystem::create_directories(_dir, ec);
        ASSERT_FALSE(ec) << "could not create temp log dir " << _dir << ": " << ec.message();
        _base = _dir / "engine";
    }

    void
    TearDown() override {
        // Re-point the global logger AWAY from our temp dir so the file handle is released, then
        // delete the directory — leaving the build tree pristine.
        const auto sink = (_dir / "drain").string();
        qb::io::log::init(sink, 1);
        std::error_code ec;
        std::filesystem::remove_all(_dir, ec);
    }

    // Force nanolog to flush the active file: replacing the global logger destroys the previous
    // one, whose destructor drains its queue to disk. Returns the path of the first rolled file.
    [[nodiscard]] std::filesystem::path
    flush_and_first_log() const {
        const auto drain = (_dir / "drain").string();
        qb::io::log::init(drain, 1); // destroys+drains the engine logger
        return std::filesystem::path(_base.string() + ".1.log");
    }

    [[nodiscard]] static std::string
    read_file(std::filesystem::path const &p) {
        std::ifstream in(p, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    [[nodiscard]] static std::size_t
    count_occurrences(std::string const &hay, std::string const &needle) {
        std::size_t n = 0, pos = 0;
        while ((pos = hay.find(needle, pos)) != std::string::npos) {
            ++n;
            pos += needle.size();
        }
        return n;
    }
};

TEST_F(EngineIoSmoke, MonoCoreLogsActorLifecycleToFile) {
    qb::io::log::init(_base.string(), 128);
    qb::io::log::setLevel(qb::io::log::Level::DEBUG);

    {
        qb::Main main;
        main.addActor<TestActor>(0);
        main.start();
        main.join();
        EXPECT_FALSE(main.hasError());
    }

    EXPECT_EQ(g_inits.load(), 1) << "exactly one actor must have initialised (no vacuous pass)";
    EXPECT_EQ(g_handled.load(), 1) << "the self-sent TestEvent must have been handled";

    const auto log = flush_and_first_log();
    ASSERT_TRUE(std::filesystem::exists(log)) << "log file must have been created at " << log;
    const auto content = read_file(log);
    EXPECT_FALSE(content.empty()) << "log file must not be empty after the engine ran";
    // Content oracle: the actor's uniquely-tagged init line is on disk.
    EXPECT_EQ(count_occurrences(content, kLogTag), 1u)
        << "the actor's tagged CRIT line must appear exactly once in " << log;
    EXPECT_NE(content.find("core=0"), std::string::npos) << "the logged core index must be present";
}

TEST_F(EngineIoSmoke, MultiCoreLogsEveryActorAndLevelGateHolds) {
    const unsigned hw    = std::thread::hardware_concurrency();
    const auto     cores = hw == 0u ? 1u : hw;
    if (cores < 2u)
        GTEST_SKIP() << "requires-multicore: single-core runner cannot exercise multi-core logging";

    qb::io::log::init(_base.string(), 128);
    qb::io::log::setLevel(qb::io::log::Level::VERBOSE);

    // Pure level-gate predicates (no engine needed): below the threshold is filtered, at/above is
    // kept. `qb::io::log::Level` is an alias of `nanolog::LogLevel`; the predicate lives in nanolog.
    EXPECT_FALSE(nanolog::is_logged(qb::io::log::Level::DEBUG)) << "DEBUG < VERBOSE → filtered";
    EXPECT_TRUE(nanolog::is_logged(qb::io::log::Level::VERBOSE)) << "VERBOSE == threshold → kept";
    EXPECT_TRUE(nanolog::is_logged(qb::io::log::Level::CRIT)) << "CRIT > VERBOSE → kept";

    {
        qb::Main main;
        for (auto i = 0u; i < cores; ++i)
            main.addActor<TestActor>(i);
        main.start();
        main.join();
        EXPECT_FALSE(main.hasError());
    }

    EXPECT_EQ(g_inits.load(), static_cast<int>(cores)) << "every per-core actor must have initialised";
    EXPECT_EQ(g_handled.load(), static_cast<int>(cores)) << "every actor must have handled its self-send";

    const auto log = flush_and_first_log();
    ASSERT_TRUE(std::filesystem::exists(log)) << "log file must exist at " << log;
    const auto content = read_file(log);
    // One tagged init line per core actor — proves every core's actor logged to the same file.
    EXPECT_EQ(count_occurrences(content, kLogTag), static_cast<std::size_t>(cores))
        << "expected one tagged CRIT line per core in " << log;
}

} // namespace
