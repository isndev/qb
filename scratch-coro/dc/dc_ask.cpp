// dc_ask.cpp -- SCRATCH witness: an `ask` still pending when its engine goes. Never committed.
//
//   dc_ask <mode>
//     one    start(false): one core on the calling thread; the asked actor never answers; the engine is stopped
//            30 ms later with the ask parked. ~VirtualCore -> recycle_coro_scheduler() must destroy the frame.
//     two    the same engine twice on the same thread (the restart path: the thread keeps its listener and scheduler)
//     cross  start(): the asker on core 0, the mute responder on core 1, both on their own threads
//
// Prints what happened to the asking coroutine's frame; the exit code carries LeakSanitizer's exit check.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>

#include <qb/actor.h>
#include <qb/io.h>
#include <qb/main.h>
#include <qb/patterns.h>

using namespace std::chrono_literals;

namespace dc_ask {

std::atomic<int> g_frame_dtors{0};
std::atomic<int> g_completed{0};
std::atomic<int> g_unwound{0};

struct frame_local {
    ~frame_local() {
        g_frame_dtors.fetch_add(1);
    }
};

struct Question : qb::Request<int> {};

class Mute : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Question>(*this);
        co_return true;
    }
    void
    on(Question &) {} // never answers
};

class Asker : public qb::Actor {
    qb::ActorId _target;

public:
    explicit Asker(qb::ActorId target)
        : _target(target) {}

    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Question>(*this);
        auto target = _target;
        spawn([target](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            frame_local l;
            try {
                (void) co_await qb::ask(ctx, target, Question{}, 5s);
                g_completed.fetch_add(1);
            } catch (...) {
                g_unwound.fetch_add(1); // cancelled or timed out: the frame unwound by itself
            }
        });
        spawn_detached([](qb::CoroContext) -> qb::io::async::task<void> {
            co_await qb::io::async::sleep(30ms);
            qb::Main::stop(); // the ask is still parked
        });
        co_return true;
    }
    void
    on(Question &q) {
        (void) resolve_ask(q);
    }
};

int
run(bool threaded, bool cross) {
    qb::Main engine;
    auto     mute = engine.addActor<Mute>(cross ? 1 : 0);
    engine.addActor<Asker>(0, mute);
    engine.start(threaded);
    engine.join();
    return engine.hasError() ? 1 : 0;
}

} // namespace dc_ask

int
main(int argc, char **argv) {
    using namespace dc_ask;
    const std::string mode   = argc > 1 ? argv[1] : "one";
    const long        live0  = qb::io::async::detail::CoroutineFrameAllocator::live_frames;
    const std::size_t tally0 = qb::io::async::abandoned_coroutine_frames_total();
    int               rc     = 0;
    if (mode == "one")
        rc = run(false, false);
    else if (mode == "two")
        rc = run(false, false) + run(false, false);
    else if (mode == "cross")
        rc = run(true, true);
    else
        return 2;
    std::printf("ask %s: engine rc=%d frame_dtors=%d completed=%d unwound=%d live_frames(main)%+ld tally+%zu\n", mode.c_str(), rc,
                g_frame_dtors.load(), g_completed.load(), g_unwound.load(), qb::io::async::detail::CoroutineFrameAllocator::live_frames - live0,
                qb::io::async::abandoned_coroutine_frames_total() - tally0);
    return rc;
}
