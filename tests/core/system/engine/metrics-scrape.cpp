/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/engine/metrics-scrape.cpp
 * @brief `qb::metrics::Registry` and `qb::metrics::scrape()` on a running engine (Huly QB-154).
 *
 * A scrape asks the registry of every core for its snapshot -- the core's own `CoreStats` and the sources its
 * actors added -- through the registry of the scraping actor's core, which receives the answers. Pinned:
 *   - two cores, a registry on each and a source on core 1: one scrape returns both cores' counters, the source's
 *     sample exactly once, and `qb_scrape_core_up` 1 for each;
 *   - a core with no registry is reported `qb_scrape_core_up{core="1"} 0` once its ask times out, and the scrape
 *     still returns the cores that answered -- `qb::ask_all` would have thrown instead;
 *   - a source that throws is contained: its samples of that scrape are dropped (even those it added before it
 *     threw), the others are there, and `qb_metrics_source_failures_total` counts it;
 *   - a source whose handle was reset is no longer called, and a scrape from a core with no registry throws
 *     `std::logic_error` before it suspends.
 *
 * The scraping coroutine writes the text to globals that are read after `join()`, which orders them.
 */

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <qb/core/metrics_registry.h>
#include <qb/main.h>

namespace metrics_scrape_test {

using namespace std::chrono_literals;

struct Go : qb::Event {};
struct Done : qb::Event {};

std::string g_text;
bool        g_threw_logic_error = false;
bool        g_handle_active     = true;

bool
has_line(std::string const &text, std::string const &line) {
    return text.find(line + "\n") != std::string::npos;
}

// Adds a source to its core's registry, then tells the scraper it may start.
class Worker : public qb::Actor {
    qb::ActorId                _scraper;
    std::uint64_t              _jobs = 41;
    qb::metrics::source_handle _source;

public:
    explicit Worker(qb::ActorId scraper) noexcept
        : _scraper(scraper) {}
    qb::io::async::task<bool>
    onInit() override {
        auto *registry = getService<qb::metrics::Registry>();
        if (!registry)
            co_return false;
        _source = registry->add_source([this](qb::metrics::snapshot &s) { s.counter("test_jobs_total", "Jobs done.").add({}, _jobs); });
        push<Go>(_scraper);
        co_return true;
    }
};

// Scrapes `cores` when told to, records the text, and ends the engine.
class Scraper : public qb::Actor {
    std::vector<qb::CoreId> _cores;
    qb::duration            _timeout;
    bool                    _wait_for_go;

    void
    start() {
        auto cores   = _cores;
        auto timeout = _timeout;
        spawn([this, cores, timeout](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            // scrape() reads `*this` before it suspends; nothing of `this` is touched after.
            auto snap = co_await qb::metrics::scrape(*this, cores, timeout);
            g_text    = qb::metrics::write_text(snap);
            ctx.push<Done>();
        });
    }

public:
    Scraper(std::vector<qb::CoreId> cores, qb::duration timeout, bool wait_for_go)
        : _cores(std::move(cores))
        , _timeout(timeout)
        , _wait_for_go(wait_for_go) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Go>(*this);
        registerEvent<Done>(*this);
        if (!_wait_for_go)
            start();
        co_return true;
    }
    void
    on(Go const &) {
        start();
    }
    void
    on(Done const &) {
        broadcast<qb::KillEvent>();
    }
};

// Keeps a core alive and busy with nothing until the broadcast kill.
class Idle : public qb::Actor {};

TEST(MetricsScrape, EveryCoreAnswersThroughTheLocalRegistry) {
    if (std::thread::hardware_concurrency() < 2u)
        GTEST_SKIP() << "requires-multicore: the scrape must cross a mailbox";
    g_text.clear();
    qb::Main main;
    main.addActor<qb::metrics::Registry>(0);
    main.addActor<qb::metrics::Registry>(1);
    const auto scraper = main.addActor<Scraper>(0, std::vector<qb::CoreId>{0, 1}, qb::duration{10s}, true);
    main.addActor<Worker>(1, scraper);
    main.start(false);
    main.join();
    ASSERT_FALSE(main.hasError());

    EXPECT_TRUE(has_line(g_text, "qb_scrape_core_up{core=\"0\"} 1")) << g_text;
    EXPECT_TRUE(has_line(g_text, "qb_scrape_core_up{core=\"1\"} 1")) << g_text;
    EXPECT_TRUE(has_line(g_text, "test_jobs_total 41")) << g_text;
    EXPECT_NE(g_text.find("qb_core_loop_passes_total{core=\"0\"} "), std::string::npos) << g_text;
    EXPECT_NE(g_text.find("qb_core_loop_passes_total{core=\"1\"} "), std::string::npos) << g_text;
    EXPECT_TRUE(has_line(g_text, "qb_metrics_source_failures_total{core=\"0\"} 0")) << g_text;
    EXPECT_TRUE(has_line(g_text, "qb_metrics_source_failures_total{core=\"1\"} 0")) << g_text;
    EXPECT_TRUE(has_line(g_text, "qb_scrape_conflicts 0")) << g_text;
    EXPECT_NE(g_text.find("# TYPE qb_scrape_duration_seconds gauge\n"), std::string::npos);
}

TEST(MetricsScrape, ACoreWithoutARegistryIsReportedDown) {
    if (std::thread::hardware_concurrency() < 2u)
        GTEST_SKIP() << "requires-multicore: core 1 must run without a registry";
    g_text.clear();
    qb::Main main;
    main.addActor<qb::metrics::Registry>(0);
    main.addActor<Idle>(1);
    main.addActor<Scraper>(0, std::vector<qb::CoreId>{0, 1}, qb::duration{100ms}, false);
    main.start(false);
    main.join();
    ASSERT_FALSE(main.hasError());

    EXPECT_TRUE(has_line(g_text, "qb_scrape_core_up{core=\"0\"} 1")) << g_text;
    EXPECT_TRUE(has_line(g_text, "qb_scrape_core_up{core=\"1\"} 0")) << g_text;
    EXPECT_NE(g_text.find("qb_core_loop_passes_total{core=\"0\"} "), std::string::npos) << g_text;
    EXPECT_EQ(g_text.find("qb_core_loop_passes_total{core=\"1\"}"), std::string::npos) << g_text;
}

// Adds a throwing source and a sound one on its own core, scrapes it, removes a third source first.
class SelfScraper : public qb::Actor {
    qb::metrics::source_handle _throws;
    qb::metrics::source_handle _sound;

public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Done>(*this);
        auto *registry = getService<qb::metrics::Registry>();
        if (!registry)
            co_return false;
        _throws      = registry->add_source([](qb::metrics::snapshot &s) {
            s.counter("test_partial_total", "Added, then the source throws.").add({}, 1);
            throw std::runtime_error("a source that fails");
        });
        _sound       = registry->add_source([](qb::metrics::snapshot &s) { s.gauge("test_sound", "A sound source.").add({}, 7); });
        auto removed = registry->add_source([](qb::metrics::snapshot &s) { s.gauge("test_removed", "Removed before the scrape.").add({}, 1); });
        removed.reset();
        g_handle_active = removed.active();
        spawn([this](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            auto snap = co_await qb::metrics::scrape(*this, std::vector<qb::CoreId>{0}, qb::duration{10s});
            g_text    = qb::metrics::write_text(snap);
            ctx.push<Done>();
        });
        co_return true;
    }
    void
    on(Done const &) {
        broadcast<qb::KillEvent>();
    }
};

TEST(MetricsScrape, AThrowingSourceIsContainedAndAResetOneIsGone) {
    g_text.clear();
    g_handle_active = true;
    qb::Main main;
    main.addActor<qb::metrics::Registry>(0);
    main.addActor<SelfScraper>(0);
    main.start(false);
    main.join();
    ASSERT_FALSE(main.hasError());

    EXPECT_TRUE(has_line(g_text, "test_sound 7")) << g_text;
    EXPECT_EQ(g_text.find("test_partial_total"), std::string::npos) << "the samples of a source that threw are dropped:\n" << g_text;
    EXPECT_TRUE(has_line(g_text, "qb_metrics_source_failures_total{core=\"0\"} 1")) << g_text;
    EXPECT_EQ(g_text.find("test_removed"), std::string::npos) << g_text;
    EXPECT_FALSE(g_handle_active);
    EXPECT_TRUE(has_line(g_text, "qb_scrape_core_up{core=\"0\"} 1")) << g_text;
}

// Scrapes with no registry on its core: scrape() refuses before it suspends.
class Orphan : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        try {
            (void) qb::metrics::scrape(*this, std::vector<qb::CoreId>{0}, qb::duration{1s});
        } catch (std::logic_error const &) {
            g_threw_logic_error = true;
        }
        kill();
        co_return true;
    }
};

TEST(MetricsScrape, AScrapeFromACoreWithoutARegistryThrows) {
    g_threw_logic_error = false;
    qb::Main main;
    main.addActor<Orphan>(0);
    main.start(false);
    main.join();
    ASSERT_FALSE(main.hasError());
    EXPECT_TRUE(g_threw_logic_error);
}

} // namespace metrics_scrape_test
