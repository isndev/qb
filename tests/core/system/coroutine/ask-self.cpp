/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/coroutine/ask-self.cpp
 * @brief An `ask` an actor addresses to ITSELF (Huly QB-1100): its handler answers it, in every form, and
 *        a self-ask from `onInit()` is refused at once.
 *
 * The request of a self-ask reaches the asker's own `on(E&)` carrying the correlation id of the asker's
 * own pending ask. `resolve_ask` used to match it there and resume the ask with the REQUEST as its reply:
 * the handler never ran and the asker read an unanswered envelope (`response` still 0). Every answer
 * below is non-zero -- key * 10 + 1 from the asker's own handler, key * 10 + 2 from the peer -- so each
 * case checks the exact envelope and the handler counts.
 *
 * The forms: `ask` by value and emplace, `ask_by` by value and emplace, `ask_all` (unbounded and bounded)
 * and `ask_any` over targets that include the caller, and `ask_stream`. The answers: `qb::answer` (a
 * `reply()`), a handler that checks `resolve_ask` before `qb::answer` checks it again, a COPY of the
 * request pushed back, and a request forwarded to the peer, which answers it. And the activation gate: a
 * self-ask from `onInit()` -- before and after its first suspension -- fails at once with
 * `std::logic_error`, since the handlers cannot run before `onInit()` completes.
 */

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/core/patterns.h>
#include <qb/io/async.h>
#include <qb/main.h>

using namespace std::chrono_literals;

namespace ask_self_test {

/// The exchange: `key` in, `response` out.
struct Echo : qb::Request<int> {
    int key{0};
    Echo() = default;
    explicit Echo(int k)
        : key(k) {}
};

/// The streaming exchange.
struct EchoStream : qb::StreamRequest<int> {
    int key{0};
    EchoStream() = default;
    explicit EchoStream(int k)
        : key(k) {}
};

struct Finish : qb::Event {};

/// What one run saw.
struct Outcome {
    std::atomic<int>  handled{0};      ///< requests the asker's own handler answered
    std::atomic<int>  peer_handled{0}; ///< requests the peer answered
    std::vector<int>  responses;       ///< every envelope the ask resolved with (core thread only)
    std::string       error;           ///< the exception the ask ended with, if any
    std::atomic<bool> finished{false}; ///< the asking coroutine reached its end
    long long         elapsed_ms = -1; ///< how long the ask took (the activation cases)
};
inline Outcome g;

void
reset() {
    g.handled      = 0;
    g.peer_handled = 0;
    g.responses.clear();
    g.error.clear();
    g.finished   = false;
    g.elapsed_ms = -1;
}

/// Answers `Echo` with key * 10 + 2 (by `reply()`) and streams key * 10 + 2 once.
class Peer : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Echo>(*this);
        registerEvent<EchoStream>(*this);
        co_return true;
    }
    void
    on(Echo &e) {
        qb::answer(*this, e, [](Echo const &r) {
            ++g.peer_handled;
            return r.key * 10 + 2;
        });
    }
    void
    on(EchoStream &e) {
        if (resolve_ask(e))
            return;
        ++g.peer_handled;
        qb::yield_answer(*this, e, e.key * 10 + 2);
        qb::end_stream(*this, e);
    }
};

enum class form { ask_value, ask_emplace, ask_by_value, ask_by_emplace, ask_all, ask_all_bounded, ask_any, ask_stream };
/// How the asker's own handler answers.
enum class answer_by { answer, resolve_then_answer, copy, forward_to_peer };

/// Runs one form of the self-ask and records what it resolved with.
qb::io::async::task<void>
ask_once(qb::ScopedCoroContext ctx, form f, qb::ActorId self, qb::ActorId peer) {
    switch (f) {
        case form::ask_value:
            g.responses.push_back((co_await qb::ask(ctx, self, Echo{7}, 500ms)).response);
            break;
        case form::ask_emplace:
            g.responses.push_back((co_await qb::ask<Echo>(ctx, self, 500ms, 7)).response);
            break;
        case form::ask_by_value:
            g.responses.push_back((co_await qb::ask_by(ctx, self, Echo{7}, qb::deadline_in(ctx, 500ms))).response);
            break;
        case form::ask_by_emplace:
            g.responses.push_back((co_await qb::ask_by<Echo>(ctx, self, qb::deadline_in(ctx, 500ms), 7)).response);
            break;
        case form::ask_all:
            for (auto const &r : co_await qb::ask_all(ctx, {peer, self}, Echo{7}, 500ms))
                g.responses.push_back(r.response);
            break;
        case form::ask_all_bounded:
            for (auto const &r : co_await qb::ask_all(ctx, {self, peer}, Echo{7}, 500ms, 1))
                g.responses.push_back(r.response);
            break;
        case form::ask_any:
            g.responses.push_back((co_await qb::ask_any(ctx, {self, peer}, Echo{7}, 500ms)).response);
            break;
        case form::ask_stream: {
            auto s = qb::ask_stream(ctx, self, EchoStream{7}, 500ms);
            while (auto c = co_await s.next())
                g.responses.push_back(c->chunk);
            break;
        }
    }
}

/// Answers `Echo` with key * 10 + 1 (as `how` says) and streams key * 10 + 1 once, and asks ITSELF in one form.
class SelfAsker : public qb::Actor {
    form        _form;
    answer_by   _how;
    qb::ActorId _peer;

public:
    SelfAsker(form f, answer_by how, qb::ActorId peer)
        : _form(f)
        , _how(how)
        , _peer(peer) {}

    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Echo>(*this);
        registerEvent<EchoStream>(*this);
        registerEvent<Finish>(*this);
        spawn([f = _form, self = id(), peer = _peer](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            try {
                co_await ask_once(ctx, f, self, peer);
            } catch (qb::io::async::timeout_error const &) {
                g.error = "timeout_error";
            } catch (qb::io::async::cancelled_error const &) {
                g.error = "cancelled_error";
            } catch (std::logic_error const &e) {
                g.error = std::string("logic_error: ") + e.what();
            } catch (std::exception const &e) {
                g.error = e.what();
            }
            g.finished = true;
            ctx.push<Finish>();
        });
        co_return true;
    }
    void
    on(Echo &e) {
        switch (_how) {
            case answer_by::answer:
                qb::answer(*this, e, [](Echo const &r) {
                    ++g.handled;
                    return r.key * 10 + 1;
                });
                break;
            case answer_by::resolve_then_answer: // qb::answer resolves the same event again
                if (resolve_ask(e))
                    return;
                qb::answer(*this, e, [](Echo const &r) {
                    ++g.handled;
                    return r.key * 10 + 1;
                });
                break;
            case answer_by::copy: { // a copy of the request carries its correlation id back
                if (resolve_ask(e))
                    return;
                ++g.handled;
                Echo answer     = e;
                answer.response = e.key * 10 + 1;
                push<Echo>(e.getSource(), answer);
                break;
            }
            case answer_by::forward_to_peer: // the peer answers the asker's request
                if (resolve_ask(e))
                    return;
                ++g.handled;
                forward(_peer, e);
                break;
        }
    }
    void
    on(EchoStream &e) {
        if (resolve_ask(e))
            return;
        ++g.handled;
        qb::yield_answer(*this, e, e.key * 10 + 1);
        qb::end_stream(*this, e);
    }
    void
    on(Finish const &) {
        push<qb::KillEvent>(_peer);
        kill();
    }
};

std::string
describe() {
    std::string s = "handled=" + std::to_string(g.handled.load()) + " peer_handled=" + std::to_string(g.peer_handled.load()) + " responses={";
    for (std::size_t i = 0; i < g.responses.size(); ++i)
        s += (i ? "," : "") + std::to_string(g.responses[i]);
    return s + "} error=" + (g.error.empty() ? "none" : g.error);
}

void
run(form f, answer_by how = answer_by::answer) {
    reset();
    qb::Main   main;
    auto const peer = main.addActor<Peer>(0);
    main.addActor<SelfAsker>(0, f, how, peer);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g.finished.load()) << describe();
    EXPECT_TRUE(g.error.empty()) << describe();
    for (int const r : g.responses)
        EXPECT_NE(r, 0) << "the ask resolved with its own unanswered request: " << describe();
}

/// Self-asks from `onInit()`: before its first suspension (`suspend_first` false) and after it.
class InitSelfAsker : public qb::Actor {
    bool _suspend_first;
    bool _stream;

public:
    InitSelfAsker(bool suspend_first, bool stream)
        : _suspend_first(suspend_first)
        , _stream(stream) {}

    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Echo>(*this);
        registerEvent<EchoStream>(*this);
        if (_suspend_first)
            co_await context().sleep(1ms); // the actor is Activating from here
        const auto t0 = std::chrono::steady_clock::now();
        try {
            if (_stream) {
                auto s = qb::ask_stream(context(), id(), EchoStream{7}, 500ms);
                while (auto c = co_await s.next())
                    g.responses.push_back(c->chunk);
            } else {
                g.responses.push_back((co_await qb::ask(context(), id(), Echo{7}, 500ms)).response);
            }
        } catch (std::logic_error const &e) {
            g.error = std::string("logic_error: ") + e.what();
        } catch (std::exception const &e) {
            g.error = e.what();
        }
        g.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        g.finished   = true;
        kill();
        co_return true;
    }
    void
    on(Echo &e) {
        qb::answer(*this, e, [](Echo const &r) {
            ++g.handled;
            return r.key * 10 + 1;
        });
    }
    void
    on(EchoStream &e) {
        if (resolve_ask(e))
            return;
        ++g.handled;
        qb::yield_answer(*this, e, e.key * 10 + 1);
        qb::end_stream(*this, e);
    }
};

void
run_in_init(bool suspend_first, bool stream) {
    reset();
    qb::Main main;
    main.addActor<InitSelfAsker>(0, suspend_first, stream);
    main.start(false);
    main.join();
    EXPECT_TRUE(g.finished.load()) << describe();
    EXPECT_EQ(g.error.rfind("logic_error: ", 0), 0u) << "a self-ask from onInit() must fail at once: " << describe();
    EXPECT_NE(g.error.find("activating"), std::string::npos) << describe();
    EXPECT_TRUE(g.responses.empty()) << describe();
    EXPECT_EQ(g.handled.load(), 0) << describe();
    EXPECT_LT(g.elapsed_ms, 250) << "refused at once, not at the 500 ms timeout: " << describe();
}

} // namespace ask_self_test

using ask_self_test::answer_by;
using ask_self_test::form;
using ask_self_test::g;

// --- every form, answered by qb::answer -------------------------------------------------------------------

TEST(AskSelf, AskByValueIsAnsweredByTheHandler) {
    ask_self_test::run(form::ask_value);
    EXPECT_EQ(g.responses, (std::vector<int>{71})) << ask_self_test::describe();
    EXPECT_EQ(g.handled.load(), 1);
}
TEST(AskSelf, AskEmplaceIsAnsweredByTheHandler) {
    ask_self_test::run(form::ask_emplace);
    EXPECT_EQ(g.responses, (std::vector<int>{71})) << ask_self_test::describe();
    EXPECT_EQ(g.handled.load(), 1);
}
TEST(AskSelf, AskByByValueIsAnsweredByTheHandler) {
    ask_self_test::run(form::ask_by_value);
    EXPECT_EQ(g.responses, (std::vector<int>{71})) << ask_self_test::describe();
    EXPECT_EQ(g.handled.load(), 1);
}
TEST(AskSelf, AskByEmplaceIsAnsweredByTheHandler) {
    ask_self_test::run(form::ask_by_emplace);
    EXPECT_EQ(g.responses, (std::vector<int>{71})) << ask_self_test::describe();
    EXPECT_EQ(g.handled.load(), 1);
}
TEST(AskSelf, AskAllIncludingTheCallerGathersBothAnswers) {
    ask_self_test::run(form::ask_all);
    EXPECT_EQ(g.responses, (std::vector<int>{72, 71})) << ask_self_test::describe();
    EXPECT_EQ(g.handled.load(), 1);
    EXPECT_EQ(g.peer_handled.load(), 1);
}
TEST(AskSelf, BoundedAskAllIncludingTheCallerGathersBothAnswers) {
    ask_self_test::run(form::ask_all_bounded);
    EXPECT_EQ(g.responses, (std::vector<int>{71, 72})) << ask_self_test::describe();
    EXPECT_EQ(g.handled.load(), 1);
    EXPECT_EQ(g.peer_handled.load(), 1);
}
TEST(AskSelf, AskAnyIncludingTheCallerResolvesWithAnAnswer) {
    // The loser's late answer then bounces between the answering actors until they die: a pre-existing
    // trait of qb::answer, which cannot tell a late answer from a request (tracked separately).
    ask_self_test::run(form::ask_any);
    ASSERT_EQ(g.responses.size(), 1u) << ask_self_test::describe();
    EXPECT_TRUE(g.responses[0] == 71 || g.responses[0] == 72) << ask_self_test::describe();
}
TEST(AskSelf, AskStreamToItselfStreamsTheHandlersChunks) {
    ask_self_test::run(form::ask_stream);
    EXPECT_EQ(g.responses, (std::vector<int>{71})) << ask_self_test::describe();
    EXPECT_EQ(g.handled.load(), 1);
}

// --- every answer form ------------------------------------------------------------------------------------

TEST(AskSelf, AHandlerThatResolvesBeforeQbAnswerStillAnswersOnce) {
    ask_self_test::run(form::ask_value, answer_by::resolve_then_answer);
    EXPECT_EQ(g.responses, (std::vector<int>{71})) << ask_self_test::describe();
    EXPECT_EQ(g.handled.load(), 1);
}
TEST(AskSelf, ACopyOfTheRequestAnswersItOnce) {
    ask_self_test::run(form::ask_value, answer_by::copy);
    EXPECT_EQ(g.responses, (std::vector<int>{71})) << ask_self_test::describe();
    EXPECT_EQ(g.handled.load(), 1) << "the copy is the answer, not a new request";
}
TEST(AskSelf, ARequestForwardedToThePeerIsAnsweredThere) {
    ask_self_test::run(form::ask_emplace, answer_by::forward_to_peer);
    EXPECT_EQ(g.responses, (std::vector<int>{72})) << ask_self_test::describe();
    EXPECT_EQ(g.handled.load(), 1);
    EXPECT_EQ(g.peer_handled.load(), 1);
}

// --- the activation gate ----------------------------------------------------------------------------------

TEST(AskSelf, ASelfAskFromOnInitIsRefusedAtOnce) {
    ask_self_test::run_in_init(/*suspend_first=*/false, /*stream=*/false);
}
TEST(AskSelf, ASelfAskFromASuspendedOnInitIsRefusedAtOnce) {
    ask_self_test::run_in_init(/*suspend_first=*/true, /*stream=*/false);
}
TEST(AskSelf, ASelfStreamFromOnInitIsRefusedAtOnce) {
    ask_self_test::run_in_init(/*suspend_first=*/false, /*stream=*/true);
}
