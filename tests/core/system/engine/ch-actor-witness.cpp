/**
 * SCRATCH, NEVER COMMITTED -- the challenger's qb-core witness (train 7, QB-1270 item 3).
 *
 * An actor that IS a standalone tcp client and ends its connection from its DESTRUCTOR, in the call (`disconnect_now()`):
 * the shape of qbm-redis's `~node_pool()` (`node.second->disconnect()` -> `drop_link_()` -> `disconnect_now()`). The
 * engine is stopped from outside (`qb::Main::stop()`), so the actor dies in its core's terminal teardown -- after
 * `listener.clear()` (core/VirtualCore.cpp:112-114).
 *
 *   QA  the engine runs on the caller's thread (`start(false)`): who holds the descriptor once start() has returned?
 *   QB  the engine runs on a worker thread: the same question after join().
 */
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <thread>

#include <gtest/gtest.h>

#include <qb/actor.h>
#include <qb/io/async.h>
#include <qb/io/protocol/text.h>
#include <qb/io/tcp/listener.h>
#include <qb/io/tcp/socket.h>
#include <qb/main.h>

#if __has_include(<qb/io/async/lingering_close.h>)
#include <qb/io/async/lingering_close.h>
#define CH_HAS_CLOSER 1
#endif

using namespace std::chrono_literals;

namespace ch_actor_witness_test {

std::atomic<long long> g_handle{-1};
std::atomic<int>       g_closing_after_destructor{-1};
std::atomic<int>       g_destroyed{0};

std::size_t
closing_now() {
#ifdef CH_HAS_CLOSER
    return qb::io::async::detail::closing_sockets();
#else
    return 0;
#endif
}

char const *
tree() {
#ifdef CH_HAS_CLOSER
    return "B";
#else
    return "base";
#endif
}

bool
descriptor_open(long long s) {
    int       type = 0;
    socklen_t len  = sizeof(type);
    return ::getsockopt(static_cast<::socket_type>(s), SOL_SOCKET, SO_TYPE, reinterpret_cast<char *>(&type), &len) == 0;
}

class Conn
    : public qb::Actor
    , public qb::io::use<Conn>::tcp::client<> {
    std::uint16_t _port;

public:
    using Protocol = qb::protocol::text::command<Conn>;

    explicit Conn(std::uint16_t port)
        : _port(port) {}

    ~Conn() {
        this->disconnect_now(); // the modules' shape: the teardown completes in the call
        g_closing_after_destructor = static_cast<int>(closing_now());
        ++g_destroyed;
    }

    qb::io::async::task<bool>
    onInit() override {
        if (this->transport().connect_v4("127.0.0.1", _port) != qb::io::SocketStatus::Done)
            co_return false;
        g_handle = static_cast<long long>(this->transport().native_handle());
        this->start();
        *this << std::string_view{"hello\n"};
        co_return true;
    }

    void
    on(Protocol::message &&) {}
    void
    on(qb::io::async::event::disconnected &&) {}
};

void
run_case(bool async, char const *name) {
    g_handle                   = -1;
    g_closing_after_destructor = -1;
    g_destroyed                = 0;
    qb::io::tcp::listener listener;
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    const std::uint16_t port = listener.local_endpoint().port();

    qb::Main engine;
    engine.addActor<Conn>(0, port);
    qb::io::tcp::socket peer;
    std::thread         stopper([&] {
        static_cast<void>(listener.accept(peer));
        std::this_thread::sleep_for(200ms);
        qb::Main::stop(); // the actor is alive: it dies in its core's terminal teardown
    });
    engine.start(async);
    engine.join();
    stopper.join();
    EXPECT_FALSE(engine.hasError());
    const long long   handle            = g_handle.load();
    const bool        open_after_join   = descriptor_open(handle);
    const std::size_t closing_on_caller = closing_now(); // this thread's loop
    std::this_thread::sleep_for(2500ms);                 // longer than linger(); the peer is silent, nothing runs this thread's loop
    const bool open_after_bound = descriptor_open(handle);
    peer.close();
    std::this_thread::sleep_for(100ms);
    const bool open_after_peer_closed = descriptor_open(handle);
    qb::io::async::run(EVRUN_NOWAIT);
    qb::io::async::run(EVRUN_NOWAIT);
    const bool open_after_a_pass = descriptor_open(handle);
    std::printf("CH-ACT %s [%s] destroyed=%d, closing nodes on the destroying thread right after the destructor=%d; after join(): "
                "descriptor open=%d, closing on the caller's thread=%zu; 2.5 s later: open=%d; after the peer closed: open=%d; after a pass "
                "of the caller's loop: open=%d closing=%zu\n",
                name, tree(), g_destroyed.load(), g_closing_after_destructor.load(), open_after_join ? 1 : 0, closing_on_caller,
                open_after_bound ? 1 : 0, open_after_peer_closed ? 1 : 0, open_after_a_pass ? 1 : 0, closing_now());
    EXPECT_FALSE(open_after_bound) << "the descriptor of an actor that is gone outlived linger(): the loop that holds it is not running";
    qb::io::async::listener::current.clear();
}

} // namespace ch_actor_witness_test

using namespace ch_actor_witness_test;

TEST(ChActor, QATheEngineRanOnTheCallersThread) {
    run_case(false, "QA start(false)");
}

TEST(ChActor, QBTheEngineRanOnAWorkerThread) {
    run_case(true, "QB start(true)");
}
