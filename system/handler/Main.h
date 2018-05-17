
#ifndef CUBE_MAIN_H
# define CUBE_MAIN_H
# include "Types.h"

namespace cube {

    template<typename ..._Core>
    class Main : TComposition<typename _Core::template type<Main<_Core...>>...> {
        using base_t = TComposition<typename _Core::template type<Main>...>;
        std::unordered_map<uint64_t, ActorProxy> _all_actor;

    public:
        Main() : base_t(typename _Core::template type<Main>(this)...) {
        }

        //Todo : no thread safe need a lock or lockfree list
        // should be not accessible to users
        void addActor(ActorProxy const &actor) {
            //_all_actor.insert({actor._id, actor});
        }

        void removeActor(ActorId const &id) {
            //_all_actor.erase(id);
        }

        void send(CacheLine const *data, uint32_t const index, uint32_t const size) {
        }
        /////////////////////////////////////////////////////

        // Start Sequence Usage

        template<std::size_t _CoreIndex, template<typename _Handler> typename _Actor, typename ..._Init>
        ActorId addActor(_Init const &...init) {
            ActorId id = ActorId::NotFound{};
            this->each([this, &id, &init...](auto &item) -> int {
                id = item.template addActor<_CoreIndex, _Actor>(init...);
                if (id)
                    return 0;
                return 1;
            });
            return id;
        }

        void start() {
            this->each([](auto &item) -> bool {
                item.__alloc__event();
                return true;
            });
            this->each([](auto &item) -> bool {
                item.__start();
                return true;
            });
        }

        void join() {
            this->each([](auto &item) -> bool {
                item.__join();
                return true;
            });
        }

    };

}

#endif //CUBE_MAIN_H
