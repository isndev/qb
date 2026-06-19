/**
 * @file qb/core/tests/benchmark/bm-mpsc-router-mailbox.cpp
 * @brief Pseudo-real mailbox workload: MPSC EventBucket ingress + router::memh dispatch
 *
 * Models a single consumer core draining qb::lockfree::mpsc::ringbuffer<EventBucket, N, 0>
 * (same shape as engine mailboxes) and routing each message through qb::router::memh<BenchEvt>.
 *
 * This benchmark is intended to approximate a framework-like hot path:
 *   - multiple producers
 *   - per-producer dedicated ingress slices
 *   - one consumer draining by batch
 *   - router dispatch to typed handlers
 *
 * Notes:
 *   - Message handlers are intentionally trivial.
 *   - A consumer-local checksum accumulator is used to keep handler work observable
 *     without paying an atomic operation per message in the hot path.
 *   - Ring capacities are limited to >= 512 here (512, 1023, 1024, 2048).
 *
 * Main tuning metrics:
 *   - items_per_s
 *   - enqueue_failures
 *   - failures_per_item
 *
 * Memory-related counters are also published so throughput can be interpreted against
 * mailbox footprint.
 *
 * Important:
 *   - checksum / bytes_reserved / bytes_reserved_per_producer are published as plain values,
 *     not benchmark::Counter rates, because they are configuration/result descriptors.
 */

/*
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#include <algorithm>
#include <atomic>
#include <barrier>
#include <benchmark/benchmark.h>
#include <cstdint>
#include <cstring>
#include <memory>
#include <thread>
#include <type_traits>
#include <typeinfo>
#include <vector>

#include <qb/core/ActorId.h>
#include <qb/core/Event.h>
#include <qb/system/event/router.h>
#include <qb/system/lockfree/mpsc.h>
#include <qb/utility/compat.h>
#include <qb/utility/prefix.h>

#include "../shared/BenchmarkIterationSink.h"

namespace {

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

template <typename Msg>
void
write_bucket(EventBucket &out, qb::ActorId const dst, std::uint32_t const salt) noexcept {
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

    std::memcpy(&out, &m, sizeof(Msg));
}

[[nodiscard]] constexpr std::uint64_t
quota_for(std::size_t const producer_index, std::uint64_t const total, std::size_t const nb_producers) noexcept {
    const auto p    = static_cast<std::uint64_t>(nb_producers);
    const auto base = total / p;
    const auto rem  = total % p;
    return base + (static_cast<std::uint64_t>(producer_index) < rem ? 1ull : 0ull);
}

template <std::size_t MailboxCap>
struct RouterMailboxRunResult {
    std::uint64_t enqueue_failures = 0;
    std::uint64_t checksum         = 0;
};

template <std::size_t MailboxCap>
[[nodiscard]] RouterMailboxRunResult<MailboxCap>
run_router_mailbox(std::size_t const nb_producers, std::uint64_t const total, std::size_t const dequeue_batch) {
    RouterMailboxRunResult<MailboxCap> result{};

    qb::lockfree::mpsc::ringbuffer<EventBucket, MailboxCap, 0> queue(nb_producers);

    const std::size_t batch_cap = std::max<std::size_t>(1u, dequeue_batch);
    auto              batch_buf = std::make_unique<EventBucket[]>(batch_cap);

    std::atomic<std::uint64_t> fail_sum{0};
    std::barrier               start_gate(static_cast<std::ptrdiff_t>(nb_producers + 1u));

    std::vector<qb::jthread> producers;
    producers.reserve(nb_producers);

    for (std::size_t pid = 0; pid < nb_producers; ++pid) {
        const std::uint64_t q = quota_for(pid, total, nb_producers);

        producers.emplace_back([&, pid, q] {
            start_gate.arrive_and_wait();

            std::uint64_t local_fails = 0;
            std::uint64_t sent        = 0;

            while (sent < q) {
                EventBucket bucket{};
                const auto  salt = static_cast<std::uint32_t>(sent + pid * 997u);

                switch ((sent + pid) % 3u) {
                    case 0u:
                        write_bucket<MsgOrder>(bucket, qb::ActorId{kBenchDstOrder}, salt);
                        break;
                    case 1u:
                        write_bucket<MsgPing>(bucket, qb::ActorId{kBenchDstPing}, salt);
                        break;
                    default:
                        write_bucket<MsgNotify>(bucket, qb::ActorId{kBenchDstNote}, salt);
                        break;
                }

                if (queue.enqueue(pid, bucket)) {
                    ++sent;
                } else {
                    ++local_fails;
                }
            }

            fail_sum.fetch_add(local_fails, std::memory_order_relaxed);
        });
    }

    std::uint64_t consumer_checksum = 0;

    qb::jthread consumer([&] {
        start_gate.arrive_and_wait();

        LocalChecksum local_checksum{};

        qb::router::memh<BenchEvt> router;
        HandlerOrder               h_order(kBenchDstOrder, local_checksum);
        HandlerPing                h_ping(kBenchDstPing, local_checksum);
        HandlerNotify              h_note(kBenchDstNote, local_checksum);

        router.subscribe<MsgOrder>(h_order);
        router.subscribe<MsgPing>(h_ping);
        router.subscribe<MsgNotify>(h_note);

        std::uint64_t popped = 0;
        while (popped < total) {
            const std::size_t n = queue.dequeue(
                [&router](EventBucket *buffer, std::size_t const nb_buckets) noexcept {
                    for (std::size_t i = 0; i < nb_buckets; ++i) {
                        auto &base = *reinterpret_cast<BenchEvt *>(buffer + i);
                        router.route(base, [](BenchEvt &) noexcept {
                            // Mis-routed message: should never happen in this benchmark.
                        });
                    }
                },
                batch_buf.get(), batch_cap);

            popped += static_cast<std::uint64_t>(n);
        }

        consumer_checksum = local_checksum.value;
    });

    producers.clear(); // join all producers
    consumer.join();

    result.enqueue_failures = fail_sum.load(std::memory_order_relaxed);
    result.checksum         = consumer_checksum;
    return result;
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

    const double bytes_reserved =
        static_cast<double>(nb_producers) * static_cast<double>(MailboxCap) * static_cast<double>(sizeof(EventBucket));

    const double bytes_reserved_per_producer = static_cast<double>(MailboxCap) * static_cast<double>(sizeof(EventBucket));

    for (auto _ : state) {
        const auto result = run_router_mailbox<MailboxCap>(nb_producers, total, dequeue_batch);

        state.counters["items_per_s"] = benchmark::Counter(static_cast<double>(total), benchmark::Counter::kIsIterationInvariantRate);

        state.counters["enqueue_failures"] =
            benchmark::Counter(static_cast<double>(result.enqueue_failures), benchmark::Counter::kIsIterationInvariant);

        state.counters["failures_per_item"] = benchmark::Counter(static_cast<double>(result.enqueue_failures) / static_cast<double>(total),
                                                                 benchmark::Counter::kIsIterationInvariant);

        // Descriptor counters: publish as plain values, not benchmark::Counter(...)
        state.counters["checksum"]                    = static_cast<double>(result.checksum);
        state.counters["bytes_reserved"]              = bytes_reserved;
        state.counters["bytes_reserved_per_producer"] = bytes_reserved_per_producer;
    }
}

void
apply_router_mailbox_args(benchmark::internal::Benchmark *b) {
    const auto         cap       = qb::bench::cappedBenchmarkCores();
    const std::int64_t totals[]  = {200'000, 1'000'000};
    const std::int64_t batches[] = {1, 16, 64};

    for (std::int64_t total : totals) {
        for (std::uint32_t p = 1u; p <= std::min<std::uint32_t>(8u, cap); p *= 2u) {
            for (std::int64_t batch : batches) {
                b->Args({static_cast<std::int64_t>(p), total, batch});
            }
        }
    }
}

} // namespace

#define QB_REGISTER_ROUTER_MAILBOX_BENCH(Cap)                        \
    BENCHMARK_TEMPLATE(BM_MpscRouterMailbox_FanIn, Cap)              \
        ->Name("BM_MpscRouterMailbox_FanIn/capacity_" #Cap)          \
        ->Apply(apply_router_mailbox_args)                           \
        ->ArgNames({"producers", "total_messages", "dequeue_batch"}) \
        ->Unit(benchmark::kMillisecond)                              \
        ->UseRealTime()

QB_REGISTER_ROUTER_MAILBOX_BENCH(512);
QB_REGISTER_ROUTER_MAILBOX_BENCH(1023);
QB_REGISTER_ROUTER_MAILBOX_BENCH(1024);
QB_REGISTER_ROUTER_MAILBOX_BENCH(2048);

#undef QB_REGISTER_ROUTER_MAILBOX_BENCH

BENCHMARK_MAIN();
