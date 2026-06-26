/**
 * @file qb/io/tests/benchmark/async/ev-backends.cpp
 * @brief Cross-backend libev stress benchmark — finds the best backend on THIS machine.
 *
 * Unlike the per-backend ctest matrix (which forces one backend per process via
 * QB_EV_BACKEND), this benchmark sweeps EVERY backend libev was built with in a
 * SINGLE run: it creates a dedicated ev_loop per backend (ev_loop_new(EVBACKEND_*)),
 * skipping any that fail to initialise at runtime (e.g. io_uring under a seccomp
 * sandbox), and runs two stress workloads on each. Google Benchmark prints the
 * results side by side so the winner is obvious.
 *
 * Workloads:
 *   - Dispatch/<backend>/<N>     : N socketpairs, ALL active each round. Raw
 *                                  readiness-dispatch throughput.
 *   - ActiveFew/<backend>/<N>    : N registered watchers, only K=16 active each
 *                                  round. THE discriminator: select/poll scan all
 *                                  N fds per poll (O(N)); epoll/kqueue/io_uring
 *                                  report only the ready ones (O(K)). As N grows,
 *                                  O(N) backends fall off a cliff — that gap is the
 *                                  reason epoll/kqueue/io_uring exist.
 *
 * Reading it: at the largest N, the backend with the highest items_per_second (and
 * flat scaling vs N) is the best on this machine. select self-reports its hard wall
 * (FD_SETSIZE) by skipping large-N cases instead of crashing.
 *
 * Run a single backend explicitly with --benchmark_filter, e.g.
 *   ./qb-io-benchmark-ev-backends --benchmark_filter='ActiveFew/(kqueue|select)'
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * @ingroup IO
 */

#include <array>
#include <benchmark/benchmark.h>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <qb/io/async/listener.h> /* brings ev.h (C API: ev_loop_new/EVBACKEND_*) + ev++.h (ev::) */

#ifndef _WIN32
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

#ifndef _WIN32

constexpr int ACTIVE_K = 16; /* number of fds made ready per round in ActiveFew */

/* Per-run dispatch counter. Google Benchmark runs benchmarks sequentially (no
 * concurrency between them), so a single file-scope counter is safe. */
long g_events = 0;

void
on_readable(ev::io &w, int) {
    char buf[256];
    while (::read(w.fd, buf, sizeof buf) > 0) {
    }
    ++g_events;
}

void
set_nonblock(int fd) {
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

/* select() on most platforms is an fd_set bitset/array capped at FD_SETSIZE; libev
 * asserts fd < FD_SETSIZE in the select backend, so we must not register past it. */
bool
backend_fits(unsigned backend, int total_fds) {
    if (backend == EVBACKEND_SELECT)
        return total_fds < FD_SETSIZE - 8; /* leave room for stdio + the loop's own fds */
    return true;
}

struct LoopFixture {
    struct ev_loop *loop = nullptr;
    explicit LoopFixture(unsigned backend) {
        loop = ev_loop_new(backend);
        if (loop && ev_backend(loop) != backend) { /* fell back to another backend */
            ev_loop_destroy(loop);
            loop = nullptr;
        }
    }
    ~LoopFixture() {
        if (loop)
            ev_loop_destroy(loop);
    }
    LoopFixture(const LoopFixture &)            = delete;
    LoopFixture &operator=(const LoopFixture &) = delete;
};

/* Make `pairs` socketpairs, register a read watcher on each readable end. Returns
 * false (and reports a skip) if the process fd budget is exhausted. */
bool
setup_pairs(benchmark::State &state, ev::loop_ref loop, int pairs, std::vector<std::array<int, 2>> &sv,
            std::vector<std::unique_ptr<ev::io>> &ws) {
    sv.resize(static_cast<std::size_t>(pairs));
    ws.reserve(static_cast<std::size_t>(pairs));
    for (int i = 0; i < pairs; ++i) {
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv[(std::size_t) i].data()) != 0) {
            state.SkipWithError("socketpair failed (raise RLIMIT_NOFILE)");
            sv.resize(static_cast<std::size_t>(i));
            return false;
        }
        set_nonblock(sv[(std::size_t) i][0]);
        ws.emplace_back(std::make_unique<ev::io>(loop));
        ws.back()->set<&on_readable>();
        ws.back()->start(sv[(std::size_t) i][0], ev::READ);
    }
    return true;
}

void
teardown_pairs(std::vector<std::array<int, 2>> &sv, std::vector<std::unique_ptr<ev::io>> &ws) {
    for (auto &w : ws)
        w->stop();
    for (auto &p : sv) {
        ::close(p[0]);
        ::close(p[1]);
    }
}

/* Workload 1: every registered fd is made ready each round. */
void
bench_dispatch(benchmark::State &state, unsigned backend) {
    const int N = static_cast<int>(state.range(0));
    if (!backend_fits(backend, N)) {
        state.SkipWithError("exceeds FD_SETSIZE");
        return;
    }

    LoopFixture fx(backend);
    if (!fx.loop) {
        state.SkipWithError("backend unavailable at runtime");
        return;
    }
    ev::loop_ref loop(fx.loop);

    std::vector<std::array<int, 2>>      sv;
    std::vector<std::unique_ptr<ev::io>> ws;
    if (!setup_pairs(state, loop, N, sv, ws)) {
        teardown_pairs(sv, ws);
        return;
    }

    ev_run(fx.loop, EVRUN_NOWAIT); /* warm-up: flush the one-time registration changelist */
    g_events = 0;

    std::int64_t dispatched = 0;
    for (auto _ : state) {
        g_events = 0;
        for (int i = 0; i < N; ++i) {
            const char b = 'x';
            (void) !::write(sv[(std::size_t) i][1], &b, 1);
        }
        long guard = 0;
        while (g_events < N && ++guard < 4000000L)
            ev_run(fx.loop, EVRUN_NOWAIT);
        dispatched += g_events;
    }
    teardown_pairs(sv, ws);
    state.SetItemsProcessed(dispatched);
    state.counters["fds"] = N;
}

/* Workload 2 (discriminator): N registered, only ACTIVE_K made ready each round.
 * Cost per round ~ O(N) for select/poll, ~ O(ACTIVE_K) for epoll/kqueue/io_uring. */
void
bench_active_few(benchmark::State &state, unsigned backend) {
    const int N = static_cast<int>(state.range(0));
    if (!backend_fits(backend, N)) {
        state.SkipWithError("exceeds FD_SETSIZE");
        return;
    }

    LoopFixture fx(backend);
    if (!fx.loop) {
        state.SkipWithError("backend unavailable at runtime");
        return;
    }
    ev::loop_ref loop(fx.loop);

    std::vector<std::array<int, 2>>      sv;
    std::vector<std::unique_ptr<ev::io>> ws;
    if (!setup_pairs(state, loop, N, sv, ws)) {
        teardown_pairs(sv, ws);
        return;
    }

    ev_run(fx.loop, EVRUN_NOWAIT); /* warm-up: flush the one-time registration changelist */
    g_events = 0;

    const int    K          = N < ACTIVE_K ? N : ACTIVE_K;
    std::int64_t dispatched = 0;
    std::size_t  cursor     = 0; /* rotate which fds are active to spread load */
    for (auto _ : state) {
        g_events = 0;
        for (int j = 0; j < K; ++j) {
            std::size_t idx = (cursor + (std::size_t) j) % (std::size_t) N;
            const char  b   = 'x';
            (void) !::write(sv[idx][1], &b, 1);
        }
        cursor     = (cursor + (std::size_t) K) % (std::size_t) N;
        long guard = 0;
        while (g_events < K && ++guard < 4000000L)
            ev_run(fx.loop, EVRUN_NOWAIT);
        dispatched += g_events;
    }
    teardown_pairs(sv, ws);
    state.SetItemsProcessed(dispatched);
    state.counters["registered"] = N;
}

const char *
backend_name(unsigned b) {
    switch (b) {
        case EVBACKEND_SELECT:
            return "select";
        case EVBACKEND_POLL:
            return "poll";
        case EVBACKEND_EPOLL:
            return "epoll";
        case EVBACKEND_KQUEUE:
            return "kqueue";
        case EVBACKEND_IOURING:
            return "iouring";
        case EVBACKEND_LINUXAIO:
            return "linuxaio";
        default:
            return "other";
    }
}

void
register_all() {
    static const unsigned candidates[] = {
        EVBACKEND_SELECT, EVBACKEND_POLL, EVBACKEND_EPOLL, EVBACKEND_KQUEUE, EVBACKEND_IOURING, EVBACKEND_LINUXAIO,
    };
    const unsigned supported = ev_supported_backends();

    for (unsigned be : candidates) {
        if (!(supported & be))
            continue;
        const std::string nm = backend_name(be);

        const bool scalable = (be != EVBACKEND_SELECT && be != EVBACKEND_POLL);

        auto *d = benchmark::RegisterBenchmark(("Dispatch/" + nm).c_str(), [be](benchmark::State &st) { bench_dispatch(st, be); });
        d->Arg(64)->Arg(512)->Arg(2048)->Arg(8192);
        d->Unit(benchmark::kMicrosecond);

        auto *a = benchmark::RegisterBenchmark(("ActiveFew/" + nm).c_str(), [be](benchmark::State &st) { bench_active_few(st, be); });
        a->Arg(256)->Arg(2048)->Arg(8192);
        /* Heavy scale only for O(active) backends: poll() EINVALs (and libev aborts)
         * on huge nfds on some platforms, and select is already past FD_SETSIZE here. */
        if (scalable)
            a->Arg(20000)->Arg(50000);
        a->Unit(benchmark::kMicrosecond);
    }
}

#endif /* !_WIN32 */

} // namespace

int
main(int argc, char **argv) {
#ifndef _WIN32
    /* Raise the open-fd budget so the large-N cases can run (best effort). */
    struct rlimit rl;
    if (::getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        rl.rlim_cur = rl.rlim_max;
        ::setrlimit(RLIMIT_NOFILE, &rl);
    }
    std::printf("libev supported backends = 0x%x\n", ev_supported_backends());
    /* Winner-summary hint: name the backend libev auto-selected for the default
     * loop on THIS machine — at the largest ActiveFew/N it is also the row with
     * the highest items_per_second and the flattest scaling vs N. */
    if (struct ev_loop *def = ev_default_loop(0)) {
        std::printf("libev default (recommended) backend = %s\n", backend_name(ev_backend(def)));
    }
    register_all();
#endif
    benchmark::Initialize(&argc, argv);
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
