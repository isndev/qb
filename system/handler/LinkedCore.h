
#ifndef CUBE_LINKEDCORE_H
# define CUBE_LINKEDCORE_H
# include "BaseHandler.h"

namespace cube {

    template<typename _ParentHandler, typename ..._Core>
    class LinkedCoreHandler
            : public BaseHandler<typename _Core::template Type<LinkedCoreHandler<_ParentHandler, _Core...>>::type...> {
        friend _ParentHandler;
    public:
        //////// Constexpr
        static const std::size_t nb_core;
        static const std::size_t linked_core;
        //////// Types
        typedef LinkedCoreHandler type;
        using base_t = BaseHandler<typename _Core::template Type<LinkedCoreHandler>::type...>;
        using parent_t = _ParentHandler;
        using parent_ptr_t = _ParentHandler *;

    private:
        _ParentHandler *_parent;
    public:
        LinkedCoreHandler() = delete;
		LinkedCoreHandler(_ParentHandler *parent)
			: base_t((typename _Core::template Type<LinkedCoreHandler>::type::parent_ptr_t)(this)...)
			, _parent(parent) {}

        inline void send_to_different_core(CacheLine const *data, uint32_t const source, uint32_t const index, uint32_t const size) {
            _parent->send(data, source, index, size);
		}

        void send(CacheLine const *data, uint32_t const source, uint32_t const index, uint32_t const size) {
            if (!this->each_or([&data, index, size](auto &item) -> bool {
				if (item._index != index)
					return false;
                item.receive(data, size);
                return true;
            })) {
                send_to_different_core(data, source, index, size);
            }
        }
    };

    // not constexpr because of windows issue
    template<typename _ParentHandler, typename ..._Core>
    const std::size_t LinkedCoreHandler<_ParentHandler, _Core...>::nb_core = sizeof...(_Core);
    template<typename _ParentHandler, typename ..._Core>
    const std::size_t LinkedCoreHandler<_ParentHandler, _Core...>::linked_core = sizeof...(_Core);

}

#endif //CUBE_LINKEDCORE_H
