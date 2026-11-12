// cv_ask.cpp -- SCRATCH witness (confirmation): exit() from a coroutine that an `ask` reply resumed. Never committed.
//
//   cv_ask <mode>
//     exit_after_ask   one core on the calling thread; a spawned coroutine awaits `qb::ask(...)`; the responder
//                      answers; the reply RESUMES THE FRAME INLINE, from the handler that routed it
//                      (ask_awaiter::deliver_thunk -> h.resume(), no run_ready() on the stack); the coroutine then
//                      calls std::exit(0) from its body
//     exit_after_sleep the same actor and the same bystander, but the coroutine is resumed by the scheduler's drain
//                      (a sleep), then calls std::exit(0): the control
//
// The thread's listener is destroyed from inside the coroutine either way; what differs is whether the scheduler's
// own run_ready() is on the stack when it happens.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <qb/actor.h>
#include <qb/io.h>
#include <qb/main.h>
#include <qb/patterns.h>

using namespace std::chrono_literals;

namespace cv_ask {

int g_frame_dtors = 0;

struct frame_local {
    char const *name;
    ~frame_local() {
        ++g_frame_dtors;
        std::fprintf(stderr, "[cv_ask] coroutine local '%s' destroyed\n", name);
    }
};

struct judge {
    std::size_t tally0;
    ~judge() {
        std::fprintf(stderr, "[cv_ask] judge (last static destroyed): frame_dtors=%d tally+%zu\n", g_frame_dtors,
                     qb::io::async::abandoned_coroutine_frames_total() - tally0);
    }
};

struct Question : qb::Request<int> {};

class Echo : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Question>(*this);
        co_return true;
    }
    void
    on(Question &q) {
        qb::answer(*this, q, [](Question const &) { return 42; });
    }
};

class Asker : public qb::Actor {
    qb::ActorId _target;
    bool        _after_ask;

public:
    Asker(qb::ActorId target, bool after_ask)
        : _target(target)
        , _after_ask(after_ask) {}

    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Question>(*this);
        auto       target    = _target;
        const bool after_ask = _after_ask;
        spawn([](qb::ScopedCoroContext) -> qb::io::async::task<void> {
            frame_local l{"bystander"};
            co_await qb::io::async::sleep(5s);
        });
        spawn([target, after_ask](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            frame_local l{"asker"};
            if (after_ask) {
                (void) co_await qb::ask(ctx, target, Question{}, 5s);
                std::fprintf(
                    stderr, "[cv_ask] the coroutine got its reply -- resumed inline by the handler that routed it -- and calls std::exit(0)\n");
            } else {
                co_await qb::io::async::sleep(20ms);
                std::fprintf(stderr, "[cv_ask] the coroutine, resumed by the scheduler's drain, calls std::exit(0)\n");
            }
            std::exit(0);
        });
        co_return true;
    }
    void
    on(Question &q) {
        (void) resolve_ask(q);
    }
};

} // namespace cv_ask

int
main(int argc, char **argv) {
    using namespace cv_ask;
    const std::string mode = argc > 1 ? argv[1] : "exit_after_ask";
    static judge      j{qb::io::async::abandoned_coroutine_frames_total()};
    qb::Main          engine;
    auto              echo = engine.addActor<Echo>(0);
    engine.addActor<Asker>(0, echo, mode == "exit_after_ask");
    engine.start(false);
    engine.join();
    std::fprintf(stderr, "[cv_ask] NOT REACHED: the engine returned\n");
    return 4;
}
