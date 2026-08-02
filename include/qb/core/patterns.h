/**
 * @file qb/core/patterns.h
 * @brief Aggregating header for the qb-core interaction patterns.
 *
 * Pulls in the whole `qb/core/patterns/` module — request/response (`qb::ask`, `qb::answer`,
 * `Request`, `ask_by`/`deadline`), discovery/liveness (`qb::ping`, `qb::require`), idempotency
 * (`answer_idempotent`, `dedup_map`), aggregation
 * (`batcher`), streaming (`ask_stream`, `StreamRequest`), scatter-gather (`ask_all`, `ask_any`,
 * `ask_quorum`), saga (`run_saga`, `SagaScope`), resilience (`ask_retry`, `ask_guarded`,
 * `CircuitBreaker`, `rate_limiter`, `bulkhead`, `retry_policy`), routing (`WorkerPool`), pub/sub
 * (`PubSub`) and supervision (`Supervisor`, `SupervisedActor`, `restart_strategy`).
 *
 * These are free functions and helper types layered over the public `Actor` / `ScopedCoroContext`
 * primitives — the kernel (`qb/core/Actor.h`) holds no pattern logic of its own.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License").
 * @defgroup Patterns Interaction Patterns
 * @brief Header-only request/response, scatter-gather, resilience, streaming, saga, routing,
 *        pub/sub and supervision helpers layered over the `Actor` / `ScopedCoroContext` kernel.
 */

#ifndef QB_CORE_PATTERNS_H
#define QB_CORE_PATTERNS_H

// `Actor.h` declares `registerEvent` / `push_to` but leaves their definitions in `Actor.tpp`, so a
// header that exposes classes calling them must pull the implementation itself. The pattern headers
// below define inline members that do exactly that (`Supervisor::onInit`, `discovery::ping`), and
// under `--coverage` GCC EMITS unused inline functions so their lines can be instrumented -- which
// instantiates those calls in every translation unit that merely INCLUDES this header, whether or
// not it uses the class. Without the line below that instantiation has no definition and the link
// fails with `undefined reference to qb::Actor::registerEvent<qb::ChildDown, qb::Supervisor>`,
// visible only in the coverage build. `qb/patterns.h` already carries the same include for the same
// reason; this header was the one missed.
#include "Actor.h"
#include "Actor.tpp"

#include "patterns/request.h"
#include "patterns/discovery.h"
#include "patterns/idempotency.h"
#include "patterns/aggregate.h"
#include "patterns/streaming.h"
#include "patterns/scatter.h"
#include "patterns/saga.h"
#include "patterns/resilience.h"
#include "patterns/routing.h"
#include "patterns/pubsub.h"
#include "patterns/supervisor.h"

#endif // QB_CORE_PATTERNS_H
