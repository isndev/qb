
#ifndef CUBE_LINKEDCORE_H
# define CUBE_LINKEDCORE_H
# include "Types.h"

namespace cube {

    template<typename _ParentHandler, typename ..._Core>
    class LinkedCoreHandler
            : public nocopy
            , public TComposition<typename _Core::template Type<LinkedCoreHandler<_ParentHandler, _Core...>>::type...> {
        friend _ParentHandler;
    public:
        //////// Constexpr
        static const std::size_t nb_core;
        static const std::size_t linked_core;
        //////// Types
        typedef LinkedCoreHandler type;
        using base_t = TComposition<typename _Core::template Type<LinkedCoreHandler<_ParentHandler, _Core...>>::type...>;
        using parent_t = _ParentHandler;
        using parent_ptr_t = _ParentHandler *;
    private:
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

        bool receive_from_different_core(CacheLine const *data, uint32_t const source, uint32_t const index, uint32_t const size) {
            return this->each([data, source, index, size](auto &item) -> bool {
               if (!item.receive_from_different_core(data, source, index, size))
                   return false;
               return true;
            });
        }

        _ParentHandler *_parent;
    public:
        LinkedCoreHandler() = delete;
		LinkedCoreHandler(_ParentHandler *parent)
			: base_t((typename _Core::template Type<LinkedCoreHandler>::type::parent_ptr_t)(this)...)
			, _parent(parent) {}

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

        inline void send_to_different_core(CacheLine const *data, uint32_t const source, uint32_t const index, uint32_t const size) {
            _parent->send(data, source, index, size);
		}

        void send(CacheLine const *data, uint32_t const source, uint32_t const index, uint32_t const size) {
            if (this->each([&data, index, size](auto &item) -> bool {
                if (item._index == index) {
                    item.receive(data, size);
                    return false;
                }
                return true;
            }))
            {
                // not in same core
                send_to_different_core(data, source, index, size);
            }
        }

    };

    // Fuckin Windows
    template<typename _ParentHandler, typename ..._Core>
    const std::size_t LinkedCoreHandler<_ParentHandler, _Core...>::nb_core = sizeof...(_Core);
    template<typename _ParentHandler, typename ..._Core>
    const std::size_t LinkedCoreHandler<_ParentHandler, _Core...>::linked_core = sizeof...(_Core);

}

#endif //CUBE_LINKEDCORE_H
