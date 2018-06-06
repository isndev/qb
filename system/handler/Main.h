
#ifndef CUBE_MAIN_H
# define CUBE_MAIN_H
# include "BaseHandler.h"

namespace cube {
    
    template<typename ..._Core>
    class Main : public BaseHandler<typename _Core::template Type<Main<_Core...>>::type...> {
    public:
        //////// Static Const Data
        static const std::size_t linked_core;
        static const std::size_t total_core;

        //////// Types
        using parent_t = Main;
        using base_t = BaseHandler<typename _Core::template Type<Main>::type...>;
        /////////////////////////////////////////////////////
		Main()
			: base_t((typename _Core::template Type<Main>::type::parent_ptr_t)(this)...)
		{
		    LOG_INFO << "Init Main with " << total_core << " PhysicalCore(s)";
		}
        /////////////////////////////////////////////////////

        inline bool send(Event const &event) {
            bool ret = false;
            if (unlikely(!this->each_or([&event, &ret](auto &item) -> bool {
                return item.receive_from_different_core(event, ret);
            }))) {
                ret = true;
                LOG_WARN << "Core(" << event.source << ") failed to send event to nonexistent Core(" << event.dest._index << ")";
            }
            return ret;
        }

        // Start Sequence Usage
        template <std::size_t _CoreIndex, typename ..._Init>
        bool setSharedData(_Init &&...init) {
            return this->template __init_shared<_CoreIndex>(std::forward<_Init>(init)...);
        }

        bool start() {
            if (!this->__alloc__event())
                return false;
            if (!this->__init__actor())
                return false;
            //Todo : should return status
            this->__start();
            return true;
        }

        void join() {
            this->__join();
        }

    };

    // not constexpr because of windows issue
    template<typename ..._Core>
    const std::size_t Main<_Core...>::linked_core = 0;
    template<typename ..._Core>
    const std::size_t Main<_Core...>::total_core = nb_core<Main, _Core...>();
}

#endif //CUBE_MAIN_H
