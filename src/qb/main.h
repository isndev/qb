/**
 * @file qb/main.h
 * @brief Convenience header for the QB Main engine controller.
 *
 * This file includes the primary headers for the Main class, which is used
 * to initialize, configure, and run the QB Actor Framework engine.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor) // Assuming similar copyright as others
 * @ingroup Engine
 */

#include "core/Main.h" // carries Main's own template bodies since 3.0 (was core/Main.tpp)
// Same reason as qb/actor.h:15 and qb/patterns.h:19 -- Actor.h does not pull the template impl,
// so an umbrella must be self-sufficient. Without these two, a TU whose only qb include is
// <qb/main.h> gets a complete qb::Actor with every member template DECLARED, compiles clean, and
// fails at LINK on qb::Actor::push<E> / qb::Pipe::push<E>. Main.h has already completed
// qb::Actor by this point (it includes Actor.h for TActorFactory), which is the position these
// bodies need.
#include "core/Actor.tpp"
// Pipe.h has carried Pipe's own template bodies since 3.0 (was core/Pipe.tpp); Actor.h:50
// already pulled it, so there is nothing left to add here.