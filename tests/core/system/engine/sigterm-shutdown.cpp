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

    // `install_default_signals()` runs inside start() and is not ordered against the first loop
    // turn, so poll for the disposition instead of assuming it is set the moment an actor runs.
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
