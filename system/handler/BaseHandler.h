
#ifndef CUBE_BASEHANDLER_H
#define CUBE_BASEHANDLER_H
# include "Types.h"

namespace cube {

    template<typename ..._Core>
    class BaseHandler
            : public nocopy, public TComposition<_Core...> {
        using base_t = TComposition<_Core...>;
    public:
        // Start Sequence Usage
        void __init__shared_data() {
            this->each([](auto &item) -> bool {
                item.__init__shared_data();
                return true;
            });
        }

        void __init__actors() {
            this->each([](auto &item) -> bool {
                item.__init__actors();
                return true;
            });
        }

        template<std::size_t _CoreIndex, typename ..._Init>
        bool __init_shared(_Init &&...init) {
            return this->each_or([&init...](auto &item) -> bool {
                return item.template __init_shared<_CoreIndex>(std::forward<_Init>(init)...);
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

        bool receive_from_different_core(CacheLine const *data, uint32_t const source, uint32_t const index,
                                           uint32_t const size) {
            return this->each_or([data, source, index, size](auto &item) -> bool {
                return item.receive_from_different_core(data, source, index, size);
            });
        }

        BaseHandler() = delete;

        template<typename ..._Init>
        BaseHandler(_Init &&...init)
                : base_t(std::forward<_Init>(init)...) {}

    public:
        template< std::size_t _CoreIndex
                , template<typename _Handler> typename _Actor
                , typename ..._Init >
        ActorId addActor(_Init &&...init) {
            ActorId id = ActorId::NotFound{};
            this->each_or([this, &id, &init...](auto &item) -> bool {
                id = item.template addActor<_CoreIndex, _Actor, _Init...>
                        (std::forward<_Init>(init)...);

                return static_cast<bool>(id);
            });
            return id;
        }

        template< std::size_t _CoreIndex
                , template <typename _Handler, typename _Trait> typename _Actor
                , typename _Trait
                , typename ..._Init >
        ActorId addActor(_Init &&...init) {
            ActorId id = ActorId::NotFound{};
            this->each_or([this, &id, &init...](auto &item) -> bool {
                id = item.template addActor<_CoreIndex, _Actor, _Trait, _Init...>
                        (std::forward<_Init>(init)...);

                return static_cast<bool>(id);
            });
            return id;
        }
    };

}

#endif //CUBE_BASEHANDLER_H
