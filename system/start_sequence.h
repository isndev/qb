
#ifndef CUBE_START_SEQUENCE_H
# define CUBE_START_SEQUENCE_H
# include "Types.h"
# include "handler/PhysicalCore.h"
# include "handler/LinkedCore.h"
# include "handler/Main.h"

template<std::size_t _CoreIndex, typename _SharedData = void>
struct PhysicalCore {
    template<typename _Parent>
    struct Type {
        typedef cube::PhysicalCoreHandler<_Parent, _CoreIndex, _SharedData> type;
    };
//    using type = cube::PhysicalCoreHandler<_Parent, _CoreIndex, _SharedData>;

};

template<typename ..._Builder>
struct CoreLink {
    template<typename _Parent>
    struct Type {
        typedef cube::LinkedCoreHandler<_Parent, _Builder...> type;
    };
//    using type = cube::LinkedCoreHandler<_Parent, _Builder...>;
};



#endif //CUBE_START_SEQUENCE_H
