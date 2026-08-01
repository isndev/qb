/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/protocol/protocol-base-framing.cpp
 * @brief `qb::protocol::base` framing primitives — byte/bytes-terminated + size-as-header.
 *
 * The framing primitives in qb/io/protocol/base.h are the building blocks every higher protocol
 * (text, json, http) composes: `byte_terminated<_IO_,Byte>` frames on a single delimiter,
 * `bytes_terminated<_IO_,Trait>` on a delimiter sequence, and `size_as_header<_IO_,Size>` on a
 * fixed-width length prefix (with network-byte-order conversion for 2- and 4-byte widths). They are
 * driven here against a trivial in-memory probe whose `in()` is a plain pipe — NO socket, NO event
 * loop — so it is a strict `unit` test of the parsing arithmetic.
 *
 * Split from system/test-buffered-io.cpp::ProtocolBase.* (spec §2). Strengthened per the spec:
 *   - the size-as-header coverage spans the 1/2/4-byte widths AND adds an 8-byte (`uint64_t`)
 *     header (which takes base.h's raw, non-byteswapped path);
 *   - at-boundary / one-over framing is pinned: a header arriving exactly across a chunk split, a
 *     payload that is one byte short (no frame) then exactly complete (one frame).
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include <qb/io/async/buffered_io.h>
#include <qb/io/protocol/base.h>

namespace {

// Minimal `_IO_` for the framing protocols: an input pipe + the base_io_t typedef the AProtocol
// layer keys on. (buffered_io supplies base_io_t but not in(); a probe provides its own in().)
class ProtocolProbe {
    qb::allocator::pipe<char> _in;

public:
    using base_io_t = qb::io::async::buffered_io<ProtocolProbe>;

    qb::allocator::pipe<char> &
    in() noexcept {
        return _in;
    }

    void
    append(std::string_view data) {
        _in << data;
    }
};

// 4-byte sequence delimiter trait (HTTP header terminator).
struct DoubleCrLf {
    static constexpr char _EndBytes[] = "\r\n\r\n";
};
constexpr char DoubleCrLf::_EndBytes[];

class LineTerminatedProtocol : public qb::protocol::base::byte_terminated<ProtocolProbe, '\n'> {
public:
    using byte_terminated::byte_terminated;
    void
    onMessage(std::size_t) noexcept final {}
};

class HeaderTerminatedProtocol : public qb::protocol::base::bytes_terminated<ProtocolProbe, DoubleCrLf> {
public:
    using bytes_terminated::bytes_terminated;
    void
    onMessage(std::size_t) noexcept final {}
};

template <typename Size>
class SizeHeaderProtocol : public qb::protocol::base::size_as_header<ProtocolProbe, Size> {
public:
    using qb::protocol::base::size_as_header<ProtocolProbe, Size>::size_as_header;
    void
    onMessage(std::size_t) noexcept final {}
};

// Append a header value (in its on-the-wire byte form) to a probe.
template <typename Header>
void
append_header(ProtocolProbe &probe, const Header &header) {
    probe.append(std::string_view{reinterpret_cast<const char *>(&header), sizeof(header)});
}

} // namespace

// =============================================================================
// byte_terminated / bytes_terminated — delimiter scan, offset resume, shiftSize
// =============================================================================

/**
 * @test The single-byte and sequence terminators locate a frame across partial appends, resume the
 *       delimiter scan from the saved offset, and `shiftSize` strips the delimiter.
 * @brief Folded from ProtocolBase.ByteAndSequenceTerminatorsTrackOffsetsAndReset.
 */
TEST(ProtocolBaseFraming, ByteAndSequenceTerminatorsTrackOffsetsAndReset) {
    ProtocolProbe          probe;
    LineTerminatedProtocol line_protocol{probe};

    probe.append("partial");
    EXPECT_EQ(line_protocol.getMessageSize(), 0u) << "no '\\n' yet";
    probe.append("-line\nnext");
    EXPECT_EQ(line_protocol.getMessageSize(), 13u) << "frame ends at the '\\n' (inclusive of delimiter)";
    EXPECT_EQ(line_protocol.shiftSize(13u), 12u) << "shiftSize strips the 1-byte delimiter";
    EXPECT_EQ(line_protocol.shiftSize(0u), 0u);

    line_protocol.reset();
    probe.in().reset();
    probe.append("abc\r\n\r\ntrailer");

    HeaderTerminatedProtocol header_protocol{probe};
    EXPECT_EQ(header_protocol.getMessageSize(), 7u) << "frame ends after the 4-byte CRLFCRLF";
    EXPECT_EQ(header_protocol.shiftSize(7u), 3u) << "shiftSize strips the 4-byte delimiter";
    EXPECT_EQ(header_protocol.shiftSize(2u), 0u);

    header_protocol.reset();
    probe.in().reset();
    probe.append("abc");
    EXPECT_EQ(header_protocol.getMessageSize(), 0u);
    probe.append("defgh");
    EXPECT_EQ(header_protocol.getMessageSize(), 0u) << "still no full delimiter sequence";
}

// =============================================================================
// size_as_header — zero-size rejection + Header() capacity guard
// =============================================================================

/**
 * @test A zero-size header marks the protocol not-ok; `Header()` rejects payloads that overflow the
 *       width and round-trips a 4-byte header through network byte order.
 * @brief Folded from ProtocolBase.SizeHeaderRejectsZeroAndChecksHeaderCapacity.
 */
TEST(ProtocolBaseFraming, SizeHeaderRejectsZeroAndChecksHeaderCapacity) {
    ProtocolProbe                     probe;
    SizeHeaderProtocol<std::uint16_t> protocol{probe};

    const std::uint16_t zero = 0;
    append_header(probe, zero);
    EXPECT_EQ(protocol.getMessageSize(), 0u);
    EXPECT_FALSE(protocol.ok()) << "a zero-length frame is a protocol error";

    // Header() throws when the payload exceeds the header width.
    auto make_too_large_header = [] {
        return SizeHeaderProtocol<std::uint8_t>::Header(256u);
    };
    EXPECT_THROW({ [[maybe_unused]] auto &&discarded_ = make_too_large_header(); }, std::runtime_error);

    // A 1-byte header is the raw value.
    EXPECT_EQ(SizeHeaderProtocol<std::uint8_t>::Header(7u), 7u);

    // A 4-byte header is htonl'd; ntohl recovers it.
    const auto header = SizeHeaderProtocol<std::uint32_t>::Header(7u);
    EXPECT_EQ(ntohl(header), 7u);

    protocol.reset();
}

// =============================================================================
// size_as_header — 1/2/4-byte widths across partial frames (at-boundary)
// =============================================================================

/**
 * @test A 1-byte and a 4-byte header both frame correctly when the header and payload arrive in
 *       separate chunks, including the header split mid-way.
 * @brief Folded from ProtocolBase.SizeHeaderHandlesOneAndFourByteHeadersAcrossPartialFrames.
 */
TEST(ProtocolBaseFraming, SizeHeaderHandlesOneAndFourByteWidthsAcrossPartialFrames) {
    // 1-byte header announcing a 3-byte payload.
    {
        ProtocolProbe                    probe;
        SizeHeaderProtocol<std::uint8_t> protocol{probe};
        const auto                       header = SizeHeaderProtocol<std::uint8_t>::Header(3u);

        append_header(probe, header);
        EXPECT_EQ(protocol.shiftSize(), sizeof(header)) << "shiftSize() reports the header width";
        EXPECT_EQ(protocol.getMessageSize(), 0u) << "header consumed, payload not yet present";
        EXPECT_EQ(probe.in().size(), 0u) << "the header byte was freed from the front";

        probe.append("ab");
        EXPECT_EQ(protocol.getMessageSize(), 0u) << "2 of 3 payload bytes — no frame";
        probe.append("c");
        EXPECT_EQ(protocol.getMessageSize(), 3u) << "exactly 3 payload bytes — one frame";

        protocol.reset();
    }

    // 4-byte header arriving split across two appends.
    {
        ProtocolProbe                     probe;
        SizeHeaderProtocol<std::uint32_t> protocol{probe};
        const auto                        header = SizeHeaderProtocol<std::uint32_t>::Header(4u);
        const std::string_view            header_view{reinterpret_cast<char const *>(&header), sizeof(header)};

        probe.append(header_view.substr(0u, 2u)); // first half of the header
        EXPECT_EQ(protocol.getMessageSize(), 0u) << "header incomplete";
        probe.append(header_view.substr(2u)); // second half
        EXPECT_EQ(protocol.getMessageSize(), 0u) << "header complete, payload absent";
        EXPECT_EQ(probe.in().size(), 0u);

        probe.append("data");
        EXPECT_EQ(protocol.getMessageSize(), 4u);
        EXPECT_TRUE(protocol.ok());
    }
}

// =============================================================================
// size_as_header — 2-byte + 8-byte widths, at-boundary / one-over (spec strengthening)
// =============================================================================

/**
 * @test A 2-byte (network-order) header frames at the exact boundary, and one byte short yields no
 *       frame.
 * @brief Spec strengthening: the 2-byte ntohs path was only exercised via the zero-rejection case;
 *        here it frames a real payload and the at-boundary / one-short edges are pinned.
 */
TEST(ProtocolBaseFraming, SizeHeaderTwoByteWidthAtBoundaryAndOneShort) {
    ProtocolProbe                     probe;
    SizeHeaderProtocol<std::uint16_t> protocol{probe};
    const auto                        header = SizeHeaderProtocol<std::uint16_t>::Header(5u);
    EXPECT_EQ(ntohs(header), 5u);

    append_header(probe, header);
    EXPECT_EQ(protocol.getMessageSize(), 0u) << "header consumed; payload absent";
    EXPECT_EQ(probe.in().size(), 0u);

    probe.append("1234"); // one byte short of the announced 5
    EXPECT_EQ(protocol.getMessageSize(), 0u) << "4 of 5 payload bytes — no frame";
    probe.append("5"); // exactly at the boundary
    EXPECT_EQ(protocol.getMessageSize(), 5u) << "the fifth byte completes the frame";
    EXPECT_TRUE(protocol.ok());
}

/**
 * @test An 8-byte (`uint64_t`) header frames correctly. base.h takes the raw (non-byteswapped) path
 *       for widths other than 2/4, so `Header()` returns the host-order value verbatim.
 * @brief Spec strengthening: adds the 8-byte width that the original suite never covered, pinning
 *        base.h's `else` branch (no ntoh/hton). The header and payload are appended together to
 *        prove the frame is produced once the full message is present.
 */
TEST(ProtocolBaseFraming, SizeHeaderEightByteWidthFramesViaRawPath) {
    // base.h's Header() for an 8-byte width returns the value unchanged (raw path).
    EXPECT_EQ(SizeHeaderProtocol<std::uint64_t>::Header(6u), 6u);

    ProtocolProbe                     probe;
    SizeHeaderProtocol<std::uint64_t> protocol{probe};
    const auto                        header = SizeHeaderProtocol<std::uint64_t>::Header(6u);

    append_header(probe, header);
    EXPECT_EQ(protocol.shiftSize(), sizeof(std::uint64_t));
    EXPECT_EQ(protocol.getMessageSize(), 0u) << "8-byte header consumed; payload absent";
    EXPECT_EQ(probe.in().size(), 0u);

    probe.append("abcde"); // one short
    EXPECT_EQ(protocol.getMessageSize(), 0u);
    probe.append("f"); // exactly 6
    EXPECT_EQ(protocol.getMessageSize(), 6u);
    EXPECT_TRUE(protocol.ok());
}
