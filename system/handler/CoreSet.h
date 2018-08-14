
#ifndef CUBE_CORESETHANDLER_H
# define CUBE_CORESETHANDLER_H
# include "BaseHandler.h"

namespace cube {
    namespace handler {

        template<typename _ParentHandler, typename ..._Core>
        class CoreSetHandler
                : public BaseHandler<typename _Core::template Type<CoreSetHandler<_ParentHandler, _Core...>>::type...> {
            friend _ParentHandler;
        public:
            //////// Constexpr
            static const std::size_t nb_core;
            static const std::size_t linked_core;
            //////// Types
            typedef CoreSetHandler type;
            using base_t = BaseHandler<typename _Core::template Type<CoreSetHandler>::type...>;
            using parent_t = type;
            using parent_ptr_t = _ParentHandler *;

        private:
            _ParentHandler *_parent;

            bool receive_from_different_core(Event const &event, bool &ret) {
                return this->each_or([&event, &ret](auto &item) -> bool {
                    if (item._index != event.dest._index)
                        return false;
                    ret = item.receive_from_unlinked_core(event);
                    return true;
                });
            }

        public:
            CoreSetHandler() = delete;

            CoreSetHandler(_ParentHandler *parent)
                    : base_t((typename _Core::template Type<CoreSetHandler>::type::parent_ptr_t) (this)...),
                      _parent(parent) {}

            inline bool send(Event const &event) {
                return _parent->send(event);
            }

        };

        // not constexpr because of windows issue
        template<typename _ParentHandler, typename ..._Core>
        const std::size_t CoreSetHandler<_ParentHandler, _Core...>::nb_core = sizeof...(_Core);
        template<typename _ParentHandler, typename ..._Core>
        const std::size_t CoreSetHandler<_ParentHandler, _Core...>::linked_core = 0;
    }

    template<typename ..._Builder>
    struct CoreSet {
        template<typename _Parent>
        struct Type {
            typedef cube::handler::CoreSetHandler<_Parent, _Builder...> type;
        };
    };

    template <template <std::size_t> typename _Core
            , std::size_t N, std::size_t Offset = 0
            , typename Indices = std::make_index_sequence<N>>
    struct FixedCoreSet {
        template<typename _Parent>
        struct Type {
            struct S {
                template <std::size_t ...I>
                auto operator()(std::index_sequence<I...>) {
                    return cube::handler::CoreSetHandler<_Parent, _Core<I + Offset>...>(nullptr);
                }
            };

            using value_type = decltype(std::declval<Indices>());
            typedef typename std::invoke_result<S, value_type>::type type;
        };
    };
}

#endif //CUBE_CORESETHANDLER_H
