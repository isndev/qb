
#ifndef CUBE_IACTOR_H
# define CUBE_IACTOR_H
# include "Event.h"

namespace cube {

    class IActor {
    public:
        virtual ~IActor() {}

        virtual int init() = 0;
        virtual int main() = 0;
        virtual void hasEvent(Event const *) = 0;
    };

}

#endif //CUBE_IACTOR_H
