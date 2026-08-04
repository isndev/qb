/**
 * @file qb/actor.h
 * @brief Convenience header for the core QB Actor components.
 *
 * This file includes the primary headers related to actor definition,
 * implementation, and actor communication pipes.
 * Include this file for easy access to core actor functionalities.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor) // Assuming similar copyright as others
 * @ingroup Actor
 */

#include "core/Actor.h"
#include "core/Actor.tpp"
// Pipe.h has carried Pipe's own template bodies since 3.0 (was core/Pipe.tpp); Actor.h:50
// already pulled it, so there is nothing left to add here.