/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/engine/sigterm-shutdown.cpp
 * @brief A registered termination signal must actually shut the engine down.
 *
 * `Main::registerSignal`'s contract (`core/Main.h`) is explicit:
 *   "By default, `SIGINT` and `SIGTERM` (on non-Windows platforms) are registered to call
 *    `Main::stop()`. Registered signals will trigger a graceful shutdown of all actors."
 *
 * Two things have to hold for that to be true, and this file pins both — with one test each,
 * because either half alone still leaves `SIGTERM` broken:
 *   1. `Main::install_default_signals()` installs a handler for `SIGTERM`, not only `SIGINT` —
 *      otherwise the default disposition applies to any process that does not call
 *      `registerSignal` itself and it is killed outright, with no actor teardown and no final
 *      flush. Pinned by `EngineInstallsTheSigtermHandlerItself`, which registers **nothing** and
 *      clears the disposition first: every other test here calls `Main::registerSignal(signum)`
 *      (documented usage), which installs the very handler this half is about and would keep the
 *      suite green through a regression to `SIGINT`-only.
 *   2. `Actor::on(SignalEvent&)`'s default body treats `SIGTERM` as terminal. If it does not,
 *      a user who registers `SIGTERM` (exactly as the API invites) converts it into a **no-op**:
 *      the handler swallows the signal, no actor is killed, and the process becomes unkillable
 *      by `SIGTERM` — `docker stop` / `systemctl stop` then hang until their grace period
 *      expires and the supervisor escalates to `SIGKILL`. Pinned by `SigtermStopsTheEngine`.
 *
 * `SIGHUP`/`SIGUSR1` must stay NON-terminal: they are the documented "register your own signal"
 * examples (config reload), and killing every actor on a reload signal would be a far worse
 * regression than the bug being fixed.
 */

#include <atomic>
#include <chrono>
#include <csignal>
#include <future>
#include <gtest/gtest.h>
#include <memory>
#include <thread>
#include <qb/actor.h>
#include <qb/main.h>

namespace {

std::atomic<bool>                 g_running{false};
[[maybe_unused]] std::atomic<int> g_hup_seen{0}; // read only by the SIGHUP case, which Windows has no signal for

/// Stays alive forever on its own: only a signal can end this engine.
class ForeverActor final
    : public qb::Actor
    , public qb::ICallback {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerCallback(*this);
        co_return true;
    }
    void
    on(qb::LoopEvent const &) final {
        g_running.store(true, std::memory_order_relaxed);
    }
};

/// Observes a non-terminal signal without dying, to prove reload signals stay non-terminal.
class ReloadActor final
    : public qb::Actor
    , public qb::ICallback {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerCallback(*this);
        // Re-subscribe from the DERIVED type: `Actor`'s constructor already registered
        // SignalEvent, but it did so with `*this` typed as `Actor&`, so the router's
        // trampoline dispatches to `Actor::on` (the handlers are not virtual). Registering
        // again here rebinds the slot to `ReloadActor::on`.
        registerEvent<qb::SignalEvent>(*this);
        co_return true;
    }
    void
    on(qb::LoopEvent const &) final {
        g_running.store(true, std::memory_order_relaxed);
    }
    void
    on(qb::SignalEvent const &event) noexcept {
#ifndef _WIN32
        if (event.signum == SIGHUP) // no SIGHUP on MSVC — see SighupIsNotTerminal
            g_hup_seen.fetch_add(1, std::memory_order_relaxed);
#endif
        qb::Actor::on(event); // keep the framework's default disposition under test
    }
};

/// Runs an engine on a detached thread, raises `signum` once it is up, and reports whether
/// the engine terminated on its own within `budget`.
template <typename ActorT>
[[nodiscard]] bool
engine_stops_on(int signum, std::chrono::seconds budget) {
    g_running.store(false, std::memory_order_relaxed);
    auto done   = std::make_shared<std::promise<void>>();
    auto future = done->get_future();

    std::thread([done] {
        qb::Main main;
        main.addActor<ActorT>(0);
        main.start();
        main.join();
        done->set_value();
    }).detach();

    // Wait for the loop to actually be turning before signalling.
    const auto up_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!g_running.load(std::memory_order_relaxed) && std::chrono::steady_clock::now() < up_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    qb::Main::registerSignal(signum); // documented usage
    std::raise(signum);

    return future.wait_for(budget) == std::future_status::ready;
}

/// Whether @p signum currently has a handler installed (i.e. neither the default disposition
/// nor SIG_IGN). POSIX can query without touching the disposition; Windows has no query API,
/// so the previous handler is read back from a set-and-restore round-trip.
[[nodiscard]] bool
handler_installed(int signum) noexcept {
#if defined(_WIN32) || defined(_WIN64)
    const auto previous = std::signal(signum, SIG_DFL);
    if (previous != SIG_ERR)
        (void) std::signal(signum, previous);
    return previous != SIG_DFL && previous != SIG_IGN && previous != SIG_ERR;
#else
    struct sigaction current{};
    if (::sigaction(signum, nullptr, &current) != 0)
        return false;
    return current.sa_handler != SIG_DFL && current.sa_handler != SIG_IGN;
#endif
}

} // namespace

// Control: the signal the engine already installs itself must stop it.
TEST(SignalShutdown, SigintStopsTheEngine) {
    EXPECT_TRUE(engine_stops_on<ForeverActor>(SIGINT, std::chrono::seconds(15)));
}

// The production stop signal: Docker, Kubernetes and systemd all send SIGTERM first.
TEST(SignalShutdown, SigtermStopsTheEngine) {
    EXPECT_TRUE(engine_stops_on<ForeverActor>(SIGTERM, std::chrono::seconds(15)))
        << "SIGTERM did not shut the engine down: registerSignal() installed a handler that "
           "swallows it (Actor::on(SignalEvent) only treats SIGINT as terminal), so the process "
           "is now UNKILLABLE by SIGTERM and a supervisor must escalate to SIGKILL";
}

// The other half of the contract: the engine must INSTALL the SIGTERM handler on its own.
// `engine_stops_on` calls `Main::registerSignal(signum)` before raising — documented usage, but it
// installs exactly the handler `install_default_signals()` is responsible for, so the tests above
// stay green even if the engine regresses to registering SIGINT only. This test registers nothing:
// it puts SIGTERM back to its default disposition, starts an engine, and requires the engine itself
// to have installed a handler before raising the signal.
TEST(SignalShutdown, EngineInstallsTheSigtermHandlerItself) {
    // Dispositions are process-wide and the engine never restores them, so a previous test in this
    // binary would otherwise mask the regression.
    qb::Main::unregisterSignal(SIGTERM);
    ASSERT_FALSE(handler_installed(SIGTERM)) << "precondition: SIGTERM must start from its default disposition";

    g_running.store(false, std::memory_order_relaxed);
    auto done   = std::make_shared<std::promise<void>>();
    auto future = done->get_future();

    std::thread([done] {
        qb::Main main;
        main.addActor<ForeverActor>(0);
        main.start();
        main.join();
        done->set_value();
    }).detach();

    const auto up_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!g_running.load(std::memory_order_relaxed) && std::chrono::steady_clock::now() < up_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    // `install_default_signals()` runs inside start() before the cores are launched since 3.3 (Huly
    // QB-1070), so the disposition is set by the first loop turn; the poll stays as a bound.
    bool       installed = false;
    const auto deadline  = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!(installed = handler_installed(SIGTERM)) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    if (!installed) {
        // Do NOT raise SIGTERM here: at its default disposition it would kill the whole test
        // binary instead of reporting a failure. Stop the engine through SIGINT and report.
        qb::Main::registerSignal(SIGINT);
        std::raise(SIGINT);
        (void) future.wait_for(std::chrono::seconds(15));
        FAIL() << "the engine left SIGTERM at its default disposition: install_default_signals() "
                  "registers SIGINT only, so any process that does not call registerSignal(SIGTERM) "
                  "itself is killed outright by `docker stop` / `systemctl stop` — no actor teardown, "
                  "no final flush";
    }

    std::raise(SIGTERM); // nothing but the engine itself installed this handler
    EXPECT_TRUE(future.wait_for(std::chrono::seconds(15)) == std::future_status::ready)
        << "the engine installed a SIGTERM handler but did not shut down when it fired";
}

// A reload signal must NOT be terminal — otherwise fixing the above would break config reload.
//
// POSIX only: MSVC's <csignal> defines no SIGHUP, so this case cannot even compile on Windows.
// It is guarded rather than the whole file excluded, because the two cases that matter most there
// — SIGINT and SIGTERM, both standard C signals — must keep running on Windows.
#ifndef _WIN32
TEST(SignalShutdown, SighupIsNotTerminal) {
    g_hup_seen.store(0, std::memory_order_relaxed);
    // The engine must still be running after SIGHUP; it is stopped afterwards via SIGINT.
    EXPECT_FALSE(engine_stops_on<ReloadActor>(SIGHUP, std::chrono::seconds(3)))
        << "SIGHUP must not terminate the engine (it is the documented reload signal)";
    EXPECT_GT(g_hup_seen.load(), 0) << "the SIGHUP SignalEvent must still reach actors";
    std::raise(SIGINT); // let the detached engine finish
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
}
#endif // !_WIN32

// The Windows console-control bridge (Huly QB-3). SIGTERM is never OS-delivered on Windows:
// a supervisor's one group-targetable console event is CTRL_BREAK, which the MSVC CRT raises
// as SIGBREAK — and install_default_signals() translates to the SIGTERM teardown the rest of
// the pipeline (and every actor's default kill path) already speaks. In-process raise(SIGBREAK)
// exercises the same CRT dispatch the console handler uses, so this is the suite-level half;
// the out-of-process half (GenerateConsoleCtrlEvent against a child) is the example runner's
// job, whose two tier-7 post-SIGTERM tails this bridge un-gates.
//
// Windows only: SIGBREAK does not exist in POSIX <csignal>. Guarded like SighupIsNotTerminal
// above, and for the mirror reason.
#ifdef _WIN32
TEST(SignalShutdown, ConsoleBreakLandsAsSigtermViaTheBridge) {
    // engine_stops_on would call registerSignal(SIGBREAK) — the explicit, UNtranslated
    // registration. The bridge belongs to install_default_signals(), so register nothing:
    // start the engine, let it install its own set, then raise SIGBREAK and require the
    // full SIGTERM teardown.
    g_running.store(false, std::memory_order_relaxed);
    auto done   = std::make_shared<std::promise<void>>();
    auto future = done->get_future();

    std::thread([done] {
        qb::Main main;
        main.addActor<ForeverActor>(0);
        main.start();
        main.join();
        done->set_value();
    }).detach();

    const auto up_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!g_running.load(std::memory_order_relaxed) && std::chrono::steady_clock::now() < up_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ASSERT_TRUE(g_running.load()) << "the engine never came up";

    std::raise(SIGBREAK);
    EXPECT_TRUE(future.wait_for(std::chrono::seconds(15)) == std::future_status::ready)
        << "SIGBREAK did not shut the engine down: the console-control bridge in "
           "install_default_signals() is not translating it to SIGTERM";
}
#endif // _WIN32

// ---------------------------------------------------------------------------------------------
// Huly QB-65 -- a signal raised AFTER a terminal one, before the core's next pass, must not hide it.
//
// Up to 3.2.1 the engine kept ONE pending-signal slot: Main::stop() and the signal handler both
// overwrote it, and each core delivered only the signum it held at its next pass. A SIGTERM (or a
// stop()) followed by a registered non-terminal signal before that pass therefore reached every
// actor as the non-terminal one alone, and the engine never stopped -- a supervisor's SIGTERM
// racing a log-rotation SIGHUP is enough. One generation per signal number delivers both.
//
// The two signals are raised from INSIDE a pass of the only core (a LoopEvent callback), so both
// land after that pass's signal check and before the next one: deterministic, where two raises
// from another thread would race the pass.
// ---------------------------------------------------------------------------------------------
namespace sigterm_shutdown_test {

#ifdef _WIN32
// MSVC's <csignal> has no SIGUSR1/SIGHUP. SIGBREAK registered EXPLICITLY (after start(), so the
// default set's console bridge does not translate it to SIGTERM) is delivered untranslated, and is
// non-terminal for the default handler -- the Windows shape of "a reload signal".
constexpr int kNonTerminal = SIGBREAK;
#else
constexpr int kNonTerminal = SIGUSR1;
#endif

std::atomic<bool> g_started{false}; // main.start() has returned: the default signal set is installed
std::atomic<bool> g_fire{false};    // armed by the test thread once its own registration is done

class DoubleRaiseActor final
    : public qb::Actor
    , public qb::ICallback {
    bool const _stop_first;

public:
    explicit DoubleRaiseActor(bool stop_first) noexcept
        : _stop_first(stop_first) {}
    qb::io::async::task<bool>
    onInit() override {
        registerCallback(*this);
        co_return true;
    }
    void
    on(qb::LoopEvent const &) final {
        g_running.store(true, std::memory_order_relaxed);
        if (g_fire.exchange(false, std::memory_order_acq_rel)) {
            if (_stop_first)
                qb::Main::stop();
            else
                std::raise(SIGTERM);
            std::raise(kNonTerminal);
        }
    }
};

/// Starts an engine whose only actor raises SIGTERM (or calls stop()) and then kNonTerminal in one
/// pass; reports whether the engine stopped on its own within `budget`. A failing run is stopped
/// by hand afterwards, so the next case starts clean.
[[nodiscard]] bool
engine_stops_after_double_raise(bool stop_first, std::chrono::seconds budget) {
    g_running.store(false, std::memory_order_relaxed);
    g_started.store(false, std::memory_order_relaxed);
    g_fire.store(false, std::memory_order_relaxed);
    auto done   = std::make_shared<std::promise<void>>();
    auto future = done->get_future();

    std::thread([done, stop_first] {
        qb::Main main;
        main.addActor<DoubleRaiseActor>(0, stop_first);
        main.start();
        g_started.store(true, std::memory_order_release);
        main.join();
        done->set_value();
    }).detach();

    const auto up_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!(g_started.load(std::memory_order_acquire) && g_running.load(std::memory_order_relaxed))
           && std::chrono::steady_clock::now() < up_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (!g_started.load() || !g_running.load())
        return false;

    qb::Main::registerSignal(kNonTerminal); // after start(): stays untranslated, stays non-terminal
    g_fire.store(true, std::memory_order_release);
    const bool stopped = future.wait_for(budget) == std::future_status::ready;
    if (!stopped) {
        qb::Main::stop(); // the defect left the engine running: stop it for the next case
        (void) future.wait_for(std::chrono::seconds(10));
    }
    qb::Main::unregisterSignal(kNonTerminal);
    return stopped;
}

} // namespace sigterm_shutdown_test

TEST(SignalShutdown, ATerminalSignalIsNotHiddenByALaterNonTerminalOne) {
    EXPECT_TRUE(sigterm_shutdown_test::engine_stops_after_double_raise(false, std::chrono::seconds(10)))
        << "SIGTERM followed, before the core's next pass, by a registered non-terminal signal did not "
           "shut the engine down: the later signal hid the terminal one (Huly QB-65)";
}

TEST(SignalShutdown, StopIsNotHiddenByALaterSignal) {
    EXPECT_TRUE(sigterm_shutdown_test::engine_stops_after_double_raise(true, std::chrono::seconds(10)))
        << "Main::stop() followed, before the core's next pass, by a registered non-terminal signal did "
           "not shut the engine down: the signal hid the stop (Huly QB-65)";
}

// Two DIFFERENT non-terminal signals raised in one pass are both delivered, once each. POSIX only:
// Windows has a single non-terminal signal to register (SIGBREAK).
#ifndef _WIN32
namespace sigterm_shutdown_test {

std::atomic<int> g_usr1_seen{0};
std::atomic<int> g_usr2_seen{0};

class FanOutActor final
    : public qb::Actor
    , public qb::ICallback {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerCallback(*this);
        registerEvent<qb::SignalEvent>(*this); // rebind the slot to FanOutActor::on (see ReloadActor)
        co_return true;
    }
    void
    on(qb::LoopEvent const &) final {
        g_running.store(true, std::memory_order_relaxed);
        if (g_fire.exchange(false, std::memory_order_acq_rel)) {
            std::raise(SIGUSR1);
            std::raise(SIGUSR2);
        }
    }
    void
    on(qb::SignalEvent const &event) noexcept {
        if (event.signum == SIGUSR1)
            g_usr1_seen.fetch_add(1, std::memory_order_relaxed);
        if (event.signum == SIGUSR2)
            g_usr2_seen.fetch_add(1, std::memory_order_relaxed);
        qb::Actor::on(event); // both non-terminal: the engine keeps running
    }
};

} // namespace sigterm_shutdown_test

TEST(SignalShutdown, TwoDifferentSignalsInOnePassAreBothDelivered) {
    using namespace sigterm_shutdown_test;
    g_running.store(false);
    g_started.store(false);
    g_fire.store(false);
    g_usr1_seen.store(0);
    g_usr2_seen.store(0);
    auto done   = std::make_shared<std::promise<void>>();
    auto future = done->get_future();
    std::thread([done] {
        qb::Main main;
        main.addActor<FanOutActor>(0);
        main.start();
        g_started.store(true, std::memory_order_release);
        main.join();
        done->set_value();
    }).detach();

    const auto up_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!(g_started.load(std::memory_order_acquire) && g_running.load(std::memory_order_relaxed))
           && std::chrono::steady_clock::now() < up_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ASSERT_TRUE(g_started.load() && g_running.load()) << "the engine never came up";

    qb::Main::registerSignal(SIGUSR1);
    qb::Main::registerSignal(SIGUSR2);
    g_fire.store(true, std::memory_order_release);
    const auto seen_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while ((g_usr1_seen.load() == 0 || g_usr2_seen.load() == 0) && std::chrono::steady_clock::now() < seen_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    EXPECT_EQ(g_usr1_seen.load(), 1) << "SIGUSR1 must be delivered once";
    EXPECT_EQ(g_usr2_seen.load(), 1) << "SIGUSR2, raised in the same pass, must be delivered too (Huly QB-65)";

    qb::Main::stop();
    EXPECT_TRUE(future.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    qb::Main::unregisterSignal(SIGUSR1);
    qb::Main::unregisterSignal(SIGUSR2);
}
#endif // !_WIN32

// ---------------------------------------------------------------------------------------------
// Huly QB-1070 -- a signal raised while the engine starts stops it; it does not meet the disposition
// the process had before the engine.
//
// Main::start() installed its default set only once every core had passed the start barrier (and, in
// a synchronous start, just before the caller's own core). A core's actors run before that -- an
// onInit() before the barrier, the first passes right after it -- so a SIGTERM raised then met the
// disposition in force before the engine: by default it killed the process with no teardown, and so
// did the Windows bridge's SIGBREAK, which is what made `ConsoleBreakLandsAsSigtermViaTheBridge` fail
// under load. Raised from an onInit(), the signal lands in that window every time. Its disposition is
// set to SIG_IGN first, so that the defect shows as an engine that does not stop, not a dead binary.
// ---------------------------------------------------------------------------------------------
namespace sigterm_shutdown_test {

std::atomic<bool> g_init_raised{false};

/// Raises `signum` from its onInit(), then stays alive: only that signal can end this engine.
class RaiseInInit final
    : public qb::Actor
    , public qb::ICallback {
    int const _signum;

public:
    explicit RaiseInInit(int signum) noexcept
        : _signum(signum) {}
    qb::io::async::task<bool>
    onInit() override {
        registerCallback(*this);
        std::raise(_signum);
        g_init_raised.store(true);
        co_return true;
    }
    void
    on(qb::LoopEvent const &) final {}
};

/// Reports whether an engine whose actor raises `signum` from its onInit() stops on its own. A run the
/// defect left going is stopped by hand afterwards, so the next case starts clean.
[[nodiscard]] bool
engine_stops_on_a_signal_raised_in_init(int signum, std::chrono::seconds budget) {
    qb::Main::ignoreSignal(signum); // the disposition the window exposed: a missed stop, not a killed binary
    g_init_raised.store(false);
    auto done   = std::make_shared<std::promise<void>>();
    auto future = done->get_future();
    std::thread([done, signum] {
        qb::Main main;
        main.addActor<RaiseInInit>(0, signum);
        main.start();
        main.join();
        done->set_value();
    }).detach();
    const bool stopped = future.wait_for(budget) == std::future_status::ready;
    if (!stopped) {
        qb::Main::stop();
        (void) future.wait_for(std::chrono::seconds(10));
    }
    return stopped;
}

} // namespace sigterm_shutdown_test

TEST(SignalShutdown, ASigtermRaisedWhileTheEngineStartsStopsIt) {
    using namespace sigterm_shutdown_test;
    EXPECT_TRUE(engine_stops_on_a_signal_raised_in_init(SIGTERM, std::chrono::seconds(10)))
        << "a SIGTERM raised from an onInit() met the disposition the process had before the engine: start() installed "
           "the engine's handlers only after its cores had run (Huly QB-1070)";
    EXPECT_TRUE(g_init_raised.load()) << "the actor never ran its onInit()";
}

#ifdef _WIN32
// The console-control bridge raised during the start: the intermittent failure of the bridge test above,
// made deterministic.
TEST(SignalShutdown, AConsoleBreakRaisedWhileTheEngineStartsStopsIt) {
    using namespace sigterm_shutdown_test;
    EXPECT_TRUE(engine_stops_on_a_signal_raised_in_init(SIGBREAK, std::chrono::seconds(10)))
        << "a SIGBREAK raised from an onInit() was not bridged to SIGTERM: the bridge was installed after the cores "
           "had run (Huly QB-1070)";
}
#endif // _WIN32

// ---------------------------------------------------------------------------------------------
// Huly QB-364 -- starting another engine neither loses nor replays the signals of one running.
//
// Main::start() reset the process-wide signal generations to zero for its fresh cores. The cores of an
// engine already running compare their own cursors with those counters: a signal raised before the
// other engine's start and not yet seen by them was erased, and one they had already delivered came
// back as new once the counters moved again. Here the running engine's actor raises the signal and
// holds its pass until the second engine has started, so its core's next check comes after that start,
// every time.
// ---------------------------------------------------------------------------------------------
namespace sigterm_shutdown_test {

std::atomic<bool> g_fire_first{false};  // raise the first signal, delivered before the other engine starts
std::atomic<bool> g_fire_second{false}; // raise the signal the other engine's start must not erase
std::atomic<bool> g_second_raised{false};
std::atomic<bool> g_other_started{false};
std::atomic<int>  g_first_seen{0};
std::atomic<int>  g_second_seen{0};
std::atomic<int>  g_first_signum{0};
std::atomic<int>  g_second_signum{0};

/// Holds its core until the other engine has started -- a deadlock guard bounds it, never an oracle.
void
hold_until_the_other_engine_started() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!g_other_started.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
}

class RaiseThenHold final
    : public qb::Actor
    , public qb::ICallback {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerCallback(*this);
        registerEvent<qb::SignalEvent>(*this); // rebind the slot to RaiseThenHold::on (see ReloadActor)
        co_return true;
    }
    void
    on(qb::LoopEvent const &) final {
        if (g_fire_first.exchange(false, std::memory_order_acq_rel))
            std::raise(g_first_signum.load());
        if (g_fire_second.exchange(false, std::memory_order_acq_rel)) {
            std::raise(g_second_signum.load());
            g_second_raised.store(true, std::memory_order_release);
            hold_until_the_other_engine_started();
        }
    }
    void
    on(qb::SignalEvent const &event) noexcept {
        if (event.signum == g_first_signum.load())
            g_first_seen.fetch_add(1, std::memory_order_relaxed);
        if (event.signum == g_second_signum.load())
            g_second_seen.fetch_add(1, std::memory_order_relaxed);
        qb::Actor::on(event); // a terminal signal still ends it
    }
};

class Bystander final : public qb::Actor {}; // keeps the other engine's core alive

/// Polls `ready` for at most `bound` -- the polls end the wait early, they never decide the verdict.
template <typename Ready>
[[nodiscard]] bool
wait_until(Ready &&ready, std::chrono::seconds bound) {
    const auto deadline = std::chrono::steady_clock::now() + bound;
    while (!ready() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return ready();
}

/// Runs the running engine on its own thread; has its actor raise `first` (when non-zero) and waits for
/// its delivery; then has it raise `second` and hold its pass while another engine starts. Reports whether
/// `second` then reached the running engine -- for SIGTERM, whether it stopped it -- within `budget`.
/// Everything is stopped afterwards.
[[nodiscard]] bool
second_signal_survives_another_start(int first, int second, std::chrono::seconds budget) {
    g_first_signum.store(first);
    g_second_signum.store(second);
    g_fire_first.store(false);
    g_fire_second.store(false);
    g_second_raised.store(false);
    g_other_started.store(false);
    g_first_seen.store(0);
    g_second_seen.store(0);
    auto done   = std::make_shared<std::promise<void>>();
    auto future = done->get_future();
    std::thread([done] {
        qb::Main main;
        main.addActor<RaiseThenHold>(0);
        main.start();
        main.join();
        done->set_value();
    }).detach();

    if (first != 0) {
        g_fire_first.store(true, std::memory_order_release);
        if (!wait_until([] { return g_first_seen.load() >= 1; }, std::chrono::seconds(5)))
            ADD_FAILURE() << "the first signal never reached the running engine";
    }
    g_fire_second.store(true, std::memory_order_release);
    bool delivered = false;
    if (!wait_until([] { return g_second_raised.load(std::memory_order_acquire); }, std::chrono::seconds(5))) {
        ADD_FAILURE() << "the running engine never raised the second signal";
    } else {
        qb::Main other;
        other.addActor<Bystander>(0);
        other.start();
        g_other_started.store(true, std::memory_order_release);
        // Delivered, if at all, by the running core's first scan after the other engine's start -- and so is
        // anything that scan replays, which has therefore shown by then too.
        if (second == SIGTERM)
            delivered = future.wait_for(budget) == std::future_status::ready;
        else
            delivered = wait_until([] { return g_second_seen.load() >= 1; }, budget);
        qb::Main::stop(); // the other engine, and the running one unless the second signal ended it
        other.join();
    }
    g_other_started.store(true, std::memory_order_release); // release a held pass, whatever happened
    qb::Main::stop();
    (void) future.wait_for(std::chrono::seconds(10));
    return delivered;
}

} // namespace sigterm_shutdown_test

TEST(SignalShutdown, AnotherEngineStartingDoesNotEraseATerminalSignal) {
    using namespace sigterm_shutdown_test;
    qb::Main::registerSignal(SIGTERM);
    EXPECT_TRUE(second_signal_survives_another_start(0, SIGTERM, std::chrono::seconds(10)))
        << "a SIGTERM raised while one engine ran, before another engine started, never stopped the first: the other "
           "start reset the process-wide signal generations under it (Huly QB-364)";
}

#ifndef _WIN32
TEST(SignalShutdown, AnotherEngineStartingNeitherLosesNorReplaysSignals) {
    using namespace sigterm_shutdown_test;
    qb::Main::registerSignal(SIGUSR1);
    qb::Main::registerSignal(SIGUSR2);
    EXPECT_TRUE(second_signal_survives_another_start(SIGUSR1, SIGUSR2, std::chrono::seconds(5)))
        << "SIGUSR2, raised before another engine started, never reached the running engine (Huly QB-364)";
    EXPECT_EQ(g_first_seen.load(), 1) << "SIGUSR1, delivered before the other engine started, was delivered again after it";
    EXPECT_EQ(g_second_seen.load(), 1);
    qb::Main::unregisterSignal(SIGUSR1);
    qb::Main::unregisterSignal(SIGUSR2);
}
#endif // !_WIN32
