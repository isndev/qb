
#ifndef CUBE_ACTORPROXY_H
# define CUBE_ACTORPROXY_H
# include "IActor.h"

namespace cube {

    struct ActorProxy {
        uint64_t const _id;
        IActor *_this;
        void *const _handler;

        ActorProxy() : _id(0), _this(nullptr), _handler(nullptr) {}

        ActorProxy(uint64_t const id, IActor *actor, void *const handler)
                : _id(id), _this(actor), _handler(handler) {
        }
    };

}

#endif //CUBE_ACTORPROXY_H
