//
// Created by isnDev on 3/6/2019.
//

#include <qb/event.h>

#ifndef CUBE_MYEVENT_H
#define CUBE_MYEVENT_H

// Event example
struct MyEvent
        : public qb::Event // /!\ should inherit from qb event
{
    // ...
};

#endif //CUBE_MYEVENT_H
