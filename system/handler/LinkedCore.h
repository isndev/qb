
#ifndef CUBE_LINKEDCORE_H
# define CUBE_LINKEDCORE_H
# include "Types.h"

namespace cube {

    template<typename _ParentHandler, typename ..._Core>
    class LinkedCoreHandler
            : public TComposition<typename _Core::template type<LinkedCoreHandler<_ParentHandler, _Core...>>...> {
        using base_t = TComposition<typename _Core::template type<LinkedCoreHandler>...>;
        friend _ParentHandler;

        // Start Sequence Usage
        void __alloc__event() {
            this->each([](auto &item) -> bool {
                item.__alloc__event();
                return true;
            });
        }

        void __start() {
            this->each([](auto &item) -> bool {
                item.__start();
                return true;
            });
        }

        void __join() {
            this->each([](auto &item) -> bool {
                item.__join();
                return true;
            });
        }

        _ParentHandler *_parent;
    public:
        LinkedCoreHandler() = delete;

        LinkedCoreHandler(LinkedCoreHandler const &rhs)
                : base_t(typename _Core::template type<LinkedCoreHandler>(this)...), _parent(rhs._parent) {
        }

        LinkedCoreHandler(_ParentHandler *parent)
                : base_t(typename _Core::template type<LinkedCoreHandler>(this)...), _parent(parent) {}

        void addActor(ActorProxy const &actor) {
            _parent->addActor(actor);
        }

        void removeActor(ActorId const &id) {
            _parent->removeActor(id);
        }

        template<std::size_t _CoreIndex, template<typename _Handler> typename _Actor, typename ..._Init>
        ActorId addActor(_Init const &...init) {
            ActorId id = ActorId::NotFound{};
            this->each([this, &id, &init...](auto &item) -> int {
                id = item.template addActor<_CoreIndex, _Actor, _Init...>(init...);
                if (id)
                    return 0;
                return 1;
            });
            return id;
        }

        void send(CacheLine const *data, uint32_t const index, uint32_t const size) {
            this->each([&data, index, size](auto &item) -> bool {
                if (item._index == index) {
                    item.receive(data, size);
                    return false;
                }
                return true;
            });
        }

    };

}

#endif //CUBE_LINKEDCORE_H
