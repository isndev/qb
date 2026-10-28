# Testing your actors

> **Audience:** Adopter · **Status:** stable · **Verified-against:** qb 3.3.0 (C++20 default, C++23 supported)

How to test your own actor code with the kit qb ships in `<qb/testkit.h>` (since 3.4): run a scenario inside a real engine, record what your actors send, and wait for it with a deadline.

**Prerequisites:** [Writing actors with `qb::Actor`](../4_qb_core/actor.md), [Event messaging](../4_qb_core/messaging.md), [Asynchronous operations inside actors](../5_core_io_integration/async_in_actors.md) — **See also:** [Testing the framework](../7_reference/testing.md), [Patterns cookbook](./patterns_cookbook.md)

## Summary

A test of actor code has the same shape every time: an engine, the actors under test, something that drives the exchange, a way to see what the actors sent, a stop, a join, and a check of `qb::Main::hasError()`. Written by hand, that is a driver actor, a few `std::atomic` globals and a guard proving the driver ran. The kit ships the shape:

- `qb::test::TestKit` owns a `qb::Main` and runs **one scenario** — a coroutine taking a `qb::ScopedCoroContext` — on a driver actor it adds itself. The engine stops the moment the scenario returns or throws; a watchdog stops it if it never ends. `run()` returns a `qb::test::RunResult`.
- `qb::test::TestProbe<Events...>` is an actor that records a copy of every event of the announced types it receives, in arrival order, one queue per type. Its id is the destination you give the actors under test.
- `co_await qb::test::expect<E>(ctx, probe, timeout)` yields the next recorded `E` (a `std::optional<E>`, empty at the deadline); `co_await qb::test::expect_none<E>(ctx, probe, window)` is `true` when no `E` arrived during the window.

The engine runs for real, on its own threads: a test asserts on what arrives within a deadline, never on an interleaving. The kit depends on no test framework.

<!-- src: qb/src/qb/core/TestKit.h:436-501 (TestKit); qb/src/qb/core/TestKit.h:251-367 (TestProbe); qb/src/qb/core/TestKit.h:382-398 (expect); qb/src/qb/core/TestKit.h:412-426 (expect_none); qb/src/qb/core/TestKit.h:203-237 (RunResult) -->

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

<!-- src: qb/tests/core/system/patterns/testkit.cpp:101-116 -->

`run` is `true` when the scenario ran to its end, the watchdog did not fire and the engine reported no error; `run.describe()` names what went wrong (`"the scenario threw: ..."`, `"the watchdog stopped the engine"`, `"the engine reported an error (qb::Main::hasError())"`), or says `"ok"`.

Check with `EXPECT_*` inside the scenario. `ASSERT_*` does not compile there: it `return`s, and a coroutine cannot. To stop a scenario on a failed check, `co_return`, or throw — a scenario that throws ends the run, and `RunResult::error` holds the exception.

## What a probe answers

- **One queue per type, in arrival order.** `expect<E>` consumes the oldest recorded `E`. When none is waiting it waits on the probe until one arrives or `timeout` passes; two waiters on one type — two `expect`s under a `when_all` — are served oldest first.
- **Nothing is dropped.** An event that arrives after an `expect` timed out stays recorded for the next one; so does every event no expectation asked for. The queues are unbounded: a probe is for tests, not for traffic.
- **`expect_none` consumes nothing.** It answers `false` at once when an `E` is already recorded, `false` the moment one arrives, `true` at the end of the window. A following `expect<E>` still gets the event.
- **A timeout is a lower bound.** The probe answers at its deadline, never earlier; the reply then crosses back to the scenario. A zero (or negative) timeout takes only an event already recorded.
- **The wrong probe fails loudly.** `expect<E>` on a probe that does not record `E` throws `std::logic_error`.
- **Any core.** `expect` is a `qb::ask` of the probe, so the probe may live on another core than the scenario (`kit.probe<Filled>(1)`). When the probe does not answer at all — its core stopped — `expect` returns empty after the timeout plus a grace (250 ms), and `expect_none` returns `false`.

<!-- src: qb/src/qb/core/TestKit.h:298-366 (record, on(Expectation), on(ExpectationDeadline)); qb/tests/core/system/patterns/testkit.cpp:118-226 -->

## When the scenario asks other actors

A reply to `qb::ask` resolves only in an actor that registers the exchange type (`registerEvent<E>` plus `resolve_ask(e)` in its handler). The driver registers the kit's own exchange; name every other exchange your scenario asks with as a template argument of `run`:

```cpp
const auto run = kit.run<Quote>([quoter](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
    const auto q = co_await qb::ask(ctx, quoter, Quote{21}, 2s);
    EXPECT_EQ(q.response, 42);
});
```

Without `<Quote>` the reply reaches a driver that does not handle it — a dead letter — and the `ask` times out.

<!-- src: qb/tests/core/system/patterns/testkit.cpp:249-274 -->

## The engine and the run

`run(scenario, watchdog = 10 s, core = 0)` adds the driver on `core`, starts the engine on its own threads, waits for the scenario or the watchdog, stops the engine, joins it, and reads `hasError()`. Before `run()`, configure the engine through `kit.engine()` (`core(id).setLatency(...)`, `setIdleSpin(...)`) and add actors through `kit.add<A>(core, args...)` or `kit.engine().addActor<A>(...)`. A core runs at latency 0 — spinning — unless configured otherwise.

- **One run per kit.** An engine does not restart; a second `run()` throws `std::logic_error`. Use one kit per test.
- **One kit at a time.** The driver ends the run with `qb::Main::stop()`, which stops every engine of the process.
- **The watchdog.** A scenario that never ends is stopped when `watchdog` passes: `RunResult::timed_out` is set, `completed` is not, and `error` stays empty — the scenario's coroutine is torn down with its core without being resumed. A run whose engine refuses to start (a registered core with no actor) waits out the watchdog too, and reports `engine_error`.
- **Capture by value.** The scenario is stored in the driver for the run, as any `spawn` body: capture ids and values, never references to the test's locals.

<!-- src: qb/src/qb/core/TestKit.h:463-500 (run); qb/tests/core/system/patterns/testkit.cpp:276-310 -->

## Pitfalls

- **A recorded type must be copy-constructible** and derive from `qb::Event`: the probe keeps a copy. `static_assert`s say so at compile time.
- **The probe is the destination, not the scenario.** An event an actor pushes to `ctx`'s actor (the driver) is not recorded: hand the actors under test the probe's id.
- **Deadlines, not sleeps.** Wait for an event with `expect`, for an absence with `expect_none`; a `co_await ctx.sleep(...)` that "gives the actors time" is the flaky test this kit exists to avoid.
- **Upper bounds are fragile.** The kit's own tests assert only that a timeout is not shorter than asked; on a loaded machine an answer can take longer than you think.

## See also

- [Testing the framework](../7_reference/testing.md) — how qb's own suite is organized, built and run, and how to register a test with `qb_add_test`.
- [Patterns cookbook](./patterns_cookbook.md) — request/reply, scatter-gather and the other interactions a scenario drives.
- [Asynchronous operations inside actors](../5_core_io_integration/async_in_actors.md) — `spawn`, `ScopedCoroContext` and the cancellation a scenario runs under.
