/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/build-contracts/embedded-loop-prefix/witness.cpp
 * @brief qb's event loop and another libev in ONE program: each runs its own code (Huly QB-1139).
 *
 * qb embeds its own fork of libev, and that copy used to keep libev's link names (`ev_run`,
 * `ev_loop_new`, ...). A program that also linked another libev -- a system one, or a library
 * that embeds one -- then had two definitions of each name, and the link line chose between them
 * without a word: measured on ld64, the foreign archive first made qb's loop call the foreign
 * `ev_default_loop`; both objects loaded refused the link with `duplicate symbol`. A real libev's
 * `struct ev_loop` is not qev's, so the first case is a memory corruption at the first `ev_run`.
 *
 * The embedded copy now links as `qb_` + libev's name (`qb_ev_run`), through the renaming block
 * of its generated `ev_config.h`; in a translation unit that includes qb, `ev_run` still SPELLS
 * qb's loop. This test links the stand-in of foreign-libev.cpp BEFORE qb and asserts both sides:
 *
 *   - the foreign `ev_run` is in the program and is what a translation unit that includes nothing
 *     of qb reaches (caller.cpp);
 *   - qb's loop runs a timer on qb's own copy and never calls the foreign one;
 *   - in this translation unit `ev_run` names a different function from the foreign `ev_run`.
 *
 * Without the renaming this target does not link: foreign-libev.cpp's object is loaded (the test
 * references its marker) beside qb's ev.o, and every linker refuses the second `ev_run`.
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <gtest/gtest.h>
#include <qb/io/async.h>

namespace embedded_loop_prefix_test {

int           foreign_marker();
std::uint32_t foreign_calls();
void const   *foreign_ev_run_address();
int           call_foreign_ev_run();

using namespace std::chrono_literals;

class EmbeddedLoopPrefix : public ::testing::Test {
protected:
    void
    TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

TEST_F(EmbeddedLoopPrefix, AnotherLibevInTheProgramIsReachableAndIsNotQbsLoop) {
    ASSERT_EQ(foreign_marker(), 0x5eed) << "the foreign libev's object must be part of this link";

    // A translation unit that includes nothing of qb resolves `ev_run` to the other libev.
    const std::uint32_t before_call = foreign_calls();
    EXPECT_EQ(call_foreign_ev_run(), 0x5eed);
    EXPECT_EQ(foreign_calls(), before_call + 1);

    // qb's loop, on qb's copy: a timer is armed, the loop runs, the timer fires.
    const std::uint32_t before_loop = foreign_calls();
    std::atomic<int>    fired{0};
    qb::io::async::callback([&fired] { ++fired; }, 1ms);
    for (int pass = 0; pass < 1000 && fired.load() == 0; ++pass)
        qb::io::async::run(EVRUN_ONCE);
    EXPECT_EQ(fired.load(), 1) << "qb's loop must run its own timer";
    EXPECT_EQ(foreign_calls(), before_loop) << "qb's loop must never call the other libev";

    // Here `ev_run` is spelled through qb's headers: it names qb's copy, not the other libev's.
    EXPECT_NE(reinterpret_cast<void const *>(&ev_run), foreign_ev_run_address());
}

} // namespace embedded_loop_prefix_test
