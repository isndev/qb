/**
 * @file qb/patterns.h
 * @brief Convenience umbrella for the qb-core interaction patterns library.
 *
 * Include this for one-stop access to the request/response, scatter-gather, saga, resilience,
 * routing, pub/sub and supervision patterns built on top of `qb::Actor`. See
 * `qb/core/patterns.h` for the module breakdown.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License").
 * @ingroup Patterns
 */

#ifndef QB_PATTERNS_H
#define QB_PATTERNS_H

#include "core/Actor.h"
#include "core/Actor.tpp" // Actor.h does not pull the template impl; the umbrella must be self-sufficient.
#include "core/patterns.h"

#endif // QB_PATTERNS_H
