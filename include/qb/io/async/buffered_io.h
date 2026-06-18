/**
 * @file qb/io/async/buffered_io.h
 * @brief Protocol/buffer core for logical asynchronous I/O streams.
 */

#ifndef QB_IO_ASYNC_BUFFERED_IO_H_
#define QB_IO_ASYNC_BUFFERED_IO_H_

#include <cstddef>
#include <memory>
#include <type_traits>
#include <vector>
#include <qb/io/config.h>
#include <qb/io/async/event/all.h>
#include <qb/io/async/protocol.h>
#include <qb/utility/branch_hints.h>
#include <qb/utility/type_traits.h>

namespace qb::io::async {

/**
 * Shared protocol/buffer machinery for I/O surfaces that are not directly backed
 * by one kernel watcher, such as a QUIC stream multiplexed over an UDP endpoint.
 *
 * The derived type owns the actual input/output pipes and must expose:
 *   - in(), out()
 *   - pendingRead(), pendingWrite()
 *   - flush(std::size_t)
 *
 * This intentionally mirrors the protocol contract of async::io: protocols are
 * owned here, can be switched at runtime, and always parse from Derived::in().
 */
template <typename _Derived>
class buffered_io {
    IProtocol *_protocol = nullptr;
    std::vector<std::unique_ptr<IProtocol>> _protocol_list;
    bool _on_message = false;
    bool _is_disposed = false;
    int _reason = 0;
    int _system_error = 0;
    std::size_t _max_message_size = QB_MAX_MESSAGE_SIZE;
    std::size_t _bytes_read = 0;
    std::size_t _bytes_written = 0;
    std::size_t _messages_processed = 0;

    [[nodiscard]] _Derived &derived() noexcept {
        return static_cast<_Derived &>(*this);
    }

    [[nodiscard]] _Derived const &derived() const noexcept {
        return static_cast<_Derived const &>(*this);
    }

public:
    using base_io_t = buffered_io<_Derived>;

    buffered_io() = default;
    buffered_io(buffered_io const &) = delete;
    buffered_io &operator=(buffered_io const &) = delete;
    ~buffered_io() noexcept = default;

    template <typename _Protocol, typename... _Args>
    _Protocol *switch_protocol(_Args &&...args) {
        auto up = std::make_unique<_Protocol>(std::forward<_Args>(args)...);
        if (up->ok()) {
            auto *raw = up.get();
            _protocol_list.push_back(std::move(up));
            _protocol = raw;
            return raw;
        }
        return nullptr;
    }

    void clear_protocols() {
        _protocol_list.clear();
        _protocol_list.shrink_to_fit();
        _protocol = nullptr;
    }

    [[nodiscard]] IProtocol *protocol() noexcept {
        return _protocol;
    }

    [[nodiscard]] IProtocol const *protocol() const noexcept {
        return _protocol;
    }

    [[nodiscard]] bool is_connected() const noexcept {
        return !_is_disposed && _reason == 0;
    }

    [[nodiscard]] bool has_pending_read() const noexcept {
        return _protocol && _protocol->ok() && derived().pendingRead() > 0;
    }

    [[nodiscard]] bool has_pending_write() const noexcept {
        return derived().pendingWrite() > 0;
    }

    [[nodiscard]] std::size_t max_message_size() const noexcept {
        return _max_message_size;
    }

    void set_max_message_size(std::size_t size) noexcept {
        _max_message_size = size;
    }

    [[nodiscard]] int disconnection_reason() const noexcept {
        return _reason;
    }

    [[nodiscard]] int system_error() const noexcept {
        return _system_error;
    }

    [[nodiscard]] std::size_t bytes_read() const noexcept {
        return _bytes_read;
    }

    [[nodiscard]] std::size_t bytes_written() const noexcept {
        return _bytes_written;
    }

    [[nodiscard]] std::size_t messages_processed() const noexcept {
        return _messages_processed;
    }

    void close_after_deliver() noexcept {
        if (_protocol)
            _protocol->not_ok();
    }

    template <typename... _Args>
    auto &publish(_Args &&...args) {
        auto &d = derived();
        if (unlikely(_is_disposed || _reason))
            return d.out();

        const auto max_write = d.max_write_buffer_size();
        if (unlikely(max_write != static_cast<std::size_t>(-1) &&
                     d.pendingWrite() >= max_write)) {
            _system_error = 0;
            disconnect(event::disconnect_reason::buffer_overflow);
            return d.out();
        }

        if constexpr (sizeof...(_Args)) {
            const auto before = d.pendingWrite();
            (d.out() << ... << std::forward<_Args>(args));
            const auto after = d.pendingWrite();
            if (unlikely(max_write != static_cast<std::size_t>(-1) &&
                         after > max_write)) {
                const auto added = after - before;
                const auto overflow = after - max_write;
                if constexpr (requires { d.out().free_back(std::size_t{}); }) {
                    if (overflow <= added)
                        d.out().free_back(overflow);
                }
                _system_error = 0;
                disconnect(event::disconnect_reason::buffer_overflow);
            }
        }
        return d.out();
    }

    template <typename T>
    auto &operator<<(T &&data) {
        return publish(std::forward<T>(data));
    }

    void disconnect(int reason = 1) noexcept {
        _reason = reason ? reason : static_cast<int>(event::disconnect_reason::user_initiated);
    }

    void disconnect(event::disconnect_reason reason) noexcept {
        disconnect(static_cast<int>(reason));
    }

    void account_read(std::size_t bytes) noexcept {
        _bytes_read += bytes;
    }

    void account_written(std::size_t bytes) noexcept {
        _bytes_written += bytes;
    }

    [[nodiscard]] bool process_input() noexcept {
        if (unlikely(_is_disposed || _reason))
            return false;
        if (!_protocol) {
            if constexpr (qb::has_on<_Derived, event::pending_read>) {
                const auto pending = derived().pendingRead();
                if (pending) {
                    auto evt = event::pending_read{pending};
                    derived().on(std::move(evt));
                }
            }
            return true;
        }
        if (!_protocol->ok())
            return false;
        if (_on_message)
            return true;

        std::shared_ptr<void> self_guard;
        if constexpr (qb::has_shared_from_this<_Derived>) {
            try { self_guard = derived().shared_from_this(); } catch (std::bad_weak_ptr const&) {}
        } else if constexpr (requires { derived().shared(); }) {
            self_guard = derived().shared();
        }

        _on_message = true;
        std::size_t ret = 0;
        while ((ret = _protocol->getMessageSize()) > 0) {
            const auto frame_exceeds_pending = [&]() noexcept {
                if (!_protocol->should_flush())
                    return false;
                if constexpr (requires { derived().pendingRead(); })
                    return ret > derived().pendingRead();
                else
                    return false;
            }();
            if (unlikely(ret > _max_message_size || frame_exceeds_pending)) {
                _protocol->not_ok();
                _system_error = 0;
                _reason = -2;
                _on_message = false;
                return false;
            }

            // Snapshot the protocol pointer AND its should_flush() before onMessage():
            // onMessage() may switch_protocol() or clear_protocols(), either of which can
            // leave `protocol` dangling. should_flush() is a per-protocol invariant (no
            // setter is ever called), so capturing the OLD protocol's value now is
            // equivalent and removes every post-onMessage deref of `protocol`.
            auto *protocol = _protocol;
            const bool old_should_flush = protocol->should_flush();
            protocol->onMessage(ret);
            ++_messages_processed;

            if (unlikely(_reason)) {
                if (likely(old_should_flush))
                    derived().flush(ret);
                _on_message = false;
                return derived().pendingWrite() > 0;
            }

            if (unlikely(!_protocol || !_protocol->ok())) {
                if (likely(old_should_flush))
                    derived().flush(ret);
                if (derived().pendingWrite() > 0) {
                    _on_message = false;
                    return true;
                }
                _system_error = 0;
                _reason = -1;
                _on_message = false;
                return false;
            }

            if (likely(old_should_flush))
                derived().flush(ret);
        }
        _on_message = false;

        if constexpr (qb::has_on<_Derived, event::pending_read> ||
                      qb::has_on<_Derived, event::eof>) {
            const auto pending = derived().pendingRead();
            if (pending) {
                if constexpr (qb::has_on<_Derived, event::pending_read>) {
                    auto evt = event::pending_read{pending};
                    derived().on(std::move(evt));
                }
            } else {
                if constexpr (qb::has_on<_Derived, event::eof>) {
                    auto evt = event::eof{};
                    derived().on(std::move(evt));
                }
            }
        }
        return _protocol && _protocol->ok();
    }

    void dispose() {
        if (_is_disposed)
            return;
        _is_disposed = true;

        if constexpr (qb::has_on<_Derived, event::disconnected>) {
            if (_system_error != 0) {
                auto evt = event::disconnected::with_error(_reason, _system_error);
                derived().on(std::move(evt));
            } else {
                auto evt = event::disconnected{_reason};
                derived().on(std::move(evt));
            }
        }

        if constexpr (requires { _Derived::has_server; }) {
            if constexpr (_Derived::has_server) {
                if constexpr (requires { derived().connection_id(); })
                    derived().server().disconnected(derived().connection_id(), derived().id());
                else
                    derived().server().disconnected(derived().id());
            } else if constexpr (qb::has_on<_Derived, event::dispose>) {
                auto evt = event::dispose{};
                derived().on(std::move(evt));
            }
        } else if constexpr (qb::has_on<_Derived, event::dispose>) {
            auto evt = event::dispose{};
            derived().on(std::move(evt));
        }
    }

protected:
    void reset_buffered_io_state() noexcept {
        _on_message = false;
        _is_disposed = false;
        _reason = 0;
        _system_error = 0;
    }
};

} // namespace qb::io::async

#endif // QB_IO_ASYNC_BUFFERED_IO_H_
