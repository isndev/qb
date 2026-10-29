/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/protocol/varint-netstring-framing.cpp
 * @brief `qb::protocol::varint` (the codec), `varint_prefixed` / `varint_binary` and `netstring` framing (Huly QB-80).
 *
 * Driven against an in-memory probe whose `in()` is a plain pipe -- NO socket, NO event loop -- so this is a
 * strict `unit` test of the parsing arithmetic, in the mould of protocol-base-framing.cpp. The probe also plays
 * the framework's part: it records every `message` the protocol delivers and flushes the frame the protocol
 * reported, exactly as the io's `process_messages()` does (`getMessageSize()` > 0 -> `onMessage(size)` ->
 * `flush(size)`). A second probe has a `max_message_size()`, as the io components do.
 *
 * What is pinned:
 *   - the codec at every group boundary (2^(7k) - 1 and 2^(7k) for k = 1..9, 0, 2^64 - 1), the two protobuf
 *     examples (300 = AC 02), incomplete at every split of a three-, a four- (MQTT's largest) and a ten-byte
 *     varint, over-long, overflow (a tenth group over 1), and the policy split: zero or 127 padded to two, three
 *     or four bytes is read by protobuf and refused by MQTT;
 *   - the framer byte by byte over a one-byte AND multi-byte prefixes (protobuf's two and three bytes, MQTT's
 *     lead byte then two, a ten-byte over-long one), an empty payload delivered (a one-byte frame), two frames back
 *     to back, the payload bound on both sides (refused before its bytes arrive, exactly the bound delivered), the
 *     component's runtime cap refused at the prefix, the default bounds leaving the header room under the cap, a
 *     non-varint prefix refused, MQTT's lead byte (a `PUBLISH` and a `PINGREQ`) reaching the handler in
 *     `message::frame`, `Header()` the inverse of the decoder, `reset()`;
 *   - the netstring across every split, `0:,` delivered empty, back to back, every refusal (a leading zero, a
 *     non-digit, a colon first, a wrong trailing byte, too many digits, a payload over the bound -- the length
 *     ones before the colon arrives), a length that would wrap a `size_t`, the runtime cap at the colon, the bound
 *     `_MaxDigits` sets, `Header()` / `put()` round trips, and the zero-copy view.
 */

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <qb/io/async/buffered_io.h>
#include <qb/io/protocol/netstring.h>
#include <qb/io/protocol/varint.h>

namespace {

using namespace qb::protocol;

// The `_IO_` of the framers under test: an input pipe, the base_io_t the AProtocol layer befriends, and the
// handler the delivering protocols call -- which records the payload, the header size and the lead byte. With a
// `Cap`, it also has the io components' `max_message_size()`, which the framers read at the prefix.
template <std::size_t Cap = 0>
class BasicFramingProbe {
    qb::allocator::pipe<char> _in;

public:
    using base_io_t = qb::io::async::buffered_io<BasicFramingProbe>;

    [[nodiscard]] std::size_t
    max_message_size() const noexcept
    requires(Cap != 0)
    {
        return Cap;
    }

    std::vector<std::string>  delivered;
    std::vector<std::size_t>  header_sizes;
    std::vector<unsigned>     leads;
    std::vector<const char *> text_data; ///< where a netstring's `text` pointed: the buffer (a view) or a copy

    qb::allocator::pipe<char> &
    in() noexcept {
        return _in;
    }
    void
    append(std::string_view data) {
        _in << data;
    }
    /// The framework's flush of a reported frame.
    void
    consume(std::size_t n) {
        _in.free_front(n);
    }
    template <typename Message>
    void
    on(Message &&m) {
        delivered.emplace_back(m.data, m.size);
        if constexpr (requires {
                          m.header_size;
                          m.frame;
                      }) {
            header_sizes.push_back(m.header_size);
            leads.push_back(static_cast<unsigned>(static_cast<unsigned char>(m.frame[0])));
        }
        if constexpr (requires { m.text; })
            text_data.push_back(std::string_view(m.text).data());
    }
};

using FramingProbe = BasicFramingProbe<>;
using CappedProbe  = BasicFramingProbe<64>; ///< a component running with set_max_message_size(64)

/// Run the framework's framing loop once over what the probe holds: every complete frame is delivered and flushed.
template <typename Protocol, typename Probe>
std::size_t
drive(Protocol &protocol, Probe &probe) {
    std::size_t frames = 0;
    std::size_t size   = 0;
    while (protocol.ok() && (size = protocol.getMessageSize()) > 0) {
        protocol.onMessage(size);
        probe.consume(size);
        ++frames;
    }
    return frames;
}

std::string
bytes(std::initializer_list<unsigned> values) {
    std::string s;
    for (auto v : values)
        s.push_back(static_cast<char>(static_cast<unsigned char>(v)));
    return s;
}

using ProtobufFrames = varint_binary<FramingProbe>;
using BoundedFrames  = varint_binary<FramingProbe, varint::protobuf_policy, 0, 64>;
using MqttPackets    = varint_binary<FramingProbe, varint::mqtt_policy, 1>; ///< the default bound (the guide's advice)
using Netstrings     = netstring<FramingProbe>;
using SmallNetstring = netstring<FramingProbe, std::string, 3, 100>;
using NetstringViews = netstring_view<FramingProbe>;

/// Feed `frame` one byte at a time: before its last byte the framer must answer 0 and stay ok() after EVERY byte --
/// inside the prefix (the varint's incomplete verdict) as inside the payload (the cached header); then one delivery.
template <typename Protocol>
::testing::AssertionResult
frames_byte_by_byte(std::string const &frame, std::string const &payload, std::size_t header_size) {
    FramingProbe probe;
    Protocol     protocol{probe};
    for (std::size_t i = 0; i + 1 < frame.size(); ++i) {
        probe.append(std::string_view(&frame[i], 1));
        if (const auto n = protocol.getMessageSize(); n != 0u)
            return ::testing::AssertionFailure() << "reported " << n << " after " << i + 1 << " of " << frame.size() << " bytes";
        if (!protocol.ok())
            return ::testing::AssertionFailure() << "refused after " << i + 1 << " of " << frame.size() << " bytes";
    }
    probe.append(std::string_view(&frame.back(), 1));
    if (const auto n = drive(protocol, probe); n != 1u)
        return ::testing::AssertionFailure() << n << " frame(s) delivered, 1 expected";
    if (probe.delivered[0] != payload)
        return ::testing::AssertionFailure() << "a payload of " << probe.delivered[0].size() << " bytes, " << payload.size() << " expected";
    if (probe.header_sizes[0] != header_size)
        return ::testing::AssertionFailure() << "a header of " << probe.header_sizes[0] << " bytes, " << header_size << " expected";
    if (probe.in().size() != 0u)
        return ::testing::AssertionFailure() << probe.in().size() << " byte(s) left after the flush";
    return ::testing::AssertionSuccess();
}

} // namespace

// =============================================================================
// The codec
// =============================================================================

TEST(VarintCodec, EncodesAndDecodesTheGroupBoundaries) {
    // Every boundary between a k-byte and a (k+1)-byte varint: 2^(7k) - 1 takes k bytes, 2^(7k) takes k + 1, for
    // k = 1..9 -- 2^63 is where the tenth group first appears -- and the two ends, 0 and 2^64 - 1.
    std::vector<std::pair<std::uint64_t, std::size_t>> cases{{0u, 1u}, {1u, 1u}, {std::numeric_limits<std::uint64_t>::max(), 10u}};
    for (std::size_t k = 1; k <= 9; ++k) {
        const std::uint64_t boundary = std::uint64_t{1} << (7 * k);
        cases.emplace_back(boundary - 1u, k);
        cases.emplace_back(boundary, k + 1u);
    }
    ASSERT_EQ(cases.size(), 21u);
    for (auto const &[value, size] : cases) {
        char       out[varint::max_encoded_bytes];
        const auto n = varint::encode(value, out);
        EXPECT_EQ(n, size) << "value " << value;
        EXPECT_EQ(varint::encoded_size(value), size) << "value " << value;
        const auto r = varint::decode(out, out + n);
        EXPECT_EQ(r.status, varint::decode_status::complete) << "value " << value;
        EXPECT_EQ(r.value, value);
        EXPECT_EQ(r.consumed, n);
    }
    // The two spellings everyone knows: protobuf's documentation example, and 128.
    char out[varint::max_encoded_bytes];
    ASSERT_EQ(varint::encode(300u, out), 2u);
    EXPECT_EQ(std::string_view(out, 2), bytes({0xAC, 0x02}));
    ASSERT_EQ(varint::encode(128u, out), 2u);
    EXPECT_EQ(std::string_view(out, 2), bytes({0x80, 0x01}));
    ASSERT_EQ(varint::encode(std::numeric_limits<std::uint64_t>::max(), out), 10u);
    EXPECT_EQ(static_cast<unsigned char>(out[9]), 0x01u) << "the tenth group holds the top bit alone";
}

TEST(VarintCodec, DecodeIsIncompleteAtEverySplit) {
    const auto frame = bytes({0x80, 0x80, 0x01}); // 16 384, three bytes
    for (std::size_t have = 0; have < frame.size(); ++have) {
        const auto r = varint::decode(frame.data(), frame.data() + have);
        EXPECT_EQ(r.status, varint::decode_status::incomplete) << "with " << have << " byte(s)";
        EXPECT_EQ(r.consumed, have);
    }
    const auto r = varint::decode(frame.data(), frame.data() + frame.size());
    EXPECT_EQ(r.status, varint::decode_status::complete);
    EXPECT_EQ(r.value, 16'384u);
    EXPECT_EQ(r.consumed, 3u);

    // The longest varint, 2^64 - 1 in ten bytes, at each of its splits: no tenth-byte verdict before the tenth byte.
    char       longest[varint::max_encoded_bytes];
    const auto ten = varint::encode(std::numeric_limits<std::uint64_t>::max(), longest);
    ASSERT_EQ(ten, 10u);
    for (std::size_t have = 0; have < ten; ++have) {
        const auto split = varint::decode(longest, longest + have);
        EXPECT_EQ(split.status, varint::decode_status::incomplete) << "2^64 - 1 with " << have << " byte(s)";
        EXPECT_EQ(split.consumed, have);
    }
    // MQTT's largest, `FF FF FF 7F`, where an incomplete prefix meets the over-long verdict: incomplete up to the
    // fourth byte, complete with it.
    const auto mqtt_max = bytes({0xFF, 0xFF, 0xFF, 0x7F});
    for (std::size_t have = 0; have < mqtt_max.size(); ++have) {
        const auto split = varint::decode<varint::mqtt_policy>(mqtt_max.data(), mqtt_max.data() + have);
        EXPECT_EQ(split.status, varint::decode_status::incomplete) << "FF FF FF 7F with " << have << " byte(s)";
        EXPECT_EQ(split.consumed, have);
    }
    EXPECT_EQ(varint::decode<varint::mqtt_policy>(mqtt_max.data(), mqtt_max.data() + 4).status, varint::decode_status::complete);
}

TEST(VarintCodec, RefusesOverlongOverflowAndNonCanonicalPerPolicy) {
    // Eleven continuation bytes: protobuf's ten are read, none ends the number.
    const auto eleven = std::string(11, static_cast<char>(0x80));
    auto       r      = varint::decode(eleven.data(), eleven.data() + eleven.size());
    EXPECT_EQ(r.status, varint::decode_status::overlong);
    EXPECT_EQ(r.consumed, 10u);
    // Five for MQTT: its four are read.
    r = varint::decode<varint::mqtt_policy>(eleven.data(), eleven.data() + 5);
    EXPECT_EQ(r.status, varint::decode_status::overlong);
    EXPECT_EQ(r.consumed, 4u);
    // Nine full groups then a tenth over 1: past 64 bits.
    auto over = std::string(9, static_cast<char>(0xFF));
    over.push_back(static_cast<char>(0x02));
    r = varint::decode(over.data(), over.data() + over.size());
    EXPECT_EQ(r.status, varint::decode_status::overflow);
    // `80 00`: zero spelled in two bytes -- protobuf reads it, MQTT refuses the non-minimal encoding.
    const auto padded = bytes({0x80, 0x00});
    r                 = varint::decode(padded.data(), padded.data() + 2);
    EXPECT_EQ(r.status, varint::decode_status::complete);
    EXPECT_EQ(r.value, 0u);
    EXPECT_EQ(r.consumed, 2u);
    r = varint::decode<varint::mqtt_policy>(padded.data(), padded.data() + 2);
    EXPECT_EQ(r.status, varint::decode_status::non_canonical);
    // Padded to three and four bytes ([MQTT-1.5.5-1] holds at every length, not only two): zero, and 127.
    struct padded_case {
        std::string   encoding;
        std::uint64_t value;
    };
    const padded_case longer[] = {
        {bytes({0x80, 0x80, 0x00}), 0u},
        {bytes({0x80, 0x80, 0x80, 0x00}), 0u},
        {bytes({0xFF, 0x80, 0x00}), 127u},
        {bytes({0xFF, 0x80, 0x80, 0x00}), 127u},
    };
    for (auto const &c : longer) {
        const auto *b = c.encoding.data();
        const auto *e = b + c.encoding.size();
        const auto  p = varint::decode(b, e);
        EXPECT_EQ(p.status, varint::decode_status::complete) << c.encoding.size() << "-byte " << c.value << ": protobuf reads it";
        EXPECT_EQ(p.value, c.value);
        EXPECT_EQ(p.consumed, c.encoding.size());
        EXPECT_EQ(varint::decode<varint::mqtt_policy>(b, e).status, varint::decode_status::non_canonical)
            << c.encoding.size() << "-byte " << c.value << ": MQTT refuses the padding";
    }
    // MQTT's largest value, `FF FF FF 7F`, is read whole.
    const auto largest = bytes({0xFF, 0xFF, 0xFF, 0x7F});
    r                  = varint::decode<varint::mqtt_policy>(largest.data(), largest.data() + 4);
    EXPECT_EQ(r.status, varint::decode_status::complete);
    EXPECT_EQ(r.value, 268'435'455u);
}

TEST(VarintCodec, HeaderIsMinimalAndBoundedByThePolicy) {
    EXPECT_EQ(varint::encode_header(300u).view(), bytes({0xAC, 0x02}));
    EXPECT_EQ(varint::encode_header<varint::mqtt_policy>(268'435'455u).size, 4u);
    EXPECT_THROW((void) varint::encode_header<varint::mqtt_policy>(268'435'456u), std::length_error);
    EXPECT_EQ(varint::encode_header(std::numeric_limits<std::uint64_t>::max()).size, 10u);
    EXPECT_EQ(varint::encode_header(0u).view(), std::string_view("\0", 1));
}

// =============================================================================
// varint_prefixed / varint_binary
// =============================================================================

TEST(VarintFraming, FramesByteByByteAndDeliversThePayload) {
    FramingProbe   probe;
    ProtobufFrames protocol{probe};
    const auto     frame = bytes({0x05}) + "hello";
    for (std::size_t i = 0; i + 1 < frame.size(); ++i) {
        probe.append(std::string_view(&frame[i], 1));
        EXPECT_EQ(protocol.getMessageSize(), 0u) << "after " << i + 1 << " byte(s)";
        EXPECT_TRUE(protocol.ok());
    }
    probe.append(std::string_view(&frame.back(), 1));
    EXPECT_EQ(drive(protocol, probe), 1u);
    ASSERT_EQ(probe.delivered.size(), 1u);
    EXPECT_EQ(probe.delivered[0], "hello");
    EXPECT_EQ(probe.header_sizes[0], 1u);
    EXPECT_EQ(probe.in().size(), 0u) << "the frame was flushed whole";
}

// The branch a TCP read that ends INSIDE a multi-byte length takes -- the common path for any payload of 128 bytes
// or more: the varint is incomplete, the framer answers 0 and stays ok, and the next byte resumes the decode.
TEST(VarintFraming, FramesByteByByteAcrossAMultiBytePrefix) {
    const std::string p300(300, 'a'), p16384(16'384, 'b'), p16383(16'383, 'c');
    EXPECT_TRUE(frames_byte_by_byte<ProtobufFrames>(bytes({0xAC, 0x02}) + p300, p300, 2u)) << "AC 02: 300";
    EXPECT_TRUE(frames_byte_by_byte<ProtobufFrames>(bytes({0x80, 0x80, 0x01}) + p16384, p16384, 3u)) << "80 80 01: 16 384";
    EXPECT_TRUE(frames_byte_by_byte<MqttPackets>(bytes({0x30, 0xFF, 0x7F}) + p16383, p16383, 3u)) << "30 FF 7F: an MQTT PUBLISH";
    // A ten-byte over-long encoding of 5, which protobuf accepts: the framer takes the whole prefix, byte by byte.
    EXPECT_TRUE(
        frames_byte_by_byte<ProtobufFrames>(bytes({0x85, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00}) + "12345", "12345", 10u))
        << "an over-long ten-byte prefix";
}

TEST(VarintFraming, DeliversAnEmptyPayloadAsAOneByteFrame) {
    FramingProbe   probe;
    ProtobufFrames protocol{probe};
    probe.append(bytes({0x00}));
    EXPECT_EQ(protocol.getMessageSize(), 1u) << "a frame is never zero bytes: the prefix is one";
    EXPECT_EQ(drive(protocol, probe), 1u);
    ASSERT_EQ(probe.delivered.size(), 1u);
    EXPECT_EQ(probe.delivered[0].size(), 0u);
    EXPECT_EQ(probe.header_sizes[0], 1u);
}

TEST(VarintFraming, TwoBytePrefixAndTwoFramesBackToBack) {
    FramingProbe      probe;
    ProtobufFrames    protocol{probe};
    const std::string big(300, 'x');
    probe.append(ProtobufFrames::Header(big.size()).view());
    probe.append(big);
    probe.append(bytes({0x03}) + "abc");
    EXPECT_EQ(protocol.getMessageSize(), 302u);
    EXPECT_EQ(drive(protocol, probe), 2u);
    ASSERT_EQ(probe.delivered.size(), 2u);
    EXPECT_EQ(probe.delivered[0], big);
    EXPECT_EQ(probe.header_sizes[0], 2u);
    EXPECT_EQ(probe.delivered[1], "abc");
    EXPECT_EQ(probe.header_sizes[1], 1u);
}

TEST(VarintFraming, RefusesAPayloadOverTheBoundBeforeItsBytesArrive) {
    FramingProbe  probe;
    BoundedFrames protocol{probe};
    probe.append(bytes({0x41})); // 65 > 64, no payload byte yet
    EXPECT_EQ(protocol.getMessageSize(), 0u);
    EXPECT_FALSE(protocol.ok()) << "a length past the bound is a protocol error at the prefix";
    EXPECT_THROW((void) BoundedFrames::Header(65), std::length_error);
    EXPECT_EQ(BoundedFrames::Header(64).view(), bytes({0x40}));
}

TEST(VarintFraming, DeliversAPayloadOfExactlyTheBound) {
    FramingProbe      probe;
    BoundedFrames     protocol{probe};
    const std::string payload(64, 'q');
    probe.append(bytes({0x40}) + payload); // 64: the bound itself, as Header(64) writes it
    EXPECT_EQ(drive(protocol, probe), 1u);
    EXPECT_TRUE(protocol.ok()) << "a payload of exactly _MaxPayload is a frame, not a protocol error";
    ASSERT_EQ(probe.delivered.size(), 1u);
    EXPECT_EQ(probe.delivered[0], payload);
}

// The io refuses a frame over its max_message_size() only once the frame is reported -- all of it buffered. The
// framer reads the cap the component runs with and refuses at the prefix (Huly QB-80): a peer announcing more than
// the cap is not waited for. Exactly the cap is a frame.
TEST(VarintFraming, RefusesAFrameOverTheComponentsCapAtThePrefix) {
    {
        CappedProbe                probe;
        varint_binary<CappedProbe> protocol{probe};
        probe.append(bytes({0x64})); // 100 announced: a 101-byte frame against a cap of 64, none of it here yet
        EXPECT_EQ(protocol.getMessageSize(), 0u);
        EXPECT_FALSE(protocol.ok()) << "the component's cap must refuse the frame at its prefix";
    }
    {
        CappedProbe                probe;
        varint_binary<CappedProbe> protocol{probe};
        const std::string          payload(63, 'k');
        probe.append(bytes({0x3F}) + payload); // 1 + 63 = 64: exactly the cap
        EXPECT_EQ(drive(protocol, probe), 1u);
        EXPECT_TRUE(protocol.ok());
        ASSERT_EQ(probe.delivered.size(), 1u);
        EXPECT_EQ(probe.delivered[0], payload);
    }
}

// The default bounds leave the header room under QB_MAX_MESSAGE_SIZE (Huly QB-80): what a framer accepts, and what its
// Header() writes, is a frame the io accepts -- the default used to be the cap itself, for the payload alone.
TEST(VarintFraming, TheDefaultBoundLeavesTheHeaderRoomUnderTheCap) {
    constexpr std::size_t cap = QB_MAX_MESSAGE_SIZE;
    static_assert(ProtobufFrames::max_payload == cap - varint::protobuf_policy::max_bytes);
    static_assert(MqttPackets::max_payload
                  == (cap - 1u - varint::mqtt_policy::max_bytes < varint::mqtt_policy::max_value
                          ? cap - 1u - varint::mqtt_policy::max_bytes
                          : static_cast<std::size_t>(varint::mqtt_policy::max_value)));
    static_assert(varint::default_max_payload<varint::mqtt_policy, 1>() == MqttPackets::max_payload);
    EXPECT_THROW((void) ProtobufFrames::Header(cap), std::length_error) << "a frame of cap + header bytes would be refused by the io";
    EXPECT_LE(ProtobufFrames::Header(ProtobufFrames::max_payload).size + ProtobufFrames::max_payload, cap);
}

TEST(VarintFraming, RefusesANonVarintPrefix) {
    {
        FramingProbe   probe;
        ProtobufFrames protocol{probe};
        probe.append(std::string(10, static_cast<char>(0x80)));
        EXPECT_EQ(protocol.getMessageSize(), 0u);
        EXPECT_FALSE(protocol.ok()) << "ten continuation bytes: over-long";
    }
    {
        FramingProbe probe;
        MqttPackets  protocol{probe};
        probe.append(bytes({0x30, 0x80, 0x00})); // a lead byte, then zero spelled non-minimally
        EXPECT_EQ(protocol.getMessageSize(), 0u);
        EXPECT_FALSE(protocol.ok()) << "MQTT requires the minimal encoding";
    }
}

TEST(VarintFraming, LeadBytesFrameAnMqttControlPacket) {
    FramingProbe probe;
    MqttPackets  protocol{probe};
    // PUBLISH (0x30), remaining length 5, five bytes of the rest -- arriving in pieces.
    probe.append(bytes({0x30}));
    EXPECT_EQ(protocol.getMessageSize(), 0u) << "the lead byte alone: not a byte of the varint yet";
    probe.append(bytes({0x05}) + "hel");
    EXPECT_EQ(protocol.getMessageSize(), 0u);
    probe.append("lo");
    EXPECT_EQ(protocol.getMessageSize(), 7u);
    // PINGREQ (0xC0), remaining length 0: a two-byte frame with an empty payload.
    probe.append(bytes({0xC0, 0x00}));
    EXPECT_EQ(drive(protocol, probe), 2u);
    ASSERT_EQ(probe.delivered.size(), 2u);
    EXPECT_EQ(probe.delivered[0], "hello");
    EXPECT_EQ(probe.header_sizes[0], 2u);
    EXPECT_EQ(probe.leads[0], 0x30u);
    EXPECT_EQ(probe.delivered[1].size(), 0u);
    EXPECT_EQ(probe.header_sizes[1], 2u);
    EXPECT_EQ(probe.leads[1], 0xC0u);
    EXPECT_TRUE(protocol.ok());
}

TEST(VarintFraming, ResetForgetsACachedPrefix) {
    FramingProbe   probe;
    ProtobufFrames protocol{probe};
    probe.append(bytes({0xAC, 0x02})); // 300 announced, nothing of it here
    EXPECT_EQ(protocol.getMessageSize(), 0u);
    protocol.reset();
    probe.in().reset();
    probe.append(bytes({0x02}) + "ab");
    EXPECT_EQ(protocol.getMessageSize(), 3u) << "the forgotten prefix must not shadow the new one";
    EXPECT_EQ(drive(protocol, probe), 1u);
    EXPECT_EQ(probe.delivered[0], "ab");
}

// =============================================================================
// netstring
// =============================================================================

TEST(NetstringFraming, FramesAcrossEverySplitAndDeliversText) {
    FramingProbe probe;
    Netstrings   protocol{probe};
    for (std::string_view piece : {"5", ":", "he", "llo"}) {
        probe.append(piece);
        EXPECT_EQ(protocol.getMessageSize(), 0u) << "after '" << piece << "'";
        EXPECT_TRUE(protocol.ok());
    }
    probe.append(",");
    EXPECT_EQ(protocol.getMessageSize(), 8u);
    EXPECT_EQ(drive(protocol, probe), 1u);
    ASSERT_EQ(probe.delivered.size(), 1u);
    EXPECT_EQ(probe.delivered[0], "hello");
    static_assert(std::is_same_v<decltype(Netstrings::message::text), std::string>);
}

TEST(NetstringFraming, DeliversTheEmptyNetstringAndFramesBackToBack) {
    FramingProbe probe;
    Netstrings   protocol{probe};
    probe.append("0:,3:abc,");
    EXPECT_EQ(protocol.getMessageSize(), 3u);
    EXPECT_EQ(drive(protocol, probe), 2u);
    ASSERT_EQ(probe.delivered.size(), 2u);
    EXPECT_EQ(probe.delivered[0].size(), 0u);
    EXPECT_EQ(probe.delivered[1], "abc");
    EXPECT_EQ(probe.in().size(), 0u);
}

TEST(NetstringFraming, RefusesWhatIsNotANetstring) {
    const std::string_view bad[] = {
        "05:hello,", // a leading zero
        "5x",        // a byte other than a digit before the colon
        ":",         // a colon with no length
        "5:hello;",  // the wrong byte where the comma belongs
    };
    for (auto input : bad) {
        FramingProbe probe;
        Netstrings   protocol{probe};
        probe.append(input);
        EXPECT_EQ(protocol.getMessageSize(), 0u) << input;
        EXPECT_FALSE(protocol.ok()) << input;
    }
}

TEST(NetstringFraming, RefusesTooManyDigitsAndAPayloadOverTheBound) {
    {
        FramingProbe   probe;
        SmallNetstring protocol{probe};
        probe.append("1234:");
        EXPECT_EQ(protocol.getMessageSize(), 0u);
        EXPECT_FALSE(protocol.ok()) << "four digits where three are the most";
    }
    {
        FramingProbe   probe;
        SmallNetstring protocol{probe};
        probe.append("999:");
        EXPECT_EQ(protocol.getMessageSize(), 0u);
        EXPECT_FALSE(protocol.ok()) << "999 bytes announced over a bound of 100: refused before they arrive";
    }
    {
        FramingProbe      probe;
        SmallNetstring    protocol{probe};
        const std::string payload(100, 'p');
        probe.append("100:" + payload + ",");
        EXPECT_EQ(protocol.getMessageSize(), 105u);
        EXPECT_EQ(drive(protocol, probe), 1u);
        EXPECT_EQ(probe.delivered[0], payload);
        EXPECT_THROW((void) SmallNetstring::Header(101), std::length_error);
    }
}

// The early refusals happen without the colon (Huly QB-80): a peer streaming digits and never the colon is refused
// at the digit that shows the length is wrong -- a leading zero, one digit too many, a length already over the
// bound -- not buffered up to the read cap while every read rescans the run.
TEST(NetstringFraming, RefusesABadLengthBeforeTheColon) {
    using ThreeDigits = netstring<FramingProbe, std::string, 3, 999>; // the bound is the digits' own: 999
    {
        FramingProbe probe;
        ThreeDigits  protocol{probe};
        probe.append("1234");
        EXPECT_EQ(protocol.getMessageSize(), 0u);
        EXPECT_FALSE(protocol.ok()) << "a fourth digit where three are the most, no colon yet";
    }
    {
        FramingProbe probe;
        Netstrings   protocol{probe};
        probe.append("05");
        EXPECT_EQ(protocol.getMessageSize(), 0u);
        EXPECT_FALSE(protocol.ok()) << "a leading zero, no colon yet";
    }
    {
        FramingProbe   probe;
        SmallNetstring protocol{probe};
        probe.append("999");
        EXPECT_EQ(protocol.getMessageSize(), 0u);
        EXPECT_FALSE(protocol.ok()) << "999 over a bound of 100 is known at the third digit, no colon yet";
    }
    {
        FramingProbe   probe;
        SmallNetstring protocol{probe};
        probe.append("99");
        EXPECT_EQ(protocol.getMessageSize(), 0u);
        EXPECT_TRUE(protocol.ok()) << "99 is under the bound: the colon is waited for";
    }
}

// The digit limit is tested before the digit is added, and that is what keeps a length of digits10 + 1 digits from
// wrapping `len` past SIZE_MAX back under the bound: 2^64 (2^32 on a 32-bit size_t) would read as 0 -- an empty
// netstring -- in a type whose bound is the most a size_t of digits10 digits allows.
TEST(NetstringFraming, ALengthThatWouldWrapASizeTIsRefused) {
    constexpr std::size_t digits = static_cast<std::size_t>(std::numeric_limits<std::size_t>::digits10);
    using Widest                 = netstring<FramingProbe, std::string, digits, std::numeric_limits<std::size_t>::max() - digits - 2u>;
    const std::string wraps      = sizeof(std::size_t) == 8 ? "18446744073709551616:," : "4294967296:,";
    FramingProbe      probe;
    Widest            protocol{probe};
    probe.append(wraps);
    EXPECT_EQ(protocol.getMessageSize(), 0u);
    EXPECT_FALSE(protocol.ok()) << "a length one digit past digits10 must be refused, never wrapped";
    EXPECT_TRUE(probe.delivered.empty());
}

// _MaxDigits caps the bound (Huly QB-80): max_payload is the smaller of _MaxPayload and the largest length the digits
// write, and Header() shares it -- so Header(n) never writes a length its own decoder refuses.
TEST(NetstringFraming, TheDigitLimitBoundsThePayloadAndHeader) {
    using ThreeDigitsWideBound = netstring<FramingProbe, std::string, 3, 100'000>;
    static_assert(ThreeDigitsWideBound::max_payload == 999u);
    static_assert(SmallNetstring::max_payload == 100u);
    EXPECT_THROW((void) ThreeDigitsWideBound::Header(1000), std::length_error) << "\"1000:\" is refused by the same type";
    EXPECT_EQ(ThreeDigitsWideBound::Header(999).view(), "999:");
    FramingProbe         probe;
    ThreeDigitsWideBound protocol{probe};
    const std::string    payload(999, 'w');
    probe.append(std::string(ThreeDigitsWideBound::Header(payload.size()).view()) + payload + ",");
    EXPECT_EQ(drive(protocol, probe), 1u);
    EXPECT_EQ(probe.delivered[0], payload);
    // The default bound leaves the digits, the colon and the comma room under the io's cap.
    static_assert(Netstrings::max_payload == QB_MAX_MESSAGE_SIZE - 9u - 2u);
    EXPECT_THROW((void) Netstrings::Header(QB_MAX_MESSAGE_SIZE), std::length_error);
}

TEST(NetstringFraming, RefusesAFrameOverTheComponentsCapAtTheColon) {
    {
        CappedProbe            probe;
        netstring<CappedProbe> protocol{probe};
        probe.append("61:"); // 3 + 61 + 1 = 65 against a cap of 64, no payload byte yet
        EXPECT_EQ(protocol.getMessageSize(), 0u);
        EXPECT_FALSE(protocol.ok()) << "the component's cap must refuse the frame at the colon";
    }
    {
        CappedProbe            probe;
        netstring<CappedProbe> protocol{probe};
        const std::string      payload(60, 'c');
        probe.append("60:" + payload + ","); // exactly the cap
        EXPECT_EQ(drive(protocol, probe), 1u);
        EXPECT_TRUE(protocol.ok());
        EXPECT_EQ(probe.delivered[0], payload);
    }
}

TEST(NetstringFraming, HeaderAndPutRoundTripThroughTheFramer) {
    EXPECT_EQ(Netstrings::Header(5).view(), "5:");
    EXPECT_EQ(Netstrings::Header(5).data()[0], '5');
    EXPECT_EQ(Netstrings::Header(0).view(), "0:");
    EXPECT_EQ(Netstrings::Header(99'999'999).view(), "99999999:"); // under the 100 MiB default bound
    EXPECT_THROW((void) Netstrings::Header(Netstrings::max_payload + 1u), std::length_error);
    EXPECT_EQ(Netstrings::end, ',');

    FramingProbe probe;
    Netstrings   protocol{probe};
    Netstrings::put(probe.in(), "hello");
    Netstrings::put(probe.in(), "");
    EXPECT_EQ(std::string_view(probe.in().cbegin(), probe.in().size()), "5:hello,0:,");
    EXPECT_EQ(drive(protocol, probe), 2u);
    ASSERT_EQ(probe.delivered.size(), 2u);
    EXPECT_EQ(probe.delivered[0], "hello");
    EXPECT_EQ(probe.delivered[1], "");
}

TEST(NetstringFraming, TheViewFormIsZeroCopy) {
    FramingProbe   probe;
    NetstringViews protocol{probe};
    static_assert(std::is_same_v<decltype(NetstringViews::message::text), const std::string_view>);
    probe.append("5:hello,");
    const char *payload = probe.in().cbegin() + 2;
    EXPECT_EQ(protocol.getMessageSize(), 8u);
    EXPECT_EQ(protocol.shiftSize(8u), 5u);
    EXPECT_EQ(protocol.header_size(), 2u);
    protocol.onMessage(8u);
    ASSERT_EQ(probe.delivered.size(), 1u);
    EXPECT_EQ(probe.delivered[0], "hello");
    ASSERT_EQ(probe.text_data.size(), 1u);
    EXPECT_EQ(static_cast<const void *>(probe.text_data[0]), static_cast<const void *>(payload))
        << "the view form's text must point into the input buffer, not at a copy";
}

TEST(NetstringFraming, TheStringFormCopies) {
    FramingProbe probe;
    Netstrings   protocol{probe};
    probe.append("5:hello,");
    const char *payload = probe.in().cbegin() + 2;
    ASSERT_EQ(protocol.getMessageSize(), 8u);
    protocol.onMessage(8u);
    ASSERT_EQ(probe.text_data.size(), 1u);
    EXPECT_NE(static_cast<const void *>(probe.text_data[0]), static_cast<const void *>(payload)) << "the std::string form owns its bytes";
}
