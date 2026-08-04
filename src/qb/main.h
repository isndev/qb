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

#include "core/Main.h"
#include "core/Main.tpp"
// Same reason as qb/actor.h:15 and qb/patterns.h:19 -- Actor.h does not pull the template impl,
// so an umbrella must be self-sufficient. Without these two, a TU whose only qb include is
// <qb/main.h> gets a complete qb::Actor with every member template DECLARED, compiles clean, and
// fails at LINK on qb::Actor::push<E> / qb::Pipe::push<E>. Main.tpp has already completed both
// qb::Actor and qb::VirtualCore by this point, which is the position these bodies need.
#include "core/Actor.tpp"
#include "core/Pipe.tpp"