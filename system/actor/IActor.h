
#ifndef CUBE_IACTOR_H
# define CUBE_IACTOR_H
# include <iostream>
# include <utility>
# include <chrono>

# include "utils/prefix.h"

namespace cube {
    using namespace std::chrono;
    struct ActorId {
    private:
        ActorId(uint32_t, uint32_t) : _id{}, _index(0) {}

    public:
        static const ActorId NotFound;
    public:
        uint32_t _id;
        uint32_t _index;
    public:
        ActorId() : _id(static_cast<uint32_t >(duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count())) {
        }

        ActorId(uint64_t const id) {
            *reinterpret_cast<uint64_t *>(this) = id;
        }

        ActorId(ActorId const &) = default;

        inline operator const uint64_t &() const {
            return *reinterpret_cast<uint64_t const *>(this);
        }

        inline operator bool() const {
            return static_cast<uint64_t >(*this) != static_cast<uint64_t >(NotFound);
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
        TEvent(_Init ...init)
                : _Data(std::forward<_Init>(init)...)
        {}
    };
}

std::ostream &operator<<(std::ostream &os, cube::ActorId const &id);

#endif //CUBE_IACTOR_H
