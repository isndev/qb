/**
 * @file qb/core/patterns.h
 * @brief Aggregating header for the qb-core interaction patterns.
 *
 * Pulls in the whole `qb/core/patterns/` module — request/response (`qb::ask`, `qb::answer`,
 * `Request`), scatter-gather (`ask_all`, `ask_any`), saga (`run_saga`, `SagaScope`), resilience
 * (`ask_retry`, `ask_guarded`, `CircuitBreaker`, `retry_policy`), routing (`WorkerPool`),
 * pub/sub (`PubSub`) and supervision (`Supervisor`, `SupervisedActor`, `restart_strategy`).
 *
 * These are free functions and helper types layered over the public `Actor` / `ScopedCoroContext`
 * primitives — the kernel (`qb/core/Actor.h`) holds no pattern logic of its own.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License").
 * @ingroup Patterns
 */

#ifndef QB_CORE_PATTERNS_H
#define QB_CORE_PATTERNS_H

#include "patterns/request.h"
#include "patterns/scatter.h"
#include "patterns/saga.h"
#include "patterns/resilience.h"
#include "patterns/routing.h"
#include "patterns/pubsub.h"
#include "patterns/supervisor.h"

#endif // QB_CORE_PATTERNS_H
