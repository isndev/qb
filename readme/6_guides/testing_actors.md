# Testing your actors

> **Audience:** Adopter · **Status:** stable · **Verified-against:** qb 3.3.0 (C++20 default, C++23 supported)

How to test your own actor code with the kit qb ships in `<qb/testkit.h>` (since 3.4): run a scenario inside a real engine, record what your actors send, and wait for it with a deadline.

**Prerequisites:** [Writing actors with `qb::Actor`](../4_qb_core/actor.md), [Event messaging](../4_qb_core/messaging.md), [Asynchronous operations inside actors](../5_core_io_integration/async_in_actors.md) — **See also:** [Testing the framework](../7_reference/testing.md), [Patterns cookbook](./patterns_cookbook.md)

## Summary

A test of actor code has the same shape every time: an engine, the actors under test, something that drives the exchange, a way to see what the actors sent, a stop, a join, and a check of `qb::Main::hasError()`. Written by hand, that is a driver actor, a few `std::atomic` globals and a guard proving the driver ran. The kit ships the shape:

- `qb::test::TestKit` owns a `qb::Main` and runs **one scenario** — a coroutine taking a `qb::ScopedCoroContext` — on a driver actor it adds itself. The engine stops the moment the scenario returns or throws; a watchdog stops it if it never ends. `run()` returns a `qb::test::RunResult`.
- `qb::test::TestProbe<Events...>` is an actor that records a copy of every event of the announced types it receives, in arrival order, one queue per type. Its id is the destination you give the actors under test.
- `co_await qb::test::expect<E>(ctx, probe, timeout)` yields the next recorded `E` (a `std::optional<E>`, empty at the deadline); `co_await qb::test::expect_none<E>(ctx, probe, window)` is `true` when no `E` arrived during the window. The probe's clock is the only one: the asker sets no deadline of its own, so a machine that stalls delays the probe's answer and never stands in for it.
- `qb::test::Asker` is the actor that expects. The kit's driver is one, so a scenario needs nothing; an actor of your test that calls `expect` itself derives from it.

The engine runs for real, on its own threads: a test asserts on what arrives within a deadline, never on an interleaving. The kit depends on no test framework.

<!-- src: qb/src/qb/core/TestKit.h:1249-1360 (TestKit); qb/src/qb/core/TestKit.h:796-1091 (TestProbe); qb/src/qb/core/TestKit.h:1129-1171 (expect); qb/src/qb/core/TestKit.h:1189-1224 (expect_none); qb/src/qb/core/TestKit.h:626-660 (RunResult) -->

## A first test

The actor under test reports to the destination it is given at construction. In the test that destination is a probe; the scenario sends the order, then waits for the fill.

```cpp
#include <gtest/gtest.h>
#include <qb/testkit.h>
#include <chrono>

using namespace std::chrono_literals;

namespace desk_test {   // a named namespace: GCC refuses coroutine-spawned fixtures in an anonymous one

struct Order : qb::Event {
    int qty = 0;
    explicit Order(int q) : qty(q) {}
};
struct Filled : qb::Event {
    int qty = 0;
    explicit Filled(int q) : qty(q) {}
};

class Desk : public qb::Actor {
    qb::ActorId _report;
public:
    explicit Desk(qb::ActorId report) : _report(report) {}
    qb::io::async::task<bool> onInit() override {
        registerEvent<Order>(*this);
        co_return true;
    }
    void on(Order const &o) { push<Filled>(_report, o.qty); }
};

} // namespace desk_test

TEST(Desk, FillsAnOrder) {
    using namespace desk_test;
    qb::test::TestKit kit;
    const auto probe = kit.probe<Filled>(0);     // a TestProbe<Filled> on core 0
    const auto desk  = kit.add<Desk>(0, probe);  // the actor under test reports to it

    const auto run = kit.run([desk, probe](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
        ctx.push_to<Order>(desk, 7);
        const auto filled = co_await qb::test::expect<Filled>(ctx, probe, 1s);
        EXPECT_TRUE(filled.has_value());
        if (filled) {
            EXPECT_EQ(filled->qty, 7);
            EXPECT_EQ(filled->getSource(), desk);      // the recorded copy keeps its sender
        }
        EXPECT_TRUE(co_await qb::test::expect_none<Filled>(ctx, probe, 50ms)); // and only one fill
    });
    EXPECT_TRUE(run) << run.describe();
}
```

<!-- src: qb/tests/core/system/patterns/testkit.cpp:1097-1112 -->

`run` is `true` when the scenario ran to its end, the watchdog did not fire and the engine reported no error; `run.describe()` names what went wrong (`"the scenario threw: ..."`, `"the watchdog stopped the engine"`, `"the engine reported an error (qb::Main::hasError())"`), or says `"ok"`.

Check with `EXPECT_*` inside the scenario. `ASSERT_*` does not compile there: it `return`s, and a coroutine cannot. To stop a scenario on a failed check, `co_return`, or throw — a scenario that throws ends the run, and `RunResult::error` holds the exception.

## What a probe answers

- **One queue per type, in arrival order.** `expect<E>` consumes the oldest recorded `E`. When none is waiting it waits on the probe until one arrives or `timeout` passes; two waiters on one type — two `expect`s under a `when_all` — are served oldest first.
- **Nothing is dropped.** An event that arrives after an `expect` timed out stays recorded for the next one; so does every event no expectation asked for. An event the probe hands to an expectation is only lent until the scenario acknowledges it — `expect` does at once. An expectation the scenario abandons (the losing branch of a `when_any`) withdraws itself as its frame ends: the probe forgets its waiter, and an event lent to it goes back at once to its place in the queue, arrival order kept. The probe also watches every actor that asks it (`watch()`): when one is gone, its waiters are dropped and what it was lent goes back the same way, oldest event first. A probe can record `qb::DownEvent` like any type (the destination of a supervisor's reports): it records the ones it is sent, and keeps the answers to its own watches — a DownEvent about an asker it watches, whose source is that asker — to itself. That shape is all it can read: an asker that sends a DownEvent about itself is taken as gone, and its expectations in flight are never answered. A loan ends by one of those three — the acknowledgement, the withdrawal, the asker's death — and by no clock: an asker that reads its answer late still has it, and one that takes an event and never acknowledges it keeps it for as long as it lives. The queues are unbounded: a probe is for tests, not for traffic.
- **`expect_none` consumes nothing.** It answers `false` at once when an `E` is already recorded, `false` the moment one arrives, `true` at the end of the window. A following `expect<E>` still gets the event.
- **A timeout is a lower bound, on the probe's clock.** The probe answers at its deadline, never earlier — and only once it has read the events addressed to it that had reached its core by then: an event that waited in its mailbox while that core was stalled is an arrival, not a "none". That is as far as it goes. The window is wall time on the probe's core: an event that did not exist yet when it ended — the desk beside the probe was held by the same stall, and fills only once the core runs again — may be read as late, as with any timeout. The reply then crosses back to the scenario. A zero (or negative) timeout takes only an event already recorded.
- **The probe's clock is the only one.** `expect` and `expect_none` set no deadline of their own: they wait for the probe's answer, however late a stalled machine delivers it. `expect_none` is `true` or `false` because the probe said so, and an event the probe holds is never reported as none. A loaded CI runner delays an answer; it does not make one up.
- **The wrong probe fails loudly.** `expect<E>` on a probe that does not record `E` throws `std::logic_error`.
- **Any core.** `expect` is a `qb::ask` of the probe, so the probe may live on another core than the scenario (`kit.probe<Filled>(1)`).
- **A probe that is gone is never an answer.** `expect` and `expect_none` then throw `qb::test::probe_gone_error` — neither "an event arrived" nor "none did": a test asserts one or the other, and a probe that is gone proves neither. They throw with nothing sent, and no window waited, when no live probe holds the id: the probe ended, or the id never was a probe's (another actor's, the null id) — the kit cannot tell the two apart, and `what()` says so. And they throw when a `qb::KillEvent` ends the probe under the expectation: a killed probe tells the expectations waiting on it, and stays up until the ones already on their way have arrived and been told the same — or their asker is gone — then dies. Left uncaught, the error ends the run: `RunResult::error` holds it and `describe()` names it. Under a `when_any`, an expectation that throws wins the race and its error stays in the result: read the winner with `get<T>()`, which rethrows, or test `has_exception()` — `index` alone does not tell an answer from an error.
- **No expectation outlives its probe.** A probe can end without a word. On a core that stops on an error — an exception out of a handler there, an actor under test that throws beside the probe — it cannot answer: that core sends nothing more. `kill()` called on it through a handle runs none of its handlers. An asker — the kit's driver, or an `Asker` of your own, see below — watches every probe it expects on, and a watch is answered whatever ended the probe, behind everything its core had sent: the expectations of that asker on that probe throw `probe_gone_error` then (`what()` says whether a core stopped), and after a core's error `engine_error` reports it.
- **A probe that stops with its engine is silent.** Its askers stop with it, and are told nothing. An asker that outlives the stop — an actor that ignores the signal — is told the probe is gone, as after any other end. Once a probe is gone, however it went, the next `expect` on it throws — two passes of the asker's core later, and only if the asker is still there. An engine stops one core at a time: a probe's core may have reaped it while an asker on another core has not read the stop yet, and an expectation asked in between finds no probe. Told at once, that asker got an error for a probe that had only stopped a pass ahead of it — out of a `spawn` body, where nothing catches it, a `CRITICAL` line on stderr in a run that went well. Two passes later the stop has reached it: it is cancelled with its actor, and nothing is printed. A probe that records `qb::KillEvent` — the destination an actor under test kills — records the ones it is sent, and ends with the engine.
- **Expect from a spawned coroutine.** A `TestKit` announces a probe as it adds it, so an `expect` that runs before the probe is built — from an `onInit()`, which runs while the other cores still build their actors — is taken, and answered once the engine has started. A probe added through `engine().addActor` is known from its constructor only: expect on it from a spawned coroutine, which runs once every core has built its actors. And an expectation asked in an `onInit()` is bounded by the asker's core, not by the kit: a core fails an actor whose `onInit()` has not returned when its activation deadline passes — `qb::VirtualCore::activation_deadline_ns`, five seconds unless you set it before `run()`, counted from the moment the actor is built. The expectation throws `cancelled_error` then and the actor is removed: a window longer than what is left of those seconds never gets its answer, and a machine that stalls through them ends one that would have got it. Raise the deadline for such a test, or expect from a spawned coroutine.

<!-- src: qb/src/qb/core/TestKit.h:874-1061 (record, on(Expectation), on(ExpectationDeadline), the loan's acknowledgement and withdrawal, on(DownEvent)); qb/tests/core/system/patterns/testkit.cpp:1114-1271; qb/tests/core/system/patterns/testkit.cpp:1364-1618; qb/tests/core/system/patterns/testkit.cpp:1633-1652; qb/src/qb/core/TestKit.h:205-373 (the census of the probes); qb/src/qb/core/TestKit.h:1070-1090 (on(KillEvent)); qb/src/qb/core/TestKit.h:930-932 (the destructor, which says nothing); qb/src/qb/core/TestKit.h:458-466 (an asker's watch of a probe); qb/src/qb/core/TestKit.h:496-505 (its answer); qb/src/qb/core/TestKit.h:743-750 (a probe found gone, told to an asker that is still there); qb/tests/core/system/patterns/testkit.cpp:1654-2400; qb/src/qb/core/TestKit.h:854-859 (lend); qb/tests/core/system/patterns/testkit.cpp:1591-1618 -->

## When the scenario asks other actors

A reply to `qb::ask` resolves only in an actor that registers the exchange type (`registerEvent<E>` plus `resolve_ask(e)` in its handler). The driver registers the kit's own exchange; name every other exchange your scenario asks with as a template argument of `run`:

```cpp
const auto run = kit.run<Quote>([quoter](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
    const auto q = co_await qb::ask(ctx, quoter, Quote{21}, 2s);
    EXPECT_EQ(q.response, 42);
});
```

Without `<Quote>` the reply reaches a driver that does not handle it — a dead letter — and the `ask` times out.

<!-- src: qb/tests/core/system/patterns/testkit.cpp:1295-1320 -->

## An actor that expects

A scenario is all most tests need. When an actor of the test has to expect itself — from its `onInit()`, from a coroutine it spawns — it derives from `qb::test::Asker` instead of `qb::Actor`:

```cpp
class Auditor : public qb::test::Asker {   // an Asker hears the probes' answers
    qb::ActorId _probe;
    qb::ActorId _report;
public:
    Auditor(qb::ActorId probe, qb::ActorId report) : _probe(probe), _report(report) {}
    qb::io::async::task<bool> onInit() override {
        spawn([probe = _probe, report = _report](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            const auto filled = co_await qb::test::expect<Filled>(ctx, probe, 1s);
            ctx.push_to<Filled>(report, filled ? filled->qty : -1);   // to a probe the scenario reads
        });
        co_return true;
    }
};
```

- **Only an `Asker` expects.** An expectation sets no deadline: its answer is all that ends it, and an actor that does not route the kit's exchange never hears one — the answer is a dead letter on its core. `expect` and `expect_none` refuse such an actor where they are called, with `std::logic_error`. The kit's driver is an `Asker`: a scenario needs nothing.
- **It watches the probes it expects on**, which is why no expectation outlives its probe. It registers `qb::DownEvent` for that: an asker that registers the type for itself takes the place of that handler, and hands every `qb::DownEvent` it does not expect to `Asker::on(e)`.
- **An error out of a `spawn` body is reported and dropped.** A scenario that lets `probe_gone_error` escape ends its run on it; in a coroutine of another actor nothing receives it, and the core prints it. Catch it there, and tell a probe.
- **From an `onInit()`** the probe is watched too: the watch waits with the expectation in the pipe to the probe's core, which builds every actor it was given before it turns its first pass. The activation deadline (above) still ends what an `onInit()` asks.

<!-- src: qb/src/qb/core/TestKit.h:441-506 (Asker); qb/tests/core/system/patterns/testkit.cpp:2186-2266 -->

## The engine and the run

`run(scenario, watchdog = 10 s, core = 0)` adds the driver on `core`, starts the engine on its own threads, waits for the scenario or the watchdog, stops the engine, joins it, and reads `hasError()`. Before `run()`, configure the engine through `kit.engine()` (`core(id).setLatency(...)`, `setIdleSpin(...)`) and add actors through `kit.add<A>(core, args...)` or `kit.engine().addActor<A>(...)`. A core runs at latency 0 — spinning — unless configured otherwise.

- **One run per kit.** An engine does not restart; a second `run()` throws `std::logic_error`. Use one kit per test.
- **One kit at a time.** The driver ends the run with `qb::Main::stop()`, which stops every engine of the process. And the kit knows one probe per id: a probe's id is its engine's own, and nothing an expectation can read names its engine. Build a kit and run it, then build the next: a kit holds the ids of its probes from `add` to the end of its `run()`, whether its engine started or refused to (and to its destruction when it never runs). A probe added while another kit's probe — or a probe of another engine alive in the process — holds the same id throws `std::logic_error`; one added to a second engine by hand stops that engine's start.
- **The watchdog.** A scenario that never ends is stopped when `watchdog` passes: `RunResult::timed_out` is set, `completed` is not, and `error` stays empty — the scenario's coroutine is torn down with its core. That verdict is final: what a scenario still does before its core reads the stop — an answer was ahead of it in that core's mailbox, it resumes and returns — changes nothing in the result. A run whose engine stops before its scenario ends — the scenario stopping it, another actor's exception on the driver's core, an engine that refuses to start (a registered core with no actor, an actor whose constructor throws: `engine_error`) — returns then, `completed` unset, whatever the watchdog: the driver's destructor, or the refused start, ends the wait -- and a driver destroyed before its scenario ends reports it not run to its end even if the scenario's frame later finishes (since 3.4, Huly QB-1227; a watchdog of `qb::duration::max()` waits without end). The stop is cooperative: `run()` then joins the engine, so a core held in a handler that never returns keeps it waiting — CTest's own timeout is the bound there.
- **An error on another core does not end the run.** A core other than the driver's that stops on an error — an exception out of a handler there — sets `engine_error` and ends nothing: the scenario runs on, to its end or to the watchdog.
- **Capture by value.** The scenario is stored in the driver for the run, as any `spawn` body: capture ids and values, never references to the test's locals.

<!-- src: qb/src/qb/core/TestKit.h:1303-1359 (run); qb/tests/core/system/patterns/testkit.cpp:1322-1362; qb/tests/core/system/patterns/testkit.cpp:2402-2425; qb/tests/core/system/patterns/testkit.cpp:2427-2437; qb/tests/core/system/patterns/testkit.cpp:2439-2465; qb/tests/core/system/patterns/testkit.cpp:2467-2489 -->

## Pitfalls

- **A recorded type must be copy-constructible** and derive from `qb::Event`: the probe keeps a copy. `static_assert`s say so at compile time.
- **A probe does not record `qb::SignalEvent`.** A probe registers a handler for each type it records, and that one would take the place of the handler that ends an actor when its engine stops: the probe would record the stop and stay, and `run()` would never come back from its join. `TestProbe<qb::SignalEvent>` is no type — nor is a probe of the kit's own exchange — and `kit.probe<qb::SignalEvent>()` does not compile.
- **The probe is the destination, not the scenario.** An event an actor pushes to `ctx`'s actor (the driver) is not recorded: hand the actors under test the probe's id.
- **Deadlines, not sleeps.** Wait for an event with `expect`, for an absence with `expect_none`; a `co_await ctx.sleep(...)` that "gives the actors time" is the flaky test this kit exists to avoid.
- **Upper bounds are fragile.** On a loaded machine an answer can take longer than you think — it is still the probe's answer, whenever it comes. To assert that an event is there NOW, expect it with a timeout of zero behind an event that proves it was sent, not with a bound on the time an expectation took.
- **An actor that expects is an `Asker`.** From any other actor `expect` throws `std::logic_error` where it is called: see "An actor that expects".
- **One image.** The kit's census of live probes is one object per program on ELF and Mach-O; on Windows each DLL has its own, so a probe and the code that expects on it belong in the same image.

<!-- src: qb/src/qb/core/TestKit.h:685-686 (the types no probe records); qb/tests/core/system/patterns/testkit.cpp:74-83 -->

## See also

- [Testing the framework](../7_reference/testing.md) — how qb's own suite is organized, built and run, and how to register a test with `qb_add_test`.
- [Patterns cookbook](./patterns_cookbook.md) — request/reply, scatter-gather and the other interactions a scenario drives.
- [Asynchronous operations inside actors](../5_core_io_integration/async_in_actors.md) — `spawn`, `ScopedCoroContext` and the cancellation a scenario runs under.
