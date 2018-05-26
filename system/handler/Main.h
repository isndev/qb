
#ifndef CUBE_MAIN_H
# define CUBE_MAIN_H
# include "Types.h"

namespace cube {
    
    template<typename ..._Core>
    class Main : public nocopy
			   , public TComposition<typename _Core::template Type<Main<_Core...>>::type...> {
        std::unordered_map<uint64_t, ActorProxy> _all_actor;

    public:
        //////// Static Const Data
        static const std::size_t linked_core;
        static const std::size_t total_core;

        //////// Types
        using parent_t = Main;
        using base_t = TComposition<typename _Core::template Type<Main>::type...>;

		Main()
			: base_t((typename _Core::template Type<Main>::type::parent_ptr_t)(this)...)
		{}

        //Todo : no thread safe need a lock or lockfree list
        // should be not accessible to users
        void addActor(ActorProxy const &actor) {
            //_all_actor.insert({actor._id, actor});
        }

        void removeActor(ActorId const &id) {
            //_all_actor.erase(id);
        }

        void send(CacheLine const *data, uint32_t const source, uint32_t const index, uint32_t const size) {
		    if (!this->each_or([data, source, index, size](auto &item) -> bool {
		        return item.receive_from_different_core(data, source, index, size);
		    })) {
		        // try to send to unknown core
		    }
        }
        /////////////////////////////////////////////////////

        // Start Sequence Usage

        template<std::size_t _CoreIndex, template<typename _Handler> typename _Actor, typename ..._Init>
        ActorId addActor(_Init const &...init) {
            ActorId id = ActorId::NotFound{};
            this->each_or([this, &id, &init...](auto &item) -> bool {
                id = item.template addActor<_CoreIndex, _Actor>(init...);

                return static_cast<bool>(id);
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

    //Init static data
    template<typename ..._Core>
    const std::size_t Main<_Core...>::linked_core = 0;
    template<typename ..._Core>
    const std::size_t Main<_Core...>::total_core = nb_core<Main, _Core...>();
}

#endif //CUBE_MAIN_H
