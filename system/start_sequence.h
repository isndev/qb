
#ifndef CUBE_START_SEQUENCE_H
# define CUBE_START_SEQUENCE_H
# include "Types.h"
# include "handler/PhysicalCore.h"
# include "handler/TimedCore.h"
# include "handler/CoreLink.h"
# include "handler/Main.h"

template<std::size_t _CoreIndex, typename _SharedData = void>
struct PhysicalCore {
    template<typename _Parent>
    struct Type {
        typedef cube::PhysicalCoreHandler<_CoreIndex, _Parent, _SharedData> type;
    };
//    using type = cube::PhysicalCoreHandler<_Parent, _CoreIndex, _SharedData>;

};

template<std::size_t _CoreIndex, typename _SharedData = void>
struct TimedCore {
    template<typename _Parent>
    struct Type {
        typedef cube::TimedCoreHandler<_CoreIndex,_Parent, _SharedData> type;
    };
};

template<typename ..._Builder>
struct CoreLink {
    template<typename _Parent>
    struct Type {
        typedef cube::CoreLinkHandler<_Parent, _Builder...> type;
    };
//    using type = cube::CoreLinkHandler<_Parent, _Builder...>;
};


#endif //CUBE_START_SEQUENCE_H
