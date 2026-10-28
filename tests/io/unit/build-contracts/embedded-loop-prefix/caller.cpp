/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/build-contracts/embedded-loop-prefix/caller.cpp
 * @brief A translation unit of the program that uses the OTHER libev and includes nothing of qb's.
 *
 * Its `ev_run` is resolved by the linker, not by qb's headers: with qb's copy linked under its
 * own names, it must reach the other libev's definition (foreign-libev.cpp), proving that copy is
 * really in the program and really callable beside qb's.
 */

struct ev_loop;
extern "C" int ev_run(struct ev_loop *, int);

namespace embedded_loop_prefix_test {

int
call_foreign_ev_run() {
    return ev_run(nullptr, 0);
}

} // namespace embedded_loop_prefix_test
