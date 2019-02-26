////////////////////////////////////////////////////////////
/// \mainpage
///
/// \section welcome Welcome
/// Welcome to the official CUBE documentation. Here you will find a detailed
/// view of all the CUBE classes, functions and Actors developed by us. <br/>
/// More tutorials will arrive soon.
///
/// \section example Short example
/// Here is a short example, to show you how simple to use CUBE :
///
/// \code
/////MyActor.h
///#include <vector>
///#include <cube/actor.h>
///
///#ifndef MYACTOR_H_
///# define MYACTOR_H_
///
///// Event example
///struct MyEvent
///        : public cube::Event // /!\ should inherit from cube event
///{
///    int data; // trivial data
///    std::vector<int> container; // dynamic data
///    // std::string str; /!\ avoid using stl string
///    // instead use fixed cstring
///    // or compile with old ABI '-D_GLIBCXX_USE_CXX11_ABI=0'
///};
///
///class MyActor
///        : public cube::Actor // /!\ should inherit from cube actor
///                , public cube::ICallback // (optional) required to register actor callback
///{
///public:
///    MyActor() = default;
///    MyActor(int, int ) {} // constructor with parameters
///
///    ~MyActor() {}
///
///    // will call this function before adding MyActor
///    virtual bool onInit() override final {
///        this->template registerEvent<MyEvent> (*this);          // will listen MyEvent
///        this->registerCallback(*this);                          // each core loop will call onCallback
///
///        // ex: just send MyEvent to myself ! forever alone ;(
///        auto &event = this->template push<MyEvent>(this->id()); // and keep a reference to the event
///        event.data = 1337;                                      // set trivial data
///        event.container.push_back(7331);
///        return true;                                            // init ok, MyActor will be added
///    }
///
///    // will call this function each core loop
///    virtual void onCallback() override final {
///        // ...
///    }
///
///    // will call this function when MyActor received MyEvent
///    void on(MyEvent const &) {
///        // I am a dummy actor, notify the engine to remove me !
///        cube::io::cout() << "MyActor(" << this->id() << ") received MyEvent and will Die";
///        this->kill();
///    }
///};
///
///#endif
///// main.cpp
///#include "MyActor.h"
///#include <cube/main.h>
///
///int main (int, char *argv[]) {
///    // (optional) initialize the logger
///    cube::io::log::init(argv[0]); // filepath
///    cube::io::log::setLevel(cube::io::log::Level::WARN); // log only warning, error an critical
///    // usage
///    LOG_INFO << "I will not be logged :(";
///
///    // configure the Engine
///    // Note : I will use only the core 0 and 1
///    cube::Main main({0, 1});
///
///    // My start sequence -> add MyActor to core 0 and 1
///    main.addActor<MyActor>(0); // default constructed
///    main.addActor<MyActor>(1, 1337, 7331); // constructed with parameters
///
///    main.start();  // start the engine asynchronously
///    main.join();   // Wait for the running engine
///    // if all my actors had been destroyed then it will release the wait !
///    return 0;
///}
/// \endcode
////////////////////////////////////////////////////////////