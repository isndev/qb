
#ifndef CUBE_IACTOR_H
# define CUBE_IACTOR_H
# include <iostream>
# include <utility>

# include "utils/prefix.h"
# include "Types.h"

namespace cube {
    struct ActorId {
        using NotFound = ActorId;

        uint32_t _id;
        uint32_t _index;
    public:
        ActorId() : _id(0), _index(0) {}
        ActorId(uint32_t const id, uint32_t const index) : _id(id), _index(index) {}
        ActorId(ActorId const &) = default;
        ActorId(uint64_t const id) {
        *reinterpret_cast<uint64_t *>(this) = id;
        }

        inline operator const uint64_t &() const {
            return *reinterpret_cast<uint64_t const *>(this);
        }

        inline bool operator!=(ActorId const &rhs) const {
            return static_cast<uint64_t>(*this) != static_cast<uint64_t>(rhs);
        }

    };

    struct CUBE_LOCKFREE_CACHELINE_ALIGNMENT Event {
        uint32_t context_size;
        uint32_t bucket_size;
        uint64_t id;
        ActorId dest;
        ActorId source;
        bool alive;

        Event() = default;

    };

    class IActor {
    public:
        virtual ~IActor() {}

        virtual int init() = 0;
        virtual int main() = 0;

        virtual void hasEvent(Event const *) = 0;
    };

    struct ActorProxy {
        uint64_t const _id;
        IActor *_this;
        void *const _handler;

        ActorProxy() : _id(0), _this(nullptr), _handler(nullptr) {}

        ActorProxy(uint64_t const id, IActor *actor, void *const handler)
                : _id(id), _this(actor), _handler(handler) {
        }
    };

    template<typename T>
    struct type {
        static void id() {}
    };

    template<typename T>
    std::size_t type_id() { return reinterpret_cast<size_t>(&type<T>::id); }

    template <typename _Data, typename ..._Init>
    struct CUBE_LOCKFREE_CACHELINE_ALIGNMENT TEvent : public _Data
    {
        TEvent() = delete;
		TEvent(TEvent &&) = default;
        TEvent(_Init ...init)
                : _Data(std::forward<_Init>(init)...)
        {}
    };
}

#endif //CUBE_IACTOR_H
