#include <vector>
#include <cube/actor/actor.h>

#ifndef MYACTOR_H_
# define MYACTOR_H_

// Event example
struct MyEvent
        : public cube::Event // /!\ should inherit from cube event
{
    int data; // trivial data
    std::vector<int> container; // dynamic data
    // std::string str; /!\ avoid using stl string
    // instead use fixed cstring
    // or compile with old ABI '-D_GLIBCXX_USE_CXX11_ABI=0'
};

class MyActor
        : public cube::Actor // /!\ should inherit from cube actor
        , public cube::ICallback // (optional) required to register actor callback
{
public:
    MyActor() = default;
    MyActor(int, int ) {} // constructor with parameters

    ~MyActor() {}

    // will call this function before adding MyActor
    virtual bool onInit() override final {
        this->template registerEvent<MyEvent> (*this);          // will listen MyEvent
        this->registerCallback(*this);                          // each core loop will call onCallback

        // ex: just send MyEvent to myself ! forever alone ;(
        auto &event = this->template push<MyEvent>(this->id()); // and keep a reference to the event
        event.data = 1337;                                      // set trivial data
        event.container.push_back(7331);
        return true;                                            // init ok, MyActor will be added
    }

    // will call this function each core loop
    virtual void onCallback() override final {
        // ...
    }

    // will call this function when MyActor received MyEvent
    void on(MyEvent const &) {
        // I am a dummy actor, notify the engine to remove me !
        cube::io::cout() << "MyActor(" << this->id() << ") received MyEvent and will Die";
        this->kill();
    }
};

#endif