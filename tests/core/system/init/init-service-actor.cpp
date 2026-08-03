/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/init/init-service-actor.cpp
 * @brief A `qb::ServiceActor` singleton with an ASYNC `onInit()` goes through the Activating phase.
 *
 * A `ServiceActor<Tag>` is a per-core singleton, but it is an Actor first: its async `onInit()`
 * suspends and the service is *Activating* exactly like any other actor. This file proves a
 * service that `co_await`s during init still gets the full activation treatment — an event
 * addressed to it while it is Activating is stashed and replayed only once it activates (never
 * delivered to a half-initialized service).
 *
 * The in-service `EXPECT`-equivalent (the "served only after init" invariant) is observed via a
 * global atomic set at the end of `onInit`, and a post-`join()` assertion confirms both that the
 * service activated and that the stashed `Poke` landed strictly after activation — so a service
 * that never ran cannot make the test pass vacuously.
 *
 * Run under ASAN_OPTIONS=detect_leaks=0 like the rest of the actor-coroutine suites.
 */

#include <atomic>
#include <chrono>

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/main.h>

#include "../../shared/InitFixtures.h"

using namespace std::chrono_literals;

namespace {

struct AsyncSvcTag {};
struct Poke : public qb::Event {};

std::atomic<bool> g_svc_inited{false};
std::atomic<bool> g_svc_served_after{true};
std::atomic<bool> g_svc_poked{false};

class AsyncService : public qb::ServiceActor<AsyncSvcTag> {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Poke>(*this);
        co_await context().sleep(30ms); // service is Activating during this window
        g_svc_inited.store(true);
        co_return true;
    }
    void
    on(Poke &) {
        if (!g_svc_inited.load())
            g_svc_served_after.store(false); // a stashed event must land only after activation
        g_svc_poked.store(true);
        kill();
    }
};

class ServicePoker : public qb::Actor {
    qb::ActorId _svc;

public:
    explicit ServicePoker(qb::ActorId svc)
        : _svc(svc) {}
    qb::io::async::task<bool>
    onInit() override {
        push<Poke>(_svc); // sent while the service is still Activating → stashed
        kill();
        co_return true;
    }
};

TEST(InitServiceActor, AsyncOnInitStashesThenServesAfterActivation) {
    g_svc_inited.store(false);
    g_svc_served_after.store(true);
    g_svc_poked.store(false);
    qb::Main   main;
    const auto svc = main.addActor<AsyncService>(0);
    main.addActor<ServicePoker>(0, svc);
    main.start(false);
    main.join();
    EXPECT_TRUE(g_svc_inited.load());       // the service's async onInit completed
    EXPECT_TRUE(g_svc_poked.load());        // the stashed Poke was replayed (not dropped)
    EXPECT_TRUE(g_svc_served_after.load()); // ...and only AFTER the service activated
    EXPECT_FALSE(main.hasError());
}

} // namespace
