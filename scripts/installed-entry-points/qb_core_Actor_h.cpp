// <qb/core/Actor.h> ALONE -- the header named after the class, entered directly.
//
// DELIBERATE SCOPE. This TU does NOT ODR-use qb::Actor::push<E>, and that is not an oversight:
// Actor.h by design does not pull the template bodies (see the comment at qb/patterns.h:19).
// It cannot -- they need a COMPLETE qb::VirtualCore, and VirtualCore.h is what drags <windows.h>,
// WIN32_LEAN_AND_MEAN and NOMINMAX into a TU. Making every actor TU pay that is a worse trade
// than the umbrella rule. That is exactly why 3.0 parked those bodies at the TAIL OF
// VirtualCore.h rather than at the tail of this header: VirtualCore.h includes Actor.h, so
// Actor.h can never be the file that completes them. `push<E>` is the umbrellas' job, and
// qb_core_VirtualCore_h.cpp / qb_main_h.cpp /
// qb_actor_h.cpp / qb_patterns_h.cpp / qb_core_patterns_h.cpp each hold it to that.
//
// What this file DOES hold Actor.h to: entering here must still reach the archive. is_alive(),
// getName() and kill() are non-template members declared here and defined in libqb-core.
// The public pipe wrappers must also be usable from this header alone. Live pointers to
// getPipe/to below force their definitions and archive dependency into the link, without
// invoking either outside a worker. EventBuilder retains its private constructor
// and public, owning copy; only its push<E> template still requires the umbrella.
#include <qb/core/Actor.h>
#include <cstdio>
#include <type_traits>

class CoreActorActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        co_return true;
    }
};

void
use_archive(const CoreActorActor &a) {
    std::printf("%d %.*s\n", (int) a.is_alive(), (int) a.getName().size(), a.getName().data());
    a.kill();
}

using Builder = qb::Actor::EventBuilder;
static_assert(!std::is_default_constructible_v<Builder>);
static_assert(!std::is_constructible_v<Builder, qb::Pipe const &>);
static_assert(!std::is_constructible_v<Builder, qb::Actor const &, qb::ActorId>);
static_assert(std::is_nothrow_copy_constructible_v<Builder>);

Builder
copy_builder(Builder const &value) noexcept {
    return value;
}

int
main() {
    using GetPipe             = qb::Pipe (qb::Actor::*)(qb::ActorId) const noexcept;
    using To                  = Builder (qb::Actor::*)(qb::ActorId) const noexcept;
    GetPipe volatile get_pipe = &qb::Actor::getPipe;
    To volatile to            = &qb::Actor::to;
    auto volatile copy        = &copy_builder;
    (void) get_pipe;
    (void) to;
    (void) copy;
    return 0;
}
