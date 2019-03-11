// MyEvent.h
#include <vector>
#include <qb/event.h>
#ifndef MYEVENT_H_
# define MYEVENT_H_
// Event example
struct MyEvent
        : public qb::Event // /!\ should inherit from qb event
{
    int data; // trivial data
    std::vector<int> container; // dynamic data
    // /!\ an event must never store an address of it own data
    // /!\ ex : int *ptr = &data;
    // /!\ avoid using std::string, instead use :
    // /!\ - fixed cstring
    // /!\ - pointer of std::string
    // /!\ - or compile with old ABI '-D_GLIBCXX_USE_CXX11_ABI=0'
};
#endif