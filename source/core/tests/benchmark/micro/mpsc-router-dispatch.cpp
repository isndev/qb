/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file benchmark/micro/mpsc-router-dispatch.cpp
 * @brief MPSC mailbox ingress + `qb::router::memh` typed dispatch (pseudo-framework hot path).
 *
 * Models the engine's receive hot path without a `qb::Main`: multiple producers fan three trivially-
 * copyable message types (`MsgOrder` / `MsgPing` / `MsgNotify`, all `<= sizeof(EventBucket)`) into a
 * `qb::lockfree::mpsc::ringbuffer<EventBucket, Cap, 0>`, and one consumer drains by batch and routes
 * each bucket through `qb::router::memh<BenchEvt>` to a typed handler — the same `route()` the engine
 * uses. The handlers fold each message into a consumer-LOCAL checksum (no per-message atom in the hot
 * path), so the dispatch work stays observable yet cheap.
 *
 * The launch / drain scaffold (start-gate barrier, one `qb::jthread` per producer, batched
 * `dequeue(Func, buf, batch)` consumer) lives once in `MpscFanInHarness.h`. This bench supplies a
 * `make_bucket` that round-robins the three message types and a `consume` that routes the batch.
 *
 * Aliasing: a dequeued `EventBucket*` is raw cache-line storage, not a live `BenchEvt`. We therefore
 * `std::memcpy` the bytes into a properly-typed `BenchEvt` (then route a `std::launder`ed view of the
 * in-buffer object for the per-type dispatch) instead of a bare `reinterpret_cast` — no strict-
 * aliasing UB, identical machine work to the engine's in-place reinterpret on a trivially-copyable
 * layout.
 *
 * Benchmark methodology (perf harness, never a ctest gate — no `EXPECT_LT(duration,…)`):
 *   - the timed region is `run_router_mailbox()`; thread spin-up is absorbed by the start gate;
 *   - `SetItemsProcessed` / `SetBytesProcessed` and the descriptor counters (the final checksum and
 *     reserved-bytes footprint) are assigned ONCE after the loop;
 *   - a one-shot, out-of-loop probe runs the smallest fan-in, `DoNotOptimize`s the consumer checksum,
 *     and asserts the dispatch actually happened (`checksum != 0`) — so a router that silently routed
 *     nothing (every message mis-routed) is caught before any timing, not turned into a timing gate.
 *
 * Rewritten from the former `bm-mpsc-router-mailbox.cpp` onto the shared harness.
 */

#include <algorithm>
#include <benchmark/benchmark.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>
#include <typeinfo>

#include <qb/core/ActorId.h>
#include <qb/core/Event.h>
#include <qb/system/event/router.h>
#include <qb/utility/prefix.h>

#include "../../shared/MpscFanInHarness.h"

namespace {

using ::EventBucket; // EventBucket is declared in the GLOBAL namespace (qb/utility/prefix.h), not qb::

// Minimal routable event (the engine event ABI surface `qb::router::memh` consults: id + dest).
struct BenchEvt {
#ifdef NDEBUG
    using id_type = qb::EventId;
#else
    using id_type = const char *;
#endif
    using id_handler_type = qb::ActorId;

    std::uint16_t bucket_size = 1;
    id_type       evt_id{};
    qb::ActorId   dest{};
    qb::ActorId   source{};

    template <typename T>
    [[nodiscard]] static id_type
    type_to_id() noexcept {
#ifndef NDEBUG
        return typeid(T).name();
#else
        return qb::detail::type_id_for<T>();
#endif
    }

    [[nodiscard]] id_type
    getID() const noexcept {
        return evt_id;
    }
    [[nodiscard]] qb::ActorId
    getDestination() const noexcept {
        return dest;
    }
    [[nodiscard]] qb::ActorId
    getSource() const noexcept {
        return source;
    }
    [[nodiscard]] bool
    is_alive() const noexcept {
        return true;
    }
};

struct MsgOrder final : BenchEvt {
    std::uint32_t seq = 0;
};
struct MsgPing final : BenchEvt {
    std::uint8_t hop = 0;
};
struct MsgNotify final : BenchEvt {
    std::uint16_t code = 0;
};

static_assert(std::is_trivially_copyable_v<MsgOrder>);
static_assert(std::is_trivially_copyable_v<MsgPing>);
static_assert(std::is_trivially_copyable_v<MsgNotify>);
static_assert(sizeof(MsgOrder) <= sizeof(EventBucket));
static_assert(sizeof(MsgPing) <= sizeof(EventBucket));
static_assert(sizeof(MsgNotify) <= sizeof(EventBucket));

[[nodiscard]] constexpr std::uint32_t
pack_actor(std::uint16_t const sid, std::uint16_t const core = 0) noexcept {
    return (static_cast<std::uint32_t>(core) << 16) | static_cast<std::uint32_t>(sid);
}

constexpr std::uint32_t kBenchDstOrder = pack_actor(201);
constexpr std::uint32_t kBenchDstPing  = pack_actor(202);
constexpr std::uint32_t kBenchDstNote  = pack_actor(203);

// Consumer-local checksum: keeps handler work observable without an atomic per message.
struct LocalChecksum {
    std::uint64_t value = 0;
    void
    mix(std::uint64_t const x) noexcept {
        value ^= x + 0x9E3779B97F4A7C15ULL + (value << 6) + (value >> 2);
    }
};

class HandlerOrder final {
    const qb::ActorId _id;
    LocalChecksum    *_checksum;

public:
    HandlerOrder(std::uint32_t const packed, LocalChecksum &checksum) noexcept
        : _id(packed)
        , _checksum(&checksum) {}
    [[nodiscard]] qb::ActorId
    id() const noexcept {
        return _id;
    }
    [[nodiscard]] bool
    is_alive() const noexcept {
        return true;
    }
    void
    on(MsgOrder &m) noexcept {
        _checksum->mix(static_cast<std::uint64_t>(m.seq) * 0x9E3779B97F4A7C15ULL);
    }
};

class HandlerPing final {
    const qb::ActorId _id;
    LocalChecksum    *_checksum;

public:
    HandlerPing(std::uint32_t const packed, LocalChecksum &checksum) noexcept
        : _id(packed)
        , _checksum(&checksum) {}
    [[nodiscard]] qb::ActorId
    id() const noexcept {
        return _id;
    }
    [[nodiscard]] bool
    is_alive() const noexcept {
        return true;
    }
    void
    on(MsgPing &m) noexcept {
        _checksum->mix(static_cast<std::uint64_t>(m.hop) << 8);
    }
};

class HandlerNotify final {
    const qb::ActorId _id;
    LocalChecksum    *_checksum;

public:
    HandlerNotify(std::uint32_t const packed, LocalChecksum &checksum) noexcept
        : _id(packed)
        , _checksum(&checksum) {}
    [[nodiscard]] qb::ActorId
    id() const noexcept {
        return _id;
    }
    [[nodiscard]] bool
    is_alive() const noexcept {
        return true;
    }
    void
    on(MsgNotify &m) noexcept {
        _checksum->mix(static_cast<std::uint64_t>(m.code) * 3u);
    }
};

// Build one typed message into a fresh EventBucket. memcpy of a trivially-copyable layout — no
// reinterpret-cast aliasing on the write side.
template <typename Msg>
[[nodiscard]] EventBucket
write_bucket(qb::ActorId const dst, std::uint32_t const salt) noexcept {
    Msg m{};
    m.bucket_size = 1;
    m.evt_id      = BenchEvt::type_to_id<Msg>();
    m.dest        = dst;
    m.source      = qb::ActorId{};

    if constexpr (std::is_same_v<Msg, MsgOrder>) {
        m.seq = salt;
    } else if constexpr (std::is_same_v<Msg, MsgPing>) {
        m.hop = static_cast<std::uint8_t>(salt & 0xFFu);
    } else {
        m.code = static_cast<std::uint16_t>(salt & 0xFFFFu);
    }

    EventBucket out{};
    std::memcpy(&out, &m, sizeof(Msg));
    return out;
}

// make_bucket: round-robin the three message types across each producer's quota.
[[nodiscard]] EventBucket
make_routed_bucket(std::size_t const pid, std::uint64_t const sent) noexcept {
    const auto salt = static_cast<std::uint32_t>(sent + pid * 997u);
    switch ((sent + pid) % 3u) {
        case 0u:
            return write_bucket<MsgOrder>(qb::ActorId{kBenchDstOrder}, salt);
        case 1u:
            return write_bucket<MsgPing>(qb::ActorId{kBenchDstPing}, salt);
        default:
            return write_bucket<MsgNotify>(qb::ActorId{kBenchDstNote}, salt);
    }
}

struct RouterRunResult {
    std::uint64_t enqueue_failures = 0;
    std::uint64_t checksum         = 0;
};

template <std::size_t MailboxCap>
[[nodiscard]] RouterRunResult
run_router_mailbox(std::size_t const nb_producers, std::uint64_t const total, std::size_t const dequeue_batch) {
    LocalChecksum local_checksum{};

    qb::router::memh<BenchEvt> router;
    HandlerOrder               h_order(kBenchDstOrder, local_checksum);
    HandlerPing                h_ping(kBenchDstPing, local_checksum);
    HandlerNotify              h_note(kBenchDstNote, local_checksum);
    router.subscribe<MsgOrder>(h_order);
    router.subscribe<MsgPing>(h_ping);
    router.subscribe<MsgNotify>(h_note);

    const auto consume = [&router](EventBucket *buffer, std::size_t const nb_buckets) noexcept {
        for (std::size_t i = 0; i < nb_buckets; ++i) {
            // The dequeued bucket is raw storage. memcpy its bytes into a typed BenchEvt header in
            // place (the buckets ARE trivially-copyable messages), then route a std::launder'd view —
            // strict-aliasing-clean, same machine work as the engine's in-place reinterpret.
            BenchEvt header{};
            std::memcpy(&header, buffer + i, sizeof(BenchEvt));
            std::memcpy(buffer + i, &header, sizeof(BenchEvt));
            auto *evt = std::launder(reinterpret_cast<BenchEvt *>(buffer + i));
            router.route(*evt, [](BenchEvt &) noexcept {
                // Mis-routed message: must never happen in this benchmark.
            });
        }
    };

    const std::uint64_t fails =
        qb::bench::run_mpsc_fan_in<MailboxCap>(nb_producers, total, dequeue_batch, make_routed_bucket, consume);

    return {fails, local_checksum.value};
}

template <std::size_t MailboxCap>
void
BM_MpscRouterMailbox_FanIn(benchmark::State &state) {
    const auto nb_producers  = static_cast<std::size_t>(state.range(0));
    const auto total         = static_cast<std::uint64_t>(state.range(1));
    const auto dequeue_batch = static_cast<std::size_t>(state.range(2));

    if (nb_producers == 0u || total == 0ull) {
        state.SkipWithError("invalid range: producers or total is zero");
        return;
    }

    // One-shot out-of-loop correctness probe: dispatch must actually fire — a router that routed
    // nothing (everything mis-routed to the dispose lambda) leaves checksum == 0. Caught here, before
    // any timing, rather than expressed as a timing gate.
    {
        auto probe = run_router_mailbox<MailboxCap>(nb_producers, total, dequeue_batch);
        benchmark::DoNotOptimize(probe.checksum);
        if (probe.checksum == 0ull) {
            state.SkipWithError("router dispatched zero messages (no handler fired)");
            return;
        }
    }

    RouterRunResult result{};
    for (auto _ : state) {
        result = run_router_mailbox<MailboxCap>(nb_producers, total, dequeue_batch);
        benchmark::DoNotOptimize(result.checksum);
    }

    const double bytes_reserved =
        static_cast<double>(nb_producers) * static_cast<double>(MailboxCap) * static_cast<double>(sizeof(EventBucket));
    const double bytes_reserved_per_producer = static_cast<double>(MailboxCap) * static_cast<double>(sizeof(EventBucket));

    // Counters assigned once, after the timed loop.
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * total));
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * total * sizeof(EventBucket)));
    state.counters["items_per_s"] = benchmark::Counter(static_cast<double>(total), benchmark::Counter::kIsIterationInvariantRate);
    state.counters["enqueue_failures"] =
        benchmark::Counter(static_cast<double>(result.enqueue_failures), benchmark::Counter::kIsIterationInvariant);
    state.counters["failures_per_item"] = benchmark::Counter(static_cast<double>(result.enqueue_failures) / static_cast<double>(total),
                                                             benchmark::Counter::kIsIterationInvariant);
    // Descriptor counters: plain values, not rates.
    state.counters["checksum"]                    = static_cast<double>(result.checksum);
    state.counters["bytes_reserved"]              = bytes_reserved;
    state.counters["bytes_reserved_per_producer"] = bytes_reserved_per_producer;
}

} // namespace

#define QB_REGISTER_ROUTER_MAILBOX_BENCH(Cap)                        \
    BENCHMARK_TEMPLATE(BM_MpscRouterMailbox_FanIn, Cap)              \
        ->Name("BM_MpscRouterMailbox_FanIn/capacity_" #Cap)          \
        ->Apply(qb::bench::apply_mpsc_fan_in_args)                   \
        ->ArgNames({"producers", "total_messages", "dequeue_batch"}) \
        ->Unit(benchmark::kMillisecond)                              \
        ->UseRealTime()

QB_REGISTER_ROUTER_MAILBOX_BENCH(512);
QB_REGISTER_ROUTER_MAILBOX_BENCH(1023);
QB_REGISTER_ROUTER_MAILBOX_BENCH(1024);
QB_REGISTER_ROUTER_MAILBOX_BENCH(2048);

#undef QB_REGISTER_ROUTER_MAILBOX_BENCH

BENCHMARK_MAIN();
