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
 * reported, exactly as `io::dispatch` does (`getMessageSize()` > 0 -> `onMessage(size)` -> `flush(size)`).
 *
 * What is pinned:
 *   - the codec at the group boundaries (0, 127, 128, 16 383, 16 384, 2^21, 2^28, 2^32, 2^64 - 1), the two
 *     protobuf examples (300 = AC 02), incomplete at every split, over-long, overflow (a tenth group over 1),
 *     and the policy split: `80 00` is zero for protobuf and a protocol error for MQTT;
 *   - the framer byte by byte, an empty payload delivered (a one-byte frame), two frames back to back, the
 *     payload bound refused before its bytes arrive, a non-varint prefix refused, MQTT's lead byte (a `PUBLISH`
 *     and a `PINGREQ`) reaching the handler in `message::frame`, `Header()` the inverse of the decoder, `reset()`;
 *   - the netstring across every split, `0:,` delivered empty, back to back, every refusal (a leading zero, a
 *     non-digit, a colon first, a wrong trailing byte, too many digits, a payload over the bound), `Header()` /
 *     `put()` round trips, and the zero-copy view.
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
#include <vector>

#include <gtest/gtest.h>

#include <qb/io/async/buffered_io.h>
#include <qb/io/protocol/netstring.h>
#include <qb/io/protocol/varint.h>

namespace {

using namespace qb::protocol;

// The `_IO_` of the framers under test: an input pipe, the base_io_t the AProtocol layer befriends, and the
// handler the delivering protocols call -- which records the payload, the header size and the lead byte.
class FramingProbe {
    qb::allocator::pipe<char> _in;

public:
    using base_io_t = qb::io::async::buffered_io<FramingProbe>;

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

/// Run the framework's framing loop once over what the probe holds: every complete frame is delivered and flushed.
template <typename Protocol>
std::size_t
drive(Protocol &protocol, FramingProbe &probe) {
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
using MqttPackets    = varint_binary<FramingProbe, varint::mqtt_policy, 1, varint::mqtt_policy::max_value>;
using Netstrings     = netstring<FramingProbe>;
using SmallNetstring = netstring<FramingProbe, std::string, 3, 100>;
using NetstringViews = netstring_view<FramingProbe>;

} // namespace

// =============================================================================
// The codec
// =============================================================================

TEST(VarintCodec, EncodesAndDecodesTheGroupBoundaries) {
    const std::uint64_t values[] = {
        0u,         1u,         127u,         128u,         16'383u,          16'384u,
        2'097'151u, 2'097'152u, 268'435'455u, 268'435'456u, 4'294'967'296ull, std::numeric_limits<std::uint64_t>::max()
    };
    const std::size_t sizes[] = {1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 10};
    for (std::size_t i = 0; i < std::size(values); ++i) {
        char       out[varint::max_encoded_bytes];
        const auto n = varint::encode(values[i], out);
        EXPECT_EQ(n, sizes[i]) << "value " << values[i];
        EXPECT_EQ(varint::encoded_size(values[i]), sizes[i]);
        const auto r = varint::decode(out, out + n);
        EXPECT_EQ(r.status, varint::decode_status::complete) << "value " << values[i];
        EXPECT_EQ(r.value, values[i]);
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

TEST(NetstringFraming, HeaderAndPutRoundTripThroughTheFramer) {
    EXPECT_EQ(Netstrings::Header(5).view(), "5:");
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
