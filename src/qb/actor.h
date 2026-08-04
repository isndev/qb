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
// Actor.h does not pull the template impl -- the bodies need a COMPLETE qb::VirtualCore and
// VirtualCore.h is what drags <windows.h> into a TU. Since 3.0 they live at the tail of
// VirtualCore.h (was core/Actor.tpp), so that is what an umbrella includes to be self-sufficient.
#include "core/VirtualCore.h"
// Pipe.h has carried Pipe's own template bodies since 3.0 (was core/Pipe.tpp); Actor.h:50
// already pulled it, so there is nothing left to add here.