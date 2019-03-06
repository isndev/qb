////////////////////////////////////////////////////////////
/// \mainpage
///
/// \section welcome Welcome
/// Welcome to the official QB documentation. Here you will find a detailed
/// view of all the QB classes, functions and Actors developed by us. <br/>
/// More tutorials will arrive soon.
///
/// \section example Short example
/// Here is a short example, to show you how simple to use QB\n
///
/// My First Event :
/// \code
///// MyEvent.h
///// Event example
///
///struct MyEvent
///        : public qb::Event // /!\ should inherit from cube event
///{
///    int data; // trivial data
///    std::vector<int> container; // dynamic data
///    // std::string str; /!\ avoid using stl string
///    // instead use fixed cstring
///    // or compile with old ABI '-D_GLIBCXX_USE_CXX11_ABI=0'
///
///    MyEvent() = default;
///    MyEvent(int param)
///            : data(param) {}
///};
/// \endcode
/// My First Actor :
/// \code
/////MyActor.h
///#include <vector>
///#include <cube/actor.h>
///
///#ifndef MYACTOR_H_
///# define MYACTOR_H_
///# include "MyEvent.h"
///
///class MyActor
///        : public qb::Actor     // /!\ should inherit from cube actor
///                , public qb::ICallback // (optional) required to register actor callback
///{
///public:
///    MyActor() = default;         // default constructor
///    MyActor(int, int ) {}        // constructor with parameters
///
///    ~MyActor() {}
///
///    // will call this function before adding MyActor
///    virtual bool onInit() override final {
///        registerEvent<MyEvent>(*this);     // will listen MyEvent
///        registerCallback(*this);           // each core loop will call onCallback
///
///        // ex: just send MyEvent to myself ! forever alone ;(
///        auto &event = push<MyEvent>(id()); // and keep a reference to the event
///        event.data = 1337;                 // set trivial data
///        event.container.push_back(7331);   // set dynamic data
///
///        // other wait to send chain event setting data using constructors
///        to(id())
///                .push<MyEvent>()
///                .push<MyEvent>(7331);
///        return true;                       // init ok, MyActor will be added
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
///        qb::io::cout() << "MyActor(" << id() << ") received MyEvent and will Die" << std::endl;
///        kill(); // /!\ after this line MyActor is not able to receive events
///    }
///};
///
///#endif
/// \endcode
/// My first program using Cube :
/// \code
///// main.cpp
///#include <cube/main.h>
///#include "MyActor.h"
///
///int main (int, char *argv[]) {
///    // (optional) initialize the logger
///    qb::io::log::init(argv[0]); // filepath
///    qb::io::log::setLevel(qb::io::log::Level::WARN); // log only warning, error an critical
///    // usage
///    LOG_INFO << "I will not be logged :(";
///
///    // configure the Engine
///    // Note : I will use only the core 0 and 1
///    qb::Main main({0, 1});
///
///    // First way to add actors at start
///    main.addActor<MyActor>(0); // in Core id=0, default constructed
///    main.addActor<MyActor>(1, 1337, 7331); // in Core id=1, constructed with parameters
///
///    // Other way to add actors retrieving core builder
///    main.core(0)
///            .addActor<MyActor>()
///            .addActor<MyActor>(1337, 7331);
///
///    main.start();  // start the engine asynchronously
///    main.join();   // Wait for the running engine
///    // if all my actors had been destroyed then it will release the wait !
///    return 0;
///}
/// \endcode
////////////////////////////////////////////////////////////