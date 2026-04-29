/**
 * @file qb/io/async/quic/io_handler.h
 * @brief Logical QUIC stream-session registry.
 */

#ifndef QB_IO_ASYNC_QUIC_IO_HANDLER_H_
#define QB_IO_ASYNC_QUIC_IO_HANDLER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>
#include <qb/io/async/event/all.h>
#include <qb/system/container/unordered_map.h>
#include <qb/utility/type_traits.h>
#include "events.h"

namespace qb::io::async::quic {

struct stream_key {
    std::uint64_t connection_id = 0;
    std::uint64_t stream_id = 0;

    [[nodiscard]] friend bool
    operator==(stream_key const&, stream_key const&) noexcept = default;
};

struct stream_key_hash {
    [[nodiscard]] std::size_t operator()(stream_key const& key) const noexcept {
        auto x = key.connection_id + 0x9e3779b97f4a7c15ull;
        x ^= key.stream_id + 0x9e3779b97f4a7c15ull + (x << 6u) + (x >> 2u);
        return static_cast<std::size_t>(x);
    }
};

/**
 * QUIC stream sessions are logical sessions, not movable kernel transports.
 *
 * A TCP session owns one socket and can be extracted/moved to another
 * io_handler. A QUIC connection owns one UDP fd, shared congestion state,
 * ACK/loss recovery, TLS state, timers and connection IDs for all streams;
 * moving a single stream to another VirtualCore would split that state.
 *
 * In qb-core deployments, keep this handler on the VirtualCore that owns
 * the QUIC endpoint. Delegate CPU-heavy or domain work to other actors via
 * normal QB events containing connection/stream IDs and payloads, then send
 * responses back to the owner actor to write on the stream.
 */
template <typename Derived, typename StreamSession>
class io_handler {
public:
    using IOSession = StreamSession;
    using session_map_t = qb::unordered_map<stream_key, std::shared_ptr<StreamSession>,
                                            stream_key_hash>;

private:
    session_map_t _sessions;
    std::size_t _max_stream_sessions = 0;

public:
    io_handler() = default;
    io_handler(io_handler const&) = delete;
    io_handler& operator=(io_handler const&) = delete;
    io_handler(io_handler&&) = delete;
    io_handler& operator=(io_handler&&) = delete;
    ~io_handler() = default;

    [[nodiscard]] session_map_t& sessions() noexcept { return _sessions; }
    [[nodiscard]] session_map_t const& sessions() const noexcept { return _sessions; }
    [[nodiscard]] std::size_t session_count() const noexcept { return _sessions.size(); }
    [[nodiscard]] std::size_t max_sessions() const noexcept { return _max_stream_sessions; }

    void set_max_sessions(std::size_t max) noexcept {
        _max_stream_sessions = max;
    }

    [[nodiscard]] StreamSession *session(std::uint64_t stream_id) noexcept {
        if (auto *exact = session(0, stream_id))
            return exact;
        StreamSession *match = nullptr;
        for (auto& entry : _sessions) {
            if (entry.first.stream_id != stream_id)
                continue;
            if (match)
                return nullptr;
            match = entry.second.get();
        }
        return match;
    }

    [[nodiscard]] StreamSession *session(std::uint64_t connection_id,
                                         std::uint64_t stream_id) noexcept {
        auto it = _sessions.find({connection_id, stream_id});
        return it == _sessions.end() ? nullptr : it->second.get();
    }

    [[nodiscard]] StreamSession const *session(std::uint64_t stream_id) const noexcept {
        if (auto const *exact = session(0, stream_id))
            return exact;
        StreamSession const *match = nullptr;
        for (auto const& entry : _sessions) {
            if (entry.first.stream_id != stream_id)
                continue;
            if (match)
                return nullptr;
            match = entry.second.get();
        }
        return match;
    }

    [[nodiscard]] StreamSession const *session(std::uint64_t connection_id,
                                               std::uint64_t stream_id) const noexcept {
        auto it = _sessions.find({connection_id, stream_id});
        return it == _sessions.end() ? nullptr : it->second.get();
    }

    [[nodiscard]] std::shared_ptr<StreamSession> session_handle(std::uint64_t stream_id) noexcept {
        if (auto exact = session_handle(0, stream_id))
            return exact;
        std::shared_ptr<StreamSession> match;
        for (auto& entry : _sessions) {
            if (entry.first.stream_id != stream_id)
                continue;
            if (match)
                return nullptr;
            match = entry.second;
        }
        return match;
    }

    [[nodiscard]] std::shared_ptr<StreamSession> session_handle(
        std::uint64_t connection_id, std::uint64_t stream_id) noexcept {
        auto it = _sessions.find({connection_id, stream_id});
        return it == _sessions.end() ? nullptr : it->second;
    }

    [[nodiscard]] std::shared_ptr<const StreamSession> session_handle(std::uint64_t stream_id) const noexcept {
        if (auto exact = session_handle(0, stream_id))
            return exact;
        std::shared_ptr<const StreamSession> match;
        for (auto const& entry : _sessions) {
            if (entry.first.stream_id != stream_id)
                continue;
            if (match)
                return nullptr;
            match = entry.second;
        }
        return match;
    }

    [[nodiscard]] std::shared_ptr<const StreamSession> session_handle(
        std::uint64_t connection_id, std::uint64_t stream_id) const noexcept {
        auto it = _sessions.find({connection_id, stream_id});
        return it == _sessions.end() ? nullptr : it->second;
    }

    [[nodiscard]] StreamSession *registerSession(std::uint64_t stream_id) {
        return registerSession(0, stream_id);
    }

    [[nodiscard]] StreamSession *registerSession(std::uint64_t connection_id,
                                                 std::uint64_t stream_id) {
        if (_max_stream_sessions > 0 && _sessions.size() >= _max_stream_sessions)
            return nullptr;

        const stream_key key{connection_id, stream_id};
        if (auto it = _sessions.find(key); it != _sessions.end())
            return it->second.get();

        std::shared_ptr<StreamSession> session;
        if constexpr (std::is_constructible_v<StreamSession, Derived&, std::uint64_t>)
            session = std::make_shared<StreamSession>(static_cast<Derived&>(*this), stream_id);
        else if constexpr (std::is_constructible_v<StreamSession, Derived&>) {
            session = std::make_shared<StreamSession>(static_cast<Derived&>(*this));
            assign_stream_id(*session, stream_id);
        } else if constexpr (std::is_constructible_v<StreamSession, std::uint64_t>)
            session = std::make_shared<StreamSession>(stream_id);
        else {
            session = std::make_shared<StreamSession>();
            assign_stream_id(*session, stream_id);
        }

        assign_quic_ids(*session, connection_id, stream_id);

        auto [it, inserted] = _sessions.emplace(key, session);
        if (!inserted)
            return it->second.get();

        if constexpr (qb::has_on<Derived, StreamSession&>)
            static_cast<Derived&>(*this).on(*session);
        return session.get();
    }

    [[nodiscard]] StreamSession *register_stream_session(std::uint64_t stream_id) {
        return registerSession(stream_id);
    }

    [[nodiscard]] StreamSession *register_stream_session(std::uint64_t connection_id,
                                                         std::uint64_t stream_id) {
        return registerSession(connection_id, stream_id);
    }

    void unregisterSession(std::uint64_t stream_id) {
        unregisterSession(0, stream_id);
    }

    void unregisterSession(std::uint64_t connection_id, std::uint64_t stream_id) {
        _sessions.erase({connection_id, stream_id});
    }

    void disconnected(std::uint64_t stream_id) {
        unregisterSession(stream_id);
    }

    void disconnected(std::uint64_t connection_id, std::uint64_t stream_id) {
        unregisterSession(connection_id, stream_id);
    }

    void clearSessions() noexcept {
        _sessions.clear();
    }

    void clearSessions(std::uint64_t connection_id) noexcept {
        for (auto it = _sessions.begin(); it != _sessions.end();) {
            if (it->first.connection_id == connection_id)
                it = _sessions.erase(it);
            else
                ++it;
        }
    }

protected:
    [[nodiscard]] StreamSession *ensure_stream_session(std::uint64_t stream_id) {
        return registerSession(0, stream_id);
    }

    [[nodiscard]] StreamSession *ensure_stream_session(std::uint64_t connection_id,
                                                       std::uint64_t stream_id) {
        return registerSession(connection_id, stream_id);
    }

    bool feed_stream_data(event::stream_data const& ev) {
        return feed_stream_data(ev, [](std::uint64_t, std::uint64_t, std::uint64_t) {});
    }

    template <typename Credit>
    bool feed_stream_data(event::stream_data const& ev, Credit&& credit) {
        if (!ensure_stream_session(ev.connection_id, ev.id))
            return false;
        auto keep_alive = session_handle(ev.connection_id, ev.id);
        auto *session = keep_alive.get();
        if constexpr (requires(StreamSession& s, std::string_view data) { s.append(data); s.process(); }) {
            const auto before = session->pendingRead();
            if (!session->append(ev.payload))
                return false;
            const auto available = before + ev.payload.size();
            const auto ok = session->process();
            const auto after = session->pendingRead();
            if (available >= after) {
                const auto consumed = available - after;
                if (consumed > 0)
                    credit(ev.connection_id, ev.id, consumed);
            }
            if (!ok && session->pendingWrite() == 0)
                session->dispose();
            return ok || session->pendingWrite() > 0;
        }
        return true;
    }

    template <typename Send>
    void drain_stream_output(StreamSession& session, Send&& send) {
        if constexpr (requires(StreamSession& s) { s.pendingWrite(); s.out(); s.id(); }) {
            while (session.pendingWrite() > 0) {
                auto& out = session.out();
                const auto size = out.size();
                send(session.connection_id(), session.id(),
                     std::span<const std::byte>(
                         reinterpret_cast<const std::byte *>(out.begin()), size),
                     false);
                if constexpr (requires(StreamSession& s) { s.account_written(size); })
                    session.account_written(size);
                out.free_front(size);
                if (out.empty())
                    out.reset();
                else
                    out.reorder();
            }
            if constexpr (qb::has_on<StreamSession, qb::io::async::event::eos>) {
                auto evt = qb::io::async::event::eos{};
                session.on(std::move(evt));
            }
        }
    }

private:
    static void assign_stream_id(StreamSession& session, std::uint64_t stream_id) {
        if constexpr (requires(StreamSession& s) { s.assign_stream_id(stream_id); })
            session.assign_stream_id(stream_id);
    }

    static void assign_quic_ids(StreamSession& session, std::uint64_t connection_id,
                                std::uint64_t stream_id) {
        if constexpr (requires(StreamSession& s) { s.assign_quic_ids(connection_id, stream_id); })
            session.assign_quic_ids(connection_id, stream_id);
        else {
            assign_stream_id(session, stream_id);
            if constexpr (requires(StreamSession& s) { s.assign_connection_id(connection_id); })
                session.assign_connection_id(connection_id);
        }
    }
};

} // namespace qb::io::async::quic

#endif // QB_IO_ASYNC_QUIC_IO_HANDLER_H_
