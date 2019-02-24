#include <sys/stat.h>
#include <fcntl.h>

//#include "assert.h"
//#include "cube.h"
//#include "service/iopoll/actor.h"
#include "network/include/cube/network/ip.h"

//using namespace cube::service::iopoll;
//
//template <typename Handler>
//class ClientTest
//        : public cube::Actor<Handler>
//        , cube::network::SocketTCP
//{
//public:
//    bool onInit() override final {
//        this->template registerEvent<event::Ready>(*this);
//        auto &e = this->template push<event::Subscribe>(cube::Tag<Actor, 0>::id());
//        e.ep_event.data.fd = this->getNative();
//        e.ep_event.events = EPOLLIN | EPOLLOUT | EPOLLONESHOT;
//        return true;
//    }
//
//    ClientTest() = delete;
//    ClientTest(SocketTCP const &socket)
//        : SocketTCP(socket)
//    {}
//
//    void on(event::Ready &event) {
//        if (event.ep_event.events & EPOLLIN) {
//            char buffer[256];
//            std::size_t received = 0;
//            if (SocketTCP::receive(buffer, 256, received) == cube::network::Socket::Done) {
//                std::cout << "Received: " << buffer;
//                event.ep_event.events |= EPOLLOUT | EPOLLONESHOT;
//                event.proxy.ctl(event.ep_event);
//            } else {
//                SocketTCP::disconnect();
//                auto &e = this->template push<event::Unsubscribe>(cube::Tag<Actor, 0>::id());
//                e.ep_event = event.ep_event;
//                this->kill();
//            }
//        }
//        else if (event.ep_event.events & EPOLLOUT) {
//            LOG_DEBUG << "CLIENT READY TO WRITE";
//            SocketTCP::send("HEARTBEAT\n", 10);
//            event.ep_event.events |= EPOLLIN | EPOLLONESHOT;
//            event.proxy.ctl(event.ep_event);
//        }
//        else {
//            LOG_DEBUG << "UNKNOWN EVENT";
//            this->kill();
//        }
//    }
//};
//
//template <typename Handler>
//class ServerTest
//        : public cube::Actor<Handler>
//{
//    cube::network::Listener server;
//public:
//    ServerTest() = default;
//
//    bool onInit() override final {
//        server.listen(12354);
//        server.setBlocking(false);
//
//        if (!server.good()) {
//            this->kill();
//            return false;
//        }
//
//        this->template registerEvent<event::Ready>(*this);
//        auto &e = this->template push<event::Subscribe>(cube::Tag<Actor, 0>::id());
//        e.ep_event.data.fd = server.getNative();
//        e.ep_event.events = EPOLLIN | EPOLLONESHOT;
//        return true;
//    }
//
//    // MyEvent call back
//    void on(event::Ready &event) {
//        cube::network::SocketTCP socket;
//        if (server.accept(socket) == cube::network::Socket::Done) {
//            this->template addRefActor<ClientTest>(socket);
//            event.ep_event.events |= EPOLLONESHOT;
//            event.proxy.ctl(event.ep_event);
//        } else
//            LOG_DEBUG << "Failed to accept";
//    }
//
//};

int main() {
//    nanolog::initialize(nanolog::GuaranteedLogger(), "./log/", "test-poller.log", 1024);
//    nanolog::set_log_level(nanolog::LogLevel::DEBUG);
//
//    test<1>("Test poller event", []() {
//        cube::Engine<cube::PhysicalCore<0>, cube::PhysicalCore<1> > main;
//
//        main.addActor<0, Actor>();
//        main.addActor<1, ServerTest>();
//
//        main.start();
//        main.join();
//        return 0;
//    });
//
//
    std::cout << cube::network::ip("127.0.0.1").toString() << std::endl;
    std::cout << cube::network::ip("localhost").toString() << std::endl;
    std::cout << cube::network::ip("server.isndev.com").toString() << std::endl;
    return 0;
}
