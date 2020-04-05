//
// Created by isnDev on 4/1/2020.
//

#include <qb/event.h>
#include <qb/string.h>

#ifndef QB_CMDEVENT_H
#    define QB_CMDEVENT_H

struct CommandEvent : public qb::Event {
    qb::string<4077> message;
};

// static_assert(sizeof(CommandEvent) != 4096)

#endif // QB_CMDEVENT_H
