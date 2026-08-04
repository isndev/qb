// <qb/event.h> ALONE. type_to_id<T>() reaches the shared type-id registry in the archive.
#include <qb/event.h>
#include <cstdio>

struct StandaloneEvent : qb::Event {
    int seq;
};

int
main() {
    std::printf("%u %s\n", (unsigned) qb::Event::type_to_id<StandaloneEvent>(),
                qb::event_type_name(qb::Event::type_to_id<StandaloneEvent>()));
    return 0;
}
