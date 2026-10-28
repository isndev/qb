/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/build-contracts/embedded-loop-prefix/foreign-libev.cpp
 * @brief Another libev in the same program: libev's own link names, and bodies that are not qb's.
 *
 * The stand-in for what a real program links beside qb -- a system libev, or a library that
 * embeds one (Huly QB-1139). It defines, under libev's own C names, the entry points a qb loop
 * starts and runs with, and counts every call. It includes nothing of qb's, so in this
 * translation unit `ev_run` is the plain C name.
 *
 * It is one object on purpose. `foreign_marker()` is referenced by the test, so the linker loads
 * this object whatever the order of the libraries; with it loaded, a qb loop whose functions kept
 * libev's names either fails to link (two definitions of `ev_run`) or, under a linker that takes
 * the first definition it meets, runs this file's `ev_loop_new` and gets no loop back.
 */

#include <cstdint>

struct ev_loop;

namespace embedded_loop_prefix_test {
std::uint32_t g_foreign_calls = 0;
} // namespace embedded_loop_prefix_test

extern "C" {

struct ev_loop *
ev_default_loop(unsigned int) {
    ++embedded_loop_prefix_test::g_foreign_calls;
    return nullptr;
}

struct ev_loop *
ev_loop_new(unsigned int) {
    ++embedded_loop_prefix_test::g_foreign_calls;
    return nullptr;
}

int
ev_run(struct ev_loop *, int) {
    ++embedded_loop_prefix_test::g_foreign_calls;
    return 0x5eed;
}

void
ev_break(struct ev_loop *, int) {
    ++embedded_loop_prefix_test::g_foreign_calls;
}

} // extern "C"

namespace embedded_loop_prefix_test {

int
foreign_marker() {
    return 0x5eed;
}

std::uint32_t
foreign_calls() {
    return g_foreign_calls;
}

void const *
foreign_ev_run_address() {
    return reinterpret_cast<void const *>(&ev_run);
}

} // namespace embedded_loop_prefix_test
