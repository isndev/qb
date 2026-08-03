/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file shared/scripted_stream_transport.h
 * @brief Programmable in-memory transports for the qb-io stream-template suite (no socket).
 *
 * The `qb::io::{istream,ostream,stream}<_IO_>` templates delegate every byte to an
 * `_IO_` transport object that exposes `int read(char*, size_t)` /
 * `int write(const char*, size_t)` (the exact-return concepts `qb::has_read_r` /
 * `qb::has_write_r`) plus optional `disconnect()` / `close()`. Tests that drive those
 * templates against a real `tcp`/`udp`/`file` transport are inherently flaky: partial
 * writes, short reads and EOF all depend on kernel/socket timing, so the interesting
 * branches (`ErrBufferLimitExceeded`, partial-write reorder, read failure) cannot be
 * provoked deterministically. The helpers below replace the kernel with a *script*:
 *
 *   - `ScriptedStreamTransport` — a fully in-memory `_IO_`. You hand it a string of bytes
 *                                 to hand back from `read()` (chunked exactly as the stream
 *                                 asks, then 0 = EOF), an optional vector of per-call write
 *                                 limits to *force* the partial-write path one call at a
 *                                 time, and `fail_reads` / `fail_writes()` toggles to inject
 *                                 the `-1` error branch. Every byte the stream emits is
 *                                 appended to the public `written` string so a test can
 *                                 assert the exact wire image; `disconnect()` / `close()`
 *                                 record their calls in `disconnected` / `closed` so
 *                                 `stream::close()`'s teardown path is observable.
 *   - `LimitedScriptedOStream`  — a thin `ostream<ScriptedStreamTransport>` subclass that
 *                                 exposes the otherwise-protected `_max_write_buffer_size`,
 *                                 so a test can shrink the buffer cap and prove `publish()`
 *                                 rejects (returns `nullptr`) once the cap would be crossed.
 *   - `NullDevice`              — a `/dev/null` transport: `read()` is always EOF (`0`),
 *                                 `write()` always claims full success, nothing is retained.
 *                                 Exercises the degenerate stream that never blocks.
 *   - `TransformStream<Base>`   — a composition adapter wrapping any istream/ostream-like
 *                                 `Base`: it applies a byte transform to the inbound buffer
 *                                 after `read()` and to outbound data before `publish()`,
 *                                 proving streams chain cleanly. `read`/`write` forward to
 *                                 `Base`; `in()` re-exposes the base buffer for assertions.
 *
 * Hoisted verbatim (single source of truth) from the in-file clones in the former
 * system/test-stream-operations.cpp, so the unit split (unit/stream/stream-templates.cpp)
 * and the framing benchmark (benchmark/transport/async-bases-framing.cpp) share one
 * deterministic, socket-free transport instead of re-declaring it per target.
 *
 * All helpers live in namespace `qb::io::test`. `ScriptedStreamTransport::read`/`write`
 * deliberately return *exactly* `int` (via `static_cast<int>`) and are `noexcept`: that is
 * the precise shape `qb::has_read_r<_IO_, int, char*, size_t>` /
 * `qb::has_write_r<_IO_, int, const char*, size_t>` require to SFINAE-enable the templates —
 * do not relax it.
 */

#ifndef QB_IO_TESTS_SHARED_SCRIPTED_STREAM_TRANSPORT_H
#define QB_IO_TESTS_SHARED_SCRIPTED_STREAM_TRANSPORT_H

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <functional>
#include <gtest/gtest.h>
#include <qb/io/stream.h>
#include <string>
#include <utility>
#include <vector>

namespace qb::io::test {

// ---------------------------------------------------------------------------
// A fully in-memory `_IO_` transport for the qb::io::{i,o,}stream templates.
//
// Construction script:
//   - `read_data`    bytes handed back by read() (chunked to the caller's size,
//                    then 0 = EOF). Empty ⇒ read() returns 0 immediately.
//   - `write_limits` per-write byte caps consumed one entry per write() call —
//                    the deterministic way to force the partial-write/reorder
//                    path. Once the vector is exhausted, write() accepts the
//                    full chunk.
//   - `fail_reads`   when set, read() returns -1 (the read-error branch).
// `fail_writes()` flips the same switch for write() at runtime.
//
// Observability:
//   - `written`      every byte the stream wrote, in order (assert the wire image).
//   - `disconnected` / `closed`  set by disconnect()/close() so the stream's
//                    teardown path (stream::close → disconnect + close) is visible.
// ---------------------------------------------------------------------------
class ScriptedStreamTransport {
    std::string              _read_data;
    std::size_t              _read_offset = 0u;
    std::vector<std::size_t> _write_limits;
    std::size_t              _write_index = 0u;
    bool                     _fail_reads  = false;
    bool                     _fail_writes = false;

public:
    std::string written;              /**< Bytes accepted by write(), in order. */
    bool        disconnected = false; /**< Set true by disconnect(). */
    bool        closed       = false; /**< Set true by close(). */

    ScriptedStreamTransport() = default;

    /**
     * @param read_data    Bytes to deliver via read() (chunked, then EOF).
     * @param write_limits Per-call write caps that force partial writes.
     * @param fail_reads   When true, read() always returns -1.
     */
    explicit ScriptedStreamTransport(std::string read_data, std::vector<std::size_t> write_limits = {}, bool fail_reads = false)
        : _read_data(std::move(read_data))
        , _write_limits(std::move(write_limits))
        , _fail_reads(fail_reads) {}

    /// Toggle the write-error branch (write() returns -1).
    void
    fail_writes(bool enabled = true) noexcept {
        _fail_writes = enabled;
    }

    /// Hand back the next chunk of the script (≤ @p size bytes), 0 at EOF, -1 on injected failure.
    int
    read(char *data, std::size_t size) noexcept {
        if (_fail_reads)
            return -1;
        if (_read_offset >= _read_data.size())
            return 0;

        const auto available = _read_data.size() - _read_offset;
        const auto count     = std::min(size, available);
        std::memcpy(data, _read_data.data() + _read_offset, count);
        _read_offset += count;
        return static_cast<int>(count);
    }

    /// Accept up to the next scripted write-limit bytes (or all of them), -1 on injected failure.
    int
    write(const char *data, std::size_t size) noexcept {
        if (_fail_writes)
            return -1;
        auto count = size;
        if (_write_index < _write_limits.size())
            count = std::min(count, _write_limits[_write_index++]);
        written.append(data, count);
        return static_cast<int>(count);
    }

    /// Record that the stream requested a logical disconnect.
    void
    disconnect() noexcept {
        disconnected = true;
    }

    /// Record that the stream requested a close.
    void
    close() noexcept {
        closed = true;
    }
};

// ---------------------------------------------------------------------------
// `ostream<ScriptedStreamTransport>` that exposes the protected write-buffer
// cap so tests can shrink it and prove publish() rejects (returns nullptr)
// once the cap would be exceeded.
// ---------------------------------------------------------------------------
class LimitedScriptedOStream : public qb::io::ostream<ScriptedStreamTransport> {
public:
    /// Lower (or raise) the maximum output-buffer size enforced by publish().
    void
    set_max_write_buffer_size(std::size_t size) noexcept {
        _max_write_buffer_size = size;
    }
};

// ---------------------------------------------------------------------------
// A `/dev/null` transport: reads are always EOF, writes always "succeed"
// (report the full size) and retain nothing. Drives the degenerate stream
// that neither blocks nor buffers.
// ---------------------------------------------------------------------------
class NullDevice {
public:
    /// Always EOF.
    int
    read(char * /*data*/, std::size_t /*size*/) noexcept {
        return 0;
    }

    /// Pretend the whole chunk was written.
    int
    write(const char * /*data*/, std::size_t size) noexcept {
        return static_cast<int>(size);
    }

    /// Always open.
    [[nodiscard]] bool
    is_open() const noexcept {
        return true;
    }

    /// No-op.
    void
    close() noexcept {}
};

// ---------------------------------------------------------------------------
// Composition adapter: wraps any istream/ostream-like `Base` and applies a
// byte transform to inbound data after read() and to outbound data before
// publish(). Proves streams chain without copying the transport's internals.
//
// `Base` is held by reference — the wrapped stream must outlive the adapter.
// `read`/`write` forward to `Base`; `in()` re-exposes the base input buffer so
// callers can assert the transformed bytes.
// ---------------------------------------------------------------------------
template <typename Base>
class TransformStream {
    Base                                    &_base_stream;
    std::function<void(char *, std::size_t)> _transform_func;

public:
    /**
     * @param base      The wrapped istream/ostream-like stream (must outlive this).
     * @param transform Mutating byte transform applied in-place on each direction.
     */
    TransformStream(Base &base, std::function<void(char *, std::size_t)> transform)
        : _base_stream(base)
        , _transform_func(std::move(transform)) {}

    /// Read through the base stream, transforming the freshly-read buffer in place.
    int
    read() {
        int result = _base_stream.read();
        if (result > 0) {
            char       *data = const_cast<char *>(_base_stream.in().data());
            std::size_t size = _base_stream.in().size();
            _transform_func(data, size);
        }
        return result;
    }

    /// Transform a copy of @p data and publish it to the base stream's output buffer.
    void
    publish(const char *data, std::size_t size) {
        std::vector<char> buffer(data, data + size);
        _transform_func(buffer.data(), buffer.size());
        std::ignore = _base_stream.publish(buffer.data(), buffer.size());
    }

    /// Flush the base stream's output buffer to its transport.
    int
    write() {
        return _base_stream.write();
    }

    /// Expose the base stream's input buffer (for post-transform assertions).
    auto &
    in() {
        return _base_stream.in();
    }
};

} // namespace qb::io::test

#endif // QB_IO_TESTS_SHARED_SCRIPTED_STREAM_TRANSPORT_H
