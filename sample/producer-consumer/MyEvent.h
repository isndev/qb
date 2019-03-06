//
// Created by isnDev on 3/6/2019.
//

#include <cube/event.h>

#ifndef CUBE_MYEVENT_H
#define CUBE_MYEVENT_H

// Event example
struct MyEvent
        : public qb::Event // /!\ should inherit from cube event
{
    // ...
};

#endif //CUBE_MYEVENT_H
