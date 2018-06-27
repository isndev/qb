
#ifndef CUBE_PHYSICALCORE_H
# define CUBE_PHYSICALCORE_H
# include "CoreBase.h"

namespace cube {
    namespace handler {

        template<std::size_t _CoreIndex, typename _ParentHandler, typename _SharedData>
        class PhysicalCoreHandler
                : public BaseCoreHandler<_CoreIndex,
                        _ParentHandler,
                        PhysicalCoreHandler<_CoreIndex, _ParentHandler, _SharedData>,
                        _SharedData> {
            friend _ParentHandler;
        public:
            using base_t = BaseCoreHandler<_CoreIndex, _ParentHandler, PhysicalCoreHandler, _SharedData>;

            PhysicalCoreHandler() = delete;

            PhysicalCoreHandler(_ParentHandler *parent)
                    : base_t(parent) {}

            inline bool onInit() const {
                return true;
            }

            inline void onCallback() const {}
        };

    }

    // Start sequence element
    template<std::size_t _CoreIndex, typename _SharedData = void>
    struct PhysicalCore {
        template<typename _Parent>
        struct Type {
            typedef cube::handler::PhysicalCoreHandler<_CoreIndex, _Parent, _SharedData> type;
        };
    };
}

#endif //CUBE_PHYSICALCORE_H
