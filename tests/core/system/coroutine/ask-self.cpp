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

namespace ask_self_test {

enum class child_finish { success, returns_false, throws };

struct NestedInitCase {
    bool         service;
    child_finish finish;
};

struct NestedInitOutcome {
    bool        adding_child                  = false;
    bool        child_started_inside_add      = false;
    bool        child_active_before_admission = false;
    bool        child_absent_before_admission = false;
    bool        child_refused                 = false;
    bool        child_reached_outcome         = false;
    bool        child_valid                   = false;
    bool        child_ready                   = false;
    bool        parent_refused_before         = false;
    bool        parent_refused_after          = false;
    bool        active_finished               = false;
    bool        finish_handled                = false;
    int         child_init_calls              = 0;
    int         child_destroyed               = 0;
    int         destroyed_when_add_returned   = 0;
    int         unexpected_stream_requests    = 0;
    int         parent_answers                = 0;
    int         active_response               = 0;
    std::string child_error;
    std::string parent_before_error;
    std::string parent_after_error;
    std::string active_error;
};

// Unlike co_await ask(), ask_stream refuses synchronously. Neither this check nor
// the child's outcome may suspend: the parent must still be in its FIRST resume,
// where the activation table cannot hide a lost outer-init marker.
bool
refuses_initial_self_stream(qb::Actor &actor, std::string &error) {
    try {
        auto stream = qb::ask_stream(actor.context(), actor.id(), EchoStream{7}, 500ms);
        (void) stream;
    } catch (std::logic_error const &e) {
        error = e.what();
        return error.find("activating") != std::string::npos;
    } catch (std::exception const &e) {
        error = e.what();
    }
    return false;
}

struct NestedInitServiceTag {};
struct CheckActiveSelfAsk : qb::Event {};

template <typename Base>
class NestedInitChild final : public Base {
    NestedInitOutcome &_out;
    child_finish       _finish;

public:
    NestedInitChild(NestedInitOutcome *out, child_finish finish)
        : _out(*out)
        , _finish(finish) {}

    ~NestedInitChild() override {
        ++_out.child_destroyed;
    }

    qb::io::async::task<bool>
    onInit() override {
        this->template registerEvent<EchoStream>(*this);
        ++_out.child_init_calls;
        _out.child_started_inside_add      = _out.adding_child;
        _out.child_active_before_admission = this->is_active();
        _out.child_absent_before_admission = !this->is_actor_alive(this->id());
        _out.child_refused                 = refuses_initial_self_stream(*this, _out.child_error);
        _out.child_reached_outcome         = true;
        if (_finish == child_finish::throws)
            throw std::runtime_error("nested init preservation witness");
        co_return _finish == child_finish::success;
    }

    void
    on(EchoStream &) {
        // A mutant may wrongly send the initial request. Count/drop it so the
        // negative proof cannot become a request/reply storm.
        ++_out.unexpected_stream_requests;
    }
};

using NestedOrdinaryChild = NestedInitChild<qb::Actor>;
using NestedServiceChild  = NestedInitChild<qb::ServiceActor<NestedInitServiceTag>>;

class NestedInitParent final : public qb::Actor {
    NestedInitOutcome &_out;
    NestedInitCase     _case;
    qb::ActorId        _child;

    template <typename Child>
    void
    add_child() {
        const auto child = addRefActor<Child>(&_out, _case.finish);
        _child           = child.id();
        _out.child_valid = child.valid();
        _out.child_ready = child.ready();
    }

public:
    NestedInitParent(NestedInitOutcome *out, NestedInitCase test_case)
        : _out(*out)
        , _case(test_case) {}

    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Echo>(*this);
        registerEvent<EchoStream>(*this);
        registerEvent<CheckActiveSelfAsk>(*this);
        registerEvent<Finish>(*this);
        _out.parent_refused_before = refuses_initial_self_stream(*this, _out.parent_before_error);
        _out.adding_child          = true;
        if (_case.service)
            add_child<NestedServiceChild>();
        else
            add_child<NestedOrdinaryChild>();
        _out.adding_child                = false;
        _out.destroyed_when_add_returned = _out.child_destroyed;
        _out.parent_refused_after        = refuses_initial_self_stream(*this, _out.parent_after_error);
        push<CheckActiveSelfAsk>(id());
        co_return true;
    }

    void
    on(CheckActiveSelfAsk const &) {
        spawn([out = &_out](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            try {
                out->active_response = (co_await qb::ask(ctx, ctx.id(), Echo{7}, 500ms)).response;
            } catch (std::exception const &e) {
                out->active_error = e.what();
            }
            out->active_finished = true;
            ctx.push<Finish>();
        });
    }

    void
    on(Echo &e) {
        qb::answer(*this, e, [this](Echo const &request) {
            ++_out.parent_answers;
            return request.key * 10 + 1;
        });
    }

    void
    on(EchoStream &) {
        ++_out.unexpected_stream_requests;
    }

    void
    on(Finish const &) {
        _out.finish_handled = true;
        if (_child.is_valid())
            push<qb::KillEvent>(_child);
        kill();
    }
};

class AskSelfNestedInit : public ::testing::TestWithParam<NestedInitCase> {};

TEST_P(AskSelfNestedInit, ChildRefusalRestoresParentAndLeavesActiveSelfAskUsable) {
    // Register the service tag before any core exists, including in shuffled runs.
    ASSERT_TRUE(qb::Actor::getServiceId<NestedInitServiceTag>(0).is_valid());
    const auto        test_case = GetParam();
    NestedInitOutcome out;
    qb::Main          main;
    main.addActor<NestedInitParent>(0, &out, test_case);
    main.start(false); // all observations are written/read on this calling thread
    main.join();

    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(out.child_init_calls, 1);
    EXPECT_TRUE(out.child_started_inside_add);
    EXPECT_TRUE(out.child_active_before_admission);
    EXPECT_TRUE(out.child_absent_before_admission) << "the synchronous child is active by phase, but has not entered the actor map";
    EXPECT_TRUE(out.child_refused) << out.child_error;
    EXPECT_TRUE(out.child_reached_outcome);
    EXPECT_EQ(out.child_valid, test_case.finish == child_finish::success);
    EXPECT_EQ(out.child_ready, test_case.finish == child_finish::success);
    EXPECT_EQ(out.destroyed_when_add_returned, test_case.finish == child_finish::success ? 0 : 1);
    EXPECT_TRUE(out.parent_refused_before) << out.parent_before_error;
    EXPECT_TRUE(out.parent_refused_after) << out.parent_after_error;
    EXPECT_TRUE(out.active_finished);
    EXPECT_TRUE(out.finish_handled);
    EXPECT_TRUE(out.active_error.empty()) << out.active_error;
    EXPECT_EQ(out.active_response, 71);
    EXPECT_EQ(out.parent_answers, 1);
    EXPECT_EQ(out.unexpected_stream_requests, 0);
    EXPECT_EQ(out.child_destroyed, 1);
}

INSTANTIATE_TEST_SUITE_P(DynamicChildren, AskSelfNestedInit,
                         ::testing::Values(NestedInitCase{false, child_finish::success}, NestedInitCase{false, child_finish::returns_false},
                                           NestedInitCase{false, child_finish::throws}, NestedInitCase{true, child_finish::success},
                                           NestedInitCase{true, child_finish::returns_false}, NestedInitCase{true, child_finish::throws}),
                         [](const ::testing::TestParamInfo<NestedInitCase> &info) {
                             std::string name = info.param.service ? "Service" : "Ordinary";
                             switch (info.param.finish) {
                                 case child_finish::success:
                                     return name + "Success";
                                 case child_finish::returns_false:
                                     return name + "False";
                                 case child_finish::throws:
                                     return name + "Throw";
                             }
                             return name;
                         });

} // namespace ask_self_test

namespace ask_self_test {

struct StartParentOwnedStream : qb::Event {};
struct ParentOwnedStreamComplete : qb::Event {};

struct ParentOwnedStreamOutcome {
    bool             parent_active    = false;
    bool             distinct_owner   = false;
    bool             owner_alive      = false;
    bool             child_valid      = false;
    bool             child_pending    = false;
    bool             stream_started   = false;
    bool             stream_ended     = false;
    bool             child_finished   = false;
    bool             completed        = false;
    int              child_init_calls = 0;
    int              child_destroyed  = 0;
    int              parent_requests  = 0;
    std::vector<int> chunks;
    std::string      error;
};

// The child consumes a stream owned by the still-active parent. Its first resume
// must not prevent that parent's handlers from serving the parent's self-request.
class ParentOwnedStreamChild final : public qb::Actor {
    qb::ScopedCoroContext     _parent;
    ParentOwnedStreamOutcome *_out;

public:
    ParentOwnedStreamChild(qb::ScopedCoroContext parent, ParentOwnedStreamOutcome *out)
        : _parent(parent)
        , _out(out) {}

    ~ParentOwnedStreamChild() override {
        ++_out->child_destroyed;
    }

    qb::io::async::task<bool>
    onInit() override {
        // Snapshot the context and observations before suspending; no child member
        // or parent actor pointer is accessed after the first await.
        auto        parent = _parent;
        auto *const out    = _out;
        ++out->child_init_calls;
        out->distinct_owner = parent.id() != id();
        out->owner_alive    = is_actor_alive(parent.id());
        try {
            auto stream         = qb::ask_stream(parent, parent.id(), EchoStream{7}, 500ms);
            out->stream_started = true;
            while (auto chunk = co_await stream.next())
                out->chunks.push_back(chunk->chunk);
            out->stream_ended = true;
        } catch (std::exception const &e) {
            out->error = e.what();
        } catch (...) {
            out->error = "non-standard exception";
        }
        out->child_finished = true;
        parent.push<ParentOwnedStreamComplete>();
        co_return true;
    }
};

class ParentOwnedStreamParent final : public qb::Actor {
    ParentOwnedStreamOutcome &_out;
    qb::ActorId               _child;

public:
    explicit ParentOwnedStreamParent(ParentOwnedStreamOutcome *out)
        : _out(*out) {}

    qb::io::async::task<bool>
    onInit() override {
        registerEvent<StartParentOwnedStream>(*this);
        registerEvent<ParentOwnedStreamComplete>(*this);
        registerEvent<EchoStream>(*this);
        push<StartParentOwnedStream>(id());
        co_return true;
    }

    void
    on(StartParentOwnedStream const &) {
        _out.parent_active = is_active();
        const auto child   = addRefActor<ParentOwnedStreamChild>(context(), &_out);
        _child             = child.id();
        _out.child_valid   = child.valid();
        _out.child_pending = child.valid() && !child.ready();
        if (!child.valid())
            push<ParentOwnedStreamComplete>(id());
    }

    void
    on(EchoStream &e) {
        if (resolve_ask(e))
            return;
        ++_out.parent_requests;
        qb::yield_answer(*this, e, e.key * 10 + 1);
        qb::end_stream(*this, e);
    }

    void
    on(ParentOwnedStreamComplete const &) {
        _out.completed = true;
        if (_child.is_valid())
            push<qb::KillEvent>(_child);
        kill();
    }
};

TEST(AskSelf, ActiveParentOwnsStreamWhileChildInitializes) {
    ParentOwnedStreamOutcome out;
    qb::Main                 main;
    main.addActor<ParentOwnedStreamParent>(0, &out);
    main.start(false);
    main.join();

    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(out.parent_active);
    EXPECT_TRUE(out.distinct_owner);
    EXPECT_TRUE(out.owner_alive);
    EXPECT_TRUE(out.child_valid);
    EXPECT_TRUE(out.child_pending);
    EXPECT_EQ(out.child_init_calls, 1);
    EXPECT_TRUE(out.stream_started) << out.error;
    EXPECT_TRUE(out.stream_ended) << out.error;
    EXPECT_TRUE(out.error.empty()) << out.error;
    EXPECT_EQ(out.chunks, (std::vector<int>{71}));
    EXPECT_EQ(out.parent_requests, 1);
    EXPECT_TRUE(out.child_finished);
    EXPECT_TRUE(out.completed);
    EXPECT_EQ(out.child_destroyed, 1);
}

} // namespace ask_self_test
