/**
 * @file qb/source/io/tests/system/bench-io-plan.cpp
 * @brief Micro-benchmarks for the QB_IO_PLAN hot paths.
 *
 * These benchmarks measure the cost of the allocation-heavy hot paths that
 * findings 2.13, 2.15 and 2.20 aim to improve:
 *
 *   - `listener::registerEvent` / `unregisterEvent` (per-event `new`/`delete`
 *     plus `std::unordered_set<void*>` insert/erase).
 *   - `async::callback` (heap allocation of `Timeout<_Func>` + self-`delete`).
 *   - `async::scoped_callback` (single allocation, no self-delete dance).
 *   - `io_handler::stream()` broadcast fan-out (validates the
 *     `_broadcast_scratch` reuse added for finding 2.12).
 *
 * They are NOT gtest assertions: the goal is to produce a stable, tunable
 * number (ns/op) that can be compared before and after the planned
 * refactors. A baseline is captured today; the same executable is run after
 * the optimisations land and deltas are recorded in QB_IO_PLAN.md.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2025 qb - isndev (cpp.actor)
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <qb/io/async.h>
#include <qb/io/async/event/io.h>
#include <qb/io/async/event/timer.h>
#include <qb/io/async/io_handler.h>
#include <qb/io/async/listener.h>

using namespace std::chrono_literals;

namespace {

// ---------------------------------------------------------------------------
// Tiny bench helpers (no Google Benchmark dependency → zero risk of drift).
// ---------------------------------------------------------------------------
struct BenchResult {
    std::string   name;
    std::size_t   iters;
    double        total_ns;
    double        ns_per_op;
};

std::vector<BenchResult> g_results;

template <typename _Fn>
BenchResult run_bench(std::string name, std::size_t iters, _Fn &&fn) {
    using clock = std::chrono::steady_clock;
    // Warm-up to settle allocator / TLB / caches.
    const std::size_t warmup = std::min<std::size_t>(iters / 10, 10'000);
    for (std::size_t i = 0; i < warmup; ++i) fn();

    const auto start = clock::now();
    for (std::size_t i = 0; i < iters; ++i) fn();
    const auto end = clock::now();

    const double total_ns =
        std::chrono::duration<double, std::nano>(end - start).count();
    BenchResult r{std::move(name), iters, total_ns, total_ns / double(iters)};
    g_results.push_back(r);
    std::printf("  %-48s  iters=%-10zu  total=%10.3f ms   %8.1f ns/op\n",
                r.name.c_str(), r.iters, r.total_ns / 1e6, r.ns_per_op);
    return r;
}

// Dummy actor that satisfies the listener's `on(_Event&)` contract without
// performing any work — we only measure the framework machinery.
struct NopActor {
    void on(qb::io::async::event::io const &) noexcept {}
    void on(qb::io::async::event::timer const &) noexcept {}
};

// ---------------------------------------------------------------------------
// 1. registerEvent / unregisterEvent round-trip.
// ---------------------------------------------------------------------------
BenchResult bench_register_unregister(std::size_t iters) {
    NopActor actor;
    auto &L = qb::io::async::listener::current;
    return run_bench("listener::registerEvent+unregisterEvent (EV_NONE)", iters,
                     [&] {
                         auto &ev = L.registerEvent<qb::io::async::event::io>(
                             actor, -1, EV_NONE);
                         L.unregisterEvent(ev._interface);
                     });
}

// ---------------------------------------------------------------------------
// 2. async::callback (heap alloc + self-delete via libev fire).
//    We drive a 0-second timer so it fires in a single loop iteration.
// ---------------------------------------------------------------------------
BenchResult bench_async_callback_fire(std::size_t iters) {
    std::atomic<std::size_t> counter{0};
    return run_bench("async::callback (immediate, heap alloc + libev fire)", iters,
                     [&] {
                         qb::io::async::callback([&] { ++counter; }, qb::duration::zero());
                         qb::io::async::listener::current.run(EVRUN_NOWAIT);
                     });
}

// ---------------------------------------------------------------------------
// 3. scoped_callback (RAII, allocation path only — timer cancelled at dtor).
// ---------------------------------------------------------------------------
BenchResult bench_scoped_callback_ctor_dtor(std::size_t iters) {
    return run_bench("scoped_callback (allocate + cancel via dtor)", iters,
                     [&] {
                         auto h = qb::io::async::scoped_callback([] {}, 1s);
                         (void)h;
                     });
}

// ---------------------------------------------------------------------------
// 4. io_handler::stream() broadcast fan-out (validates the broadcast-scratch
//    reuse from finding 2.12 and gives a perf number for N sessions).
// ---------------------------------------------------------------------------
class DummySession {
public:
    using transport_type = int; // unused, placeholder
    explicit DummySession(int id = 0) : _id(id) {}

    void on_data(qb::allocator::pipe<char> &) {}
    int  &id() noexcept { return _id; }
    bool  is_alive() const noexcept { return true; }
    bool  is_valid() const noexcept { return true; }

    template <typename T>
    DummySession &operator<<(T const &) noexcept { return *this; }

private:
    int _id;
};

// We don't run an actual io_handler broadcast here because it would require
// full transport wiring — instead we measure the pure vector reuse path that
// the fix targets: push N pointers, traverse, clear, iterate.
BenchResult bench_broadcast_scratch_reuse(std::size_t iters, std::size_t sessions) {
    std::vector<DummySession *> pool;
    pool.reserve(sessions);
    for (std::size_t i = 0; i < sessions; ++i)
        pool.push_back(new DummySession(static_cast<int>(i)));

    std::vector<DummySession *> scratch; // reused across iterations
    scratch.reserve(sessions);

    auto res = run_bench(
        "broadcast scratch reuse (" + std::to_string(sessions) + " sessions)", iters,
        [&] {
            scratch.clear();
            for (auto *s : pool)
                scratch.push_back(s);
            std::size_t touched = 0;
            for (auto *s : scratch)
                touched += static_cast<std::size_t>(s->id() & 1);
            // Prevent optimisation from removing the work.
            if (touched == ~std::size_t{0}) std::abort();
        });

    for (auto *p : pool) delete p;
    return res;
}

void write_report(std::string_view label, std::string_view outpath) {
    std::FILE *f = std::fopen(outpath.data(), "w");
    if (!f) {
        std::fprintf(stderr, "bench-io-plan: cannot open %.*s for writing\n",
                     (int)outpath.size(), outpath.data());
        return;
    }
    std::fprintf(f, "# qb-io micro-benchmark report (%.*s)\n",
                 (int)label.size(), label.data());
    std::fprintf(f, "# %-46s  %-10s  %-14s  %-10s\n", "bench", "iters",
                 "total_ms", "ns_per_op");
    for (const auto &r : g_results) {
        std::fprintf(f, "%-48s  %-10zu  %-14.3f  %-10.1f\n", r.name.c_str(),
                     r.iters, r.total_ns / 1e6, r.ns_per_op);
    }
    std::fclose(f);
    std::printf("\nReport written to %.*s\n", (int)outpath.size(),
                outpath.data());
}

} // namespace

int
main(int argc, char **argv) {
    std::string label   = "baseline";
    std::string outpath = "/tmp/qb-io-bench.txt";

    for (int i = 1; i < argc; ++i) {
        std::string_view a{argv[i]};
        if (a == "--label" && i + 1 < argc)
            label = argv[++i];
        else if (a == "--out" && i + 1 < argc)
            outpath = argv[++i];
    }

    qb::io::async::init();

    std::printf("== qb-io micro-benchmarks (label=%s) ==\n\n", label.c_str());

    // Tune iters so each bench runs ~0.2 s on modern hardware.
    bench_register_unregister(200'000);
    bench_async_callback_fire(50'000);
    bench_scoped_callback_ctor_dtor(200'000);
    bench_broadcast_scratch_reuse(100'000, 16);
    bench_broadcast_scratch_reuse(10'000, 256);
    bench_broadcast_scratch_reuse(1'000, 4'096);

    write_report(label, outpath);

    // Drain anything libev still queued (there shouldn't be — but be safe).
    qb::io::async::listener::current.clear();

    return 0;
}
