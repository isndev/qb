/**
 * @file qb/source/io/tests/system/test-uri-json.cpp
 * @brief System tests for qb URI parsing and JSON pipe serialization.
 *
 * These tests cover deterministic parsing, encoding and serialization contracts
 * that are shared by the IO layer without requiring network or filesystem state.
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
 *
 * @ingroup Tests
 */

#include <gtest/gtest.h>

#include <qb/io/uri.h>
#include <qb/json.h>
#include <qb/system/allocator/pipe.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::string
pipe_json(qb::json const &value) {
    qb::allocator::pipe<char> pipe;
    pipe.put<qb::json>(value);
    return pipe.str();
}

} // namespace

TEST(URI, ParsesAuthorityIpv6DefaultPortQueriesAndFragment) {
    const qb::io::uri uri(
        "https://user:pass@[2001:db8::1]/api/v1/resource?name=qb+framework&dup=1&dup=2&empty=&encoded=%7Bok%7D&&flag#section-2");

    ASSERT_TRUE(uri.is_valid());
    EXPECT_EQ(uri.scheme(), "https");
    EXPECT_EQ(uri.user_info(), "user:pass");
    EXPECT_EQ(uri.host(), "2001:db8::1");
    EXPECT_EQ(uri.port(), "443");
    EXPECT_EQ(uri.u_port(), 443u);
    EXPECT_EQ(uri.path(), "/api/v1/resource");
    EXPECT_EQ(uri.encoded_queries(),
              "name=qb+framework&dup=1&dup=2&empty=&encoded=%7Bok%7D&&flag");
    EXPECT_EQ(uri.query("name"), "qb framework");
    EXPECT_EQ(uri.query("dup", 0), "1");
    EXPECT_EQ(uri.query("dup", 1), "2");
    EXPECT_EQ(uri.query("empty"), "");
    EXPECT_EQ(uri.query("encoded"), "{ok}");
    EXPECT_EQ(uri.query("flag"), "");
    EXPECT_EQ(uri.query("missing", 0, "fallback"), "fallback");
    EXPECT_EQ(uri.fragment(), "section-2");
    EXPECT_EQ(uri.af(), AF_INET6);
}

TEST(URI, HandlesRelativeUnixCopyMoveAndPortContracts) {
    const qb::io::uri default_constructed;
    EXPECT_TRUE(default_constructed.is_valid());
    EXPECT_TRUE(default_constructed.path().empty());

    const qb::io::uri empty_source(std::string{});
    EXPECT_TRUE(empty_source.is_valid());
    EXPECT_EQ(empty_source.path(), "/");

    qb::io::uri relative("assets/../images/logo.png?mode=dark");
    ASSERT_TRUE(relative.is_valid());
    EXPECT_TRUE(relative.scheme().empty());
    EXPECT_TRUE(relative.host().empty());
    EXPECT_EQ(relative.path(), "assets/../images/logo.png");
    EXPECT_EQ(relative.query("mode"), "dark");

    qb::io::uri unix_uri("unix:///tmp/qb.sock");
    ASSERT_TRUE(unix_uri.is_valid());
    EXPECT_EQ(unix_uri.scheme(), "unix");
    EXPECT_EQ(unix_uri.af(), AF_UNIX);
    EXPECT_EQ(unix_uri.path(), "/tmp/qb.sock");
    EXPECT_EQ(unix_uri.port(), "0");

    qb::io::uri assigned;
    assigned = std::string("tcp://127.0.0.1:4242");
    EXPECT_TRUE(assigned.is_valid());
    EXPECT_EQ(assigned.host(), "127.0.0.1");
    EXPECT_EQ(assigned.u_port(), 4242u);

    qb::io::uri copied(assigned);
    EXPECT_EQ(copied.source(), assigned.source());
    EXPECT_EQ(copied.host(), "127.0.0.1");

    qb::io::uri moved(std::move(copied));
    EXPECT_EQ(moved.host(), "127.0.0.1");
    EXPECT_EQ(moved.u_port(), 4242u);

    qb::io::uri overflowing_port("tcp://localhost:99999");
    EXPECT_TRUE(overflowing_port.is_valid());
    EXPECT_EQ(overflowing_port.port(), "99999");
    EXPECT_EQ(overflowing_port.u_port(), 0u);

    qb::io::uri host_colon_without_numeric_port("tcp://example.com:notaport");
    EXPECT_TRUE(host_colon_without_numeric_port.is_valid());
    EXPECT_EQ(host_colon_without_numeric_port.host(), "example.com:notaport");
    EXPECT_EQ(host_colon_without_numeric_port.port(), "0");
    EXPECT_EQ(host_colon_without_numeric_port.u_port(), 0u);
}

TEST(URI, RejectsMalformedAuthorityAndPath) {
    EXPECT_FALSE(qb::io::uri("http://exa mple.com").is_valid());
    EXPECT_FALSE(qb::io::uri("http://[2001:db8::1").is_valid());
    EXPECT_FALSE(qb::io::uri("http://[2001:db8::1]:bad").is_valid());
    EXPECT_FALSE(qb::io::uri("http://example.com/path with spaces").is_valid());
}

TEST(URI, EncodesDecodesValidatesAndNormalizesPaths) {
    EXPECT_EQ(qb::io::uri::encode(std::string_view("hello world")), "hello+world");
    EXPECT_EQ(qb::io::uri::encode(std::string_view("100% ready")), "100%25+ready");
    EXPECT_EQ(qb::io::uri::decode(std::string_view("hello+world%21")), "hello world!");
    EXPECT_EQ(qb::io::uri::decode(std::string_view("%ZZ%")), "%ZZ%");
    const std::string invalid_iterator_encoded = "ok%ZZtail";
    EXPECT_EQ(qb::io::uri::decode(invalid_iterator_encoded.begin(),
                                  invalid_iterator_encoded.end()),
              "ok");
    EXPECT_TRUE(qb::io::uri::decode(nullptr, 4).empty());
    EXPECT_TRUE(qb::io::uri::encode(nullptr, 4).empty());

    const std::string bytes("\xC3\xA9", 2);
    EXPECT_EQ(qb::io::uri::encode(bytes.data(), bytes.size()), "%C3%A9");
    EXPECT_EQ(qb::io::uri::decode("%C3%A9", 6), bytes);

    EXPECT_TRUE(qb::io::uri::is_valid_scheme("h2+tls"));
    EXPECT_TRUE(qb::io::uri::is_valid_scheme("custom.v1-transport"));
    EXPECT_FALSE(qb::io::uri::is_valid_scheme(""));
    EXPECT_FALSE(qb::io::uri::is_valid_scheme("1http"));
    EXPECT_FALSE(qb::io::uri::is_valid_scheme("bad_scheme"));

    EXPECT_TRUE(qb::io::uri::is_valid_host("example.com"));
    EXPECT_TRUE(qb::io::uri::is_valid_host("[::1]"));
    EXPECT_FALSE(qb::io::uri::is_valid_host(""));
    EXPECT_FALSE(qb::io::uri::is_valid_host("bad host"));

    std::string empty_path;
    EXPECT_TRUE(qb::io::uri::normalize_path(empty_path));
    EXPECT_EQ(empty_path, "/");

    std::string dot_path(".");
    EXPECT_TRUE(qb::io::uri::normalize_path(dot_path));
    EXPECT_EQ(dot_path, "/");

    std::string absolute("/a/b/../c/./d//");
    EXPECT_TRUE(qb::io::uri::normalize_path(absolute));
    EXPECT_EQ(absolute, "/a/c/d");

    std::string relative("../a/./b/../../c");
    EXPECT_TRUE(qb::io::uri::normalize_path(relative));
    EXPECT_EQ(relative, "../c");

    std::string backslashes("a\\b\\..\\c");
    EXPECT_TRUE(qb::io::uri::normalize_path(backslashes));
    EXPECT_EQ(backslashes, "a/c");
}

TEST(JSONPipe, SerializesPrimitiveEmptyAndCompositeValues) {
    EXPECT_EQ(pipe_json(qb::json(nullptr)), "null");
    EXPECT_EQ(pipe_json(qb::json(true)), "true");
    EXPECT_EQ(pipe_json(qb::json(false)), "false");
    EXPECT_EQ(pipe_json(qb::json(-42)), "-42");
    EXPECT_EQ(pipe_json(qb::json(42u)), "42");
    EXPECT_EQ(pipe_json(qb::json("qb")), "\"qb\"");
    EXPECT_EQ(pipe_json(qb::json::object()), "{}");
    EXPECT_EQ(pipe_json(qb::json::array()), "[]");

    qb::json array = qb::json::array({1, "two", true, nullptr});
    EXPECT_EQ(pipe_json(array), "[1,\"two\",true,null]");

    qb::json object;
    object["array"] = array;
    object["empty"] = qb::json::object();
    object["float"] = 1.25;

    const std::string serialized = pipe_json(object);
    const qb::json    reparsed   = qb::json::parse(serialized);
    EXPECT_EQ(reparsed, object);
}

TEST(JSONPipe, SerializesDiscardedAndUuidRoundTrip) {
    const qb::json discarded = qb::json::parse("{", nullptr, false);
    ASSERT_TRUE(discarded.is_discarded());
    EXPECT_EQ(pipe_json(discarded), "<discarded>");

    const qb::uuid id = qb::generate_random_uuid();
    qb::json       encoded;
    uuids::to_json(encoded, id);

    qb::uuid decoded;
    uuids::from_json(encoded, decoded);
    EXPECT_EQ(decoded, id);
}
