/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/core/uri-parse.cpp
 * @brief `qb::io::uri` — RFC 3986 parse / validate / encode / decode / normalize.
 *
 * `qb::io::uri` (qb/io/uri.h) is a pure string state machine: it parses a source string into
 * scheme/user-info/host/port/path/query/fragment views, infers the address family, decodes the
 * query map, and exposes static encode/decode/normalize helpers. None of it touches a socket or
 * the event loop, so this is a strict `unit` test — NO `qb::Main`, NO wall clock, NO sleeps.
 *
 * Consolidated from three previously-overlapping sources (spec D4 + §2):
 *   - the URI half of system/test-uri-json.cpp (authority/IPv6/query/fragment, relative + unix,
 *     malformed rejection, encode/decode/validate/normalize);
 *   - test-io.cpp::URI.Resolving (the ~22 scheme×host×port permutation cases) collapsed into a
 *     single TEST_P matrix so the wall of copy-pasted `EXPECT_TRUE(u.scheme()=="https")` lines
 *     becomes one parametrized, exact-value table;
 *   - test-io.cpp URIRegression.* (decode trailing-percent, re-parse clears stale components,
 *     copy/move preserve AF) and URIRobustness.* (template-vs-string_view decode divergence,
 *     encode/decode roundtrip, query parsing edges, is_valid, port out-of-range truncation).
 *
 * Strengthened over the originals: every `EXPECT_TRUE(a == b)` becomes `EXPECT_EQ`; the
 * roundtrip corpus is a TEST_P; and the spec D4 additions are pinned — `/../`-escape normalize,
 * Windows drive-letter / backslash normalize, and an IDN/punycode host (the parser is byte-level,
 * so it carries the already-encoded `xn--` label through verbatim rather than transcoding it).
 */

#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <qb/io/uri.h>

using qb::io::uri;

// =============================================================================
// AUTHORITY / IPv6 / QUERY / FRAGMENT — the full-featured parse
// =============================================================================

/**
 * @test One maximal URI exercises every structural component at once.
 * @brief userinfo + bracketed IPv6 host + default port + multi/empty/encoded queries + fragment.
 *
 * Folded from test-uri-json.cpp::ParsesAuthorityIpv6DefaultPortQueriesAndFragment. The
 * default-port inference (`https` ⇒ 443), duplicate-key multi-value queries, empty value,
 * percent-decoded value, and an `&&`/bare-flag run are all asserted in one place.
 */
TEST(UriParse, FullAuthorityIpv6QueriesAndFragment) {
    const uri u("https://user:pass@[2001:db8::1]/api/v1/resource"
                "?name=qb+framework&dup=1&dup=2&empty=&encoded=%7Bok%7D&&flag#section-2");

    ASSERT_TRUE(u.is_valid());
    EXPECT_EQ(u.scheme(), "https");
    EXPECT_EQ(u.user_info(), "user:pass");
    EXPECT_EQ(u.host(), "2001:db8::1");
    EXPECT_EQ(u.port(), "443");
    EXPECT_EQ(u.u_port(), 443u);
    EXPECT_EQ(u.path(), "/api/v1/resource");
    EXPECT_EQ(u.encoded_queries(), "name=qb+framework&dup=1&dup=2&empty=&encoded=%7Bok%7D&&flag");
    EXPECT_EQ(u.query("name"), "qb framework");
    EXPECT_EQ(u.query("dup", 0), "1");
    EXPECT_EQ(u.query("dup", 1), "2");
    EXPECT_EQ(u.query("empty"), "");
    EXPECT_EQ(u.query("encoded"), "{ok}");
    EXPECT_EQ(u.query("flag"), "");
    EXPECT_EQ(u.query_or("missing", "fallback"), "fallback");
    EXPECT_EQ(u.fragment(), "section-2");
    EXPECT_EQ(u.af(), AF_INET6);
}

/**
 * @test Percent-encoded query KEYS decode before lookup.
 * @brief `query1%5B%5D` (= `query1[]`) is keyed under its decoded name.
 *
 * Salvaged from test-io.cpp::URI.Resolving u80 — the only case there that encoded the key, not
 * just the value. Pins that `query()` lookups go through the decoded key map.
 */
TEST(UriParse, EncodedQueryKeysDecodeBeforeLookup) {
    const uri u("https://user:password@[::1]:8080/section1/section2/action"
                "?query1%5B%5D=value1&query2%5B%5D=value2#fragment");
    ASSERT_TRUE(u.is_valid());
    EXPECT_EQ(u.user_info(), "user:password");
    EXPECT_EQ(u.host(), "::1");
    EXPECT_EQ(u.u_port(), 8080u);
    EXPECT_EQ(u.query("query1[]"), "value1");
    EXPECT_EQ(u.query("query2[]"), "value2");
    EXPECT_EQ(u.fragment(), "fragment");
    EXPECT_EQ(u.af(), AF_INET6);
}

// =============================================================================
// URI.Resolving permutation matrix — host family × explicit/default port × userinfo
//
// test-io.cpp::URI.Resolving hand-unrolled ~22 near-identical blocks. They collapse to one
// parametrized table that asserts the exact scheme/host/port/path/query/AF for each shape.
// =============================================================================

namespace {

struct ResolveCase {
    const char   *source;
    const char   *scheme;
    const char   *user_info; // "" when absent
    const char   *host;
    std::uint16_t port;
    int           af;
};

class UriResolveMatrix : public ::testing::TestWithParam<ResolveCase> {};

} // namespace

TEST_P(UriResolveMatrix, ParsesEachAuthorityShape) {
    const auto &c = GetParam();
    const uri   u(c.source);

    ASSERT_TRUE(u.is_valid()) << c.source;
    EXPECT_EQ(u.scheme(), c.scheme);
    EXPECT_EQ(u.user_info(), c.user_info);
    EXPECT_EQ(u.host(), c.host);
    EXPECT_EQ(u.u_port(), c.port);
    EXPECT_EQ(u.path(), "/section1/section2/action");
    EXPECT_EQ(u.query("query1"), "value1");
    EXPECT_EQ(u.query("query2"), "value2");
    EXPECT_EQ(u.af(), c.af);
}

INSTANTIATE_TEST_SUITE_P(
    Resolving, UriResolveMatrix,
    ::testing::Values(
        // host = DNS name, default vs explicit port, no userinfo
        ResolveCase{
            "https://www.example.com/section1/section2/action?query1=value1&query2=value2", "https", "", "www.example.com", 443, AF_INET
        },
        ResolveCase{"hTtP://www.example.com/section1/section2/action?query1=value1&query2=value2", "hTtP", "", "www.example.com", 80, AF_INET},
        ResolveCase{
            "HtTpS://www.example.com/section1/section2/action?query1=value1&query2=value2", "HtTpS", "", "www.example.com", 443, AF_INET
        },
        ResolveCase{
            "https://www.example.com:8080/section1/section2/action?query1=value1&query2=value2", "https", "", "www.example.com", 8080, AF_INET
        },
        ResolveCase{
            "HtTpS://www.example.com:9443/section1/section2/action?query1=value1&query2=value2", "HtTpS", "", "www.example.com", 9443, AF_INET
        },
        ResolveCase{"https://localhost/section1/section2/action?query1=value1&query2=value2", "https", "", "localhost", 443, AF_INET},
        ResolveCase{"https://localhost:8080/section1/section2/action?query1=value1&query2=value2", "https", "", "localhost", 8080, AF_INET},
        // host = IPv4 literal
        ResolveCase{"https://127.0.0.1/section1/section2/action?query1=value1&query2=value2", "https", "", "127.0.0.1", 443, AF_INET},
        ResolveCase{"https://127.0.0.1:8080/section1/section2/action?query1=value1&query2=value2", "https", "", "127.0.0.1", 8080, AF_INET},
        // host = IPv6 literal (bracketed) → AF_INET6
        ResolveCase{"https://[::1]/section1/section2/action?query1=value1&query2=value2", "https", "", "::1", 443, AF_INET6},
        ResolveCase{"https://[::1]:8080/section1/section2/action?query1=value1&query2=value2", "https", "", "::1", 8080, AF_INET6},
        // with userinfo across each host family
        ResolveCase{
            "https://user:password@www.example.com/section1/section2/action?query1=value1&query2=value2", "https", "user:password",
            "www.example.com", 443, AF_INET
        },
        ResolveCase{
            "https://user:password@www.example.com:8080/section1/section2/action?query1=value1&query2=value2", "https", "user:password",
            "www.example.com", 8080, AF_INET
        },
        ResolveCase{
            "https://user:password@localhost:8080/section1/section2/action?query1=value1&query2=value2", "https", "user:password", "localhost",
            8080, AF_INET
        },
        ResolveCase{
            "https://user:password@127.0.0.1/section1/section2/action?query1=value1&query2=value2", "https", "user:password", "127.0.0.1", 443,
            AF_INET
        },
        ResolveCase{
            "https://user:password@[::1]:8080/section1/section2/action?query1=value1&query2=value2", "https", "user:password", "::1", 8080,
            AF_INET6
        }));

TEST(UriParse, DefaultPortLookupPreservesSourceSchemeAndRejectsUnknownDefault) {
    const uri mixed_http("hTtP://example.com/resource");
    const uri mixed_https("HtTpS://example.com/resource");
    const uri lowercase_http("http://example.com/resource");
    const uri lowercase_https("https://example.com/resource");
    const uri long_known("PrOmEtHeUs-AlErTmAnAgEr://example.com/resource");
    const uri unknown("x-custom://example.com/resource");

    ASSERT_TRUE(mixed_http.is_valid());
    ASSERT_TRUE(mixed_https.is_valid());
    EXPECT_EQ(mixed_http.source(), "hTtP://example.com/resource");
    EXPECT_EQ(mixed_https.source(), "HtTpS://example.com/resource");
    EXPECT_EQ(mixed_http.scheme(), "hTtP");
    EXPECT_EQ(mixed_https.scheme(), "HtTpS");
    EXPECT_EQ(mixed_http.port(), lowercase_http.port());
    EXPECT_EQ(mixed_https.port(), lowercase_https.port());
    EXPECT_EQ(mixed_http.u_port(), 80u);
    EXPECT_EQ(mixed_https.u_port(), 443u);
    EXPECT_EQ(long_known.u_port(), 9093u); // longest current default-port scheme also fits the stack key
    EXPECT_TRUE(unknown.is_valid());       // syntactically valid; no default service is registered
    EXPECT_TRUE(unknown.port().empty());
    EXPECT_EQ(unknown.u_port(), 0u); // caller must supply a port for an unknown scheme

    for (const std::size_t size : {64u, 65u, 80u}) {
        std::string long_scheme(size, 'a');
        long_scheme[0]                = 'A';
        const std::string long_source = long_scheme + "://example.com/resource";
        const uri         long_unknown(long_source);
        ASSERT_TRUE(long_unknown.is_valid()) << size;
        EXPECT_EQ(long_unknown.source(), long_source);
        EXPECT_EQ(long_unknown.scheme(), long_scheme);
        EXPECT_EQ(long_unknown.host(), "example.com");
        EXPECT_EQ(long_unknown.path(), "/resource");
        EXPECT_TRUE(long_unknown.port().empty());
        EXPECT_EQ(long_unknown.u_port(), 0u);

        const std::string explicit_source = long_scheme + "://example.com:8443/resource";
        const uri         explicit_port(explicit_source);
        ASSERT_TRUE(explicit_port.is_valid()) << size;
        EXPECT_EQ(explicit_port.source(), explicit_source);
        EXPECT_EQ(explicit_port.scheme(), long_scheme);
        EXPECT_EQ(explicit_port.u_port(), 8443u);
    }
}

// =============================================================================
// RELATIVE / UNIX / DEFAULT / COPY-MOVE / PORT CONTRACTS
// =============================================================================

/**
 * @test Default, empty, relative and unix sources, plus copy/move and port edge contracts.
 * @brief Folded from test-uri-json.cpp::HandlesRelativeUnixCopyMoveAndPortContracts.
 *
 * A default-constructed uri is valid with an empty path; an empty *source* normalizes to "/".
 * A relative reference keeps its dotted path verbatim (no scheme/host). `unix://` yields AF_UNIX
 * and port "0". Copy and move both preserve the parsed components. An overflowing or non-numeric
 * port is reported verbatim in `port()` but rejected to 0 by `u_port()`.
 */
TEST(UriParse, RelativeUnixCopyMoveAndPortContracts) {
    const uri default_constructed;
    EXPECT_TRUE(default_constructed.is_valid());
    EXPECT_TRUE(default_constructed.path().empty());

    const uri empty_source(std::string{});
    EXPECT_TRUE(empty_source.is_valid());
    EXPECT_EQ(empty_source.path(), "/");

    const uri relative("assets/../images/logo.png?mode=dark");
    ASSERT_TRUE(relative.is_valid());
    EXPECT_TRUE(relative.scheme().empty());
    EXPECT_TRUE(relative.host().empty());
    EXPECT_EQ(relative.path(), "assets/../images/logo.png") << "a relative reference keeps its raw dotted path";
    EXPECT_EQ(relative.query("mode"), "dark");

    const uri unix_uri("unix:///tmp/qb.sock");
    ASSERT_TRUE(unix_uri.is_valid());
    EXPECT_EQ(unix_uri.scheme(), "unix");
    EXPECT_EQ(unix_uri.af(), AF_UNIX);
    EXPECT_EQ(unix_uri.path(), "/tmp/qb.sock");
    EXPECT_EQ(unix_uri.port(), "0");

    uri assigned;
    assigned = std::string("tcp://127.0.0.1:4242");
    EXPECT_TRUE(assigned.is_valid());
    EXPECT_EQ(assigned.host(), "127.0.0.1");
    EXPECT_EQ(assigned.u_port(), 4242u);

    const uri copied(assigned);
    EXPECT_EQ(copied.source(), assigned.source());
    EXPECT_EQ(copied.host(), "127.0.0.1");

    uri       to_move(assigned);
    const uri moved(std::move(to_move));
    EXPECT_EQ(moved.host(), "127.0.0.1");
    EXPECT_EQ(moved.u_port(), 4242u);

    const uri overflowing_port("tcp://localhost:99999");
    EXPECT_TRUE(overflowing_port.is_valid());
    EXPECT_EQ(overflowing_port.port(), "99999");
    EXPECT_EQ(overflowing_port.u_port(), 0u) << "out-of-range port must reject, not wrap";

    const uri non_numeric("tcp://example.com:notaport");
    EXPECT_TRUE(non_numeric.is_valid());
    EXPECT_EQ(non_numeric.host(), "example.com:notaport") << "a non-numeric colon-suffix stays part of the host";
    EXPECT_EQ(non_numeric.port(), "0");
    EXPECT_EQ(non_numeric.u_port(), 0u);
}

// =============================================================================
// VALIDITY — structural rejection the constructor swallows
// =============================================================================

/**
 * @test `is_valid()` exposes the parse failures the constructor never throws on.
 * @brief Folds test-uri-json.cpp::RejectsMalformedAuthorityAndPath + test-io URIRobustness.IsValidReportsParseFailures.
 */
TEST(UriParse, IsValidReportsStructuralFailures) {
    // Well-formed inputs are valid.
    EXPECT_TRUE(uri("https://host:8080/path?a=b").is_valid());
    EXPECT_TRUE(uri("unix://name.sock/svc").is_valid());
    EXPECT_TRUE(uri("").is_valid()); // empty → path "/", still valid

    // Malformed authority / path / port → invalid.
    EXPECT_FALSE(uri("http://exa mple.com").is_valid());
    EXPECT_FALSE(uri("http://[2001:db8::1").is_valid()) << "unclosed IPv6 bracket";
    EXPECT_FALSE(uri("http://[2001:db8::1]:bad").is_valid());
    EXPECT_FALSE(uri("http://example.com/path with spaces").is_valid());
    EXPECT_FALSE(uri(std::string("http://host/pa\x01th")).is_valid()) << "control char in path";

    // Validity is recomputed on assignment — no stale flag.
    uri u("http://[::1/bad");
    EXPECT_FALSE(u.is_valid());
    u = std::string("http://good/path");
    EXPECT_TRUE(u.is_valid());
}

// =============================================================================
// RE-PARSE / AF PRESERVATION REGRESSIONS (test-io.cpp URIRegression.*)
// =============================================================================

/**
 * @test A trailing or truncated `%` is preserved by the string_view decode (no over-read).
 * @brief Regression: `uri::decode` used to read past the end on a dangling escape.
 */
TEST(UriParse, DecodeTrailingPercentIsPreserved) {
    EXPECT_EQ(uri::decode(std::string_view("hello%")), "hello%");
    EXPECT_EQ(uri::decode(std::string_view("hello%2")), "hello%2");
    EXPECT_EQ(uri::decode(std::string_view("%20end%")), " end%");
    EXPECT_EQ(uri::decode(std::string_view("")), "");
    EXPECT_EQ(uri::decode(std::string_view("%")), "%");
}

/**
 * @test Re-assigning a URI clears every stale string_view component.
 * @brief Regression: `parse()` did not reset members, so a sparse re-parse leaked old views.
 */
TEST(UriParse, ReassignClearsStaleComponents) {
    uri u("https://user:pass@host.com:9090/path?q=v#frag");
    EXPECT_EQ(u.scheme(), "https");
    EXPECT_EQ(u.user_info(), "user:pass");
    EXPECT_EQ(u.host(), "host.com");
    EXPECT_EQ(u.u_port(), 9090u);
    EXPECT_EQ(u.fragment(), "frag");

    u = std::string("http://minimal.com");
    EXPECT_EQ(u.scheme(), "http");
    EXPECT_EQ(u.host(), "minimal.com");
    EXPECT_EQ(u.u_port(), 80u);
    EXPECT_EQ(u.path(), "/");
    EXPECT_TRUE(u.user_info().empty());
    EXPECT_TRUE(u.fragment().empty());
    EXPECT_TRUE(u.queries().empty());

    u = std::string("");
    EXPECT_TRUE(u.scheme().empty());
    EXPECT_TRUE(u.host().empty());
    EXPECT_TRUE(u.fragment().empty());
    EXPECT_EQ(u.path(), "/");
}

/**
 * @test Copy and move (both ctor and assignment) preserve the explicitly-inferred address family.
 * @brief Regression: assignment dropped `_af`, so an IPv6/unix URI silently reverted to AF_INET.
 */
TEST(UriParse, CopyAndMovePreserveAddressFamily) {
    // Copy-assign: IPv6.
    const uri ipv6_src("tcp://[::1]:5000/path");
    ASSERT_EQ(ipv6_src.af(), AF_INET6);
    uri copy_assigned;
    copy_assigned = ipv6_src;
    EXPECT_EQ(copy_assigned.af(), AF_INET6);
    EXPECT_EQ(copy_assigned.host(), "::1");

    // Copy-construct: IPv6.
    const uri copy_constructed(ipv6_src);
    EXPECT_EQ(copy_constructed.af(), AF_INET6);
    EXPECT_EQ(copy_constructed.u_port(), 5000u);

    // Move-assign: unix.
    uri unix_src("unix:///var/run/app.sock");
    ASSERT_EQ(unix_src.af(), AF_UNIX);
    uri move_assigned;
    move_assigned = std::move(unix_src);
    EXPECT_EQ(move_assigned.af(), AF_UNIX);
    EXPECT_EQ(move_assigned.scheme(), "unix");

    // Move-construct: unix.
    uri unix_src2("unix://my.sock/service");
    ASSERT_EQ(unix_src2.af(), AF_UNIX);
    const uri move_constructed(std::move(unix_src2));
    EXPECT_EQ(move_constructed.af(), AF_UNIX);
    EXPECT_EQ(move_constructed.scheme(), "unix");
}

// =============================================================================
// QUERY-MAP PARSING EDGES (test-io.cpp URIRobustness.*)
// =============================================================================

TEST(UriParse, QueryMapHandlesEncodedAmpersandEmptyAndKeyOnly) {
    // A `%26` in a value is decoded to '&' AFTER the parser splits on raw '&'.
    const uri encoded_amp("http://host/p?key=val%26ue&k2=v2");
    EXPECT_EQ(encoded_amp.query("key"), "val&ue");
    EXPECT_EQ(encoded_amp.query("k2"), "v2");

    // Empty values.
    const uri empties("http://host/p?a=&b=&c=");
    EXPECT_EQ(empties.query("a"), "");
    EXPECT_EQ(empties.query("b"), "");
    EXPECT_EQ(empties.query("c"), "");

    // Bare flags (key only, no '=').
    const uri flags("http://host/p?flagA&flagB&key=val");
    EXPECT_EQ(flags.query("flagA"), "");
    EXPECT_EQ(flags.query("flagB"), "");
    EXPECT_EQ(flags.query("key"), "val");
}

TEST(UriParse, LongQueryStringParsesEveryPair) {
    std::string long_query;
    for (int i = 0; i < 200; ++i) {
        if (i > 0)
            long_query += "&";
        long_query += "key" + std::to_string(i) + "=value" + std::to_string(i);
    }
    const uri u("http://host/path?" + long_query);
    EXPECT_EQ(u.query("key0"), "value0");
    EXPECT_EQ(u.query("key99"), "value99");
    EXPECT_EQ(u.query("key199"), "value199");
}

/**
 * @test `u_port()` rejects out-of-range / overflowing all-digit ports instead of truncating.
 * @brief Regression: `static_cast<uint16_t>(99999) == 34463` — a silent wrap. Now → 0.
 */
TEST(UriParse, PortRejectsOutOfRangeTruncation) {
    EXPECT_EQ(uri("http://host:8080/").u_port(), 8080u);
    EXPECT_EQ(uri("http://host:65535/").u_port(), 65535u);

    EXPECT_EQ(uri("http://host:65536/").u_port(), 0u);
    EXPECT_EQ(uri("http://host:99999/").u_port(), 0u);
    EXPECT_EQ(uri("http://host:4294967296/").u_port(), 0u) << "above UINT32_MAX must still reject";
    EXPECT_EQ(uri("http://host:99999999999999999999/").u_port(), 0u) << "from_chars out_of_range path";
}

// =============================================================================
// STATIC ENCODE / DECODE / VALIDATE
// =============================================================================

/**
 * @test The two decode surfaces have a deliberately different dangling-escape policy.
 * @brief Iterator `decode(begin,end)` STOPS at a truncated `%` (drops it); `decode(string_view)`
 *        PRESERVES it. Both must round-trip valid escapes identically. Folds the divergent
 *        URIRobustness decode cases into one explicit contrast.
 */
TEST(UriParse, DecodeIteratorDropsTruncatedEscapeStringViewKeepsIt) {
    // Iterator form: truncated escape stops the scan, the '%' is dropped.
    const std::string abc_pct = "abc%";
    EXPECT_EQ(uri::decode(abc_pct.begin(), abc_pct.end()), "abc");
    const std::string lone_pct = "%";
    EXPECT_EQ(uri::decode(lone_pct.begin(), lone_pct.end()), "");
    const std::string test_pct2 = "test%2";
    EXPECT_EQ(uri::decode(test_pct2.begin(), test_pct2.end()), "test");
    const std::string mid_bad = "ok%ZZtail";
    EXPECT_EQ(uri::decode(mid_bad.begin(), mid_bad.end()), "ok") << "a bad hex pair also stops the iterator scan";

    // string_view form: truncated escape is preserved verbatim.
    EXPECT_EQ(uri::decode(std::string_view("abc%")), "abc%");
    EXPECT_EQ(uri::decode(std::string_view("test%2")), "test%2");
    EXPECT_EQ(uri::decode(std::string_view("%20ok%")), " ok%");
    EXPECT_EQ(uri::decode(std::string_view("%ZZ%")), "%ZZ%");

    // Valid escapes decode identically through both surfaces.
    const std::string hello = "%48%65%6C%6C%6F";
    EXPECT_EQ(uri::decode(hello.begin(), hello.end()), "Hello");
    EXPECT_EQ(uri::decode(std::string_view("hello+world%21")), "hello world!");
}

TEST(UriParse, EncodeMapsReservedAndPlusFormsAndNullPointersAreEmpty) {
    EXPECT_EQ(uri::encode(std::string_view("hello world")), "hello+world");
    EXPECT_EQ(uri::encode(std::string_view("100% ready")), "100%25+ready");

    // Null-pointer / size sources are safe and empty (no deref).
    EXPECT_TRUE(uri::decode(nullptr, 4).empty());
    EXPECT_TRUE(uri::encode(nullptr, 4).empty());

    // Raw-byte (UTF-8) round-trip through the pointer+size surface.
    const std::string bytes("\xC3\xA9", 2);
    EXPECT_EQ(uri::encode(bytes.data(), bytes.size()), "%C3%A9");
    EXPECT_EQ(uri::decode("%C3%A9", 6), bytes);
}

/**
 * @test encode→decode is loss-free across a representative corpus.
 * @brief Parametrized from URIRobustness.EncodeDecodeRoundtrip.
 */
class UriRoundtrip : public ::testing::TestWithParam<std::string> {};

TEST_P(UriRoundtrip, EncodeThenDecodeRecoversOriginal) {
    const std::string &original = GetParam();
    EXPECT_EQ(uri::decode(uri::encode(original)), original);
}

INSTANTIATE_TEST_SUITE_P(Corpus, UriRoundtrip,
                         ::testing::Values(std::string("simple text"), std::string("special: !@#$%^&*()"), std::string(""),
                                           std::string("unicode: \xC3\xA9\xC3\xA0\xC3\xBC"), std::string("slashes/and?query=yes&more=true"),
                                           std::string(1000, 'X'), std::string("trailing%"), std::string("%already%20encoded")));

TEST(UriParse, SchemeAndHostValidators) {
    EXPECT_TRUE(uri::is_valid_scheme("h2+tls"));
    EXPECT_TRUE(uri::is_valid_scheme("custom.v1-transport"));
    EXPECT_FALSE(uri::is_valid_scheme("")) << "scheme must be non-empty";
    EXPECT_FALSE(uri::is_valid_scheme("1http")) << "scheme must start with a letter";
    EXPECT_FALSE(uri::is_valid_scheme("bad_scheme")) << "'_' is not a scheme character";

    EXPECT_TRUE(uri::is_valid_host("example.com"));
    EXPECT_TRUE(uri::is_valid_host("[::1]"));
    EXPECT_FALSE(uri::is_valid_host(""));
    EXPECT_FALSE(uri::is_valid_host("bad host"));
}

// =============================================================================
// PATH NORMALIZATION — `.`/`..` resolution, slash standardization (spec D4 additions)
// =============================================================================

namespace {

struct NormalizeCase {
    const char *input;
    const char *expected;
};

class UriNormalize : public ::testing::TestWithParam<NormalizeCase> {};

} // namespace

TEST_P(UriNormalize, ResolvesDotSegmentsAndSlashes) {
    const auto &c    = GetParam();
    std::string path = c.input;
    EXPECT_TRUE(uri::normalize_path(path)) << c.input;
    EXPECT_EQ(path, c.expected) << "normalize_path(\"" << c.input << "\")";
}

INSTANTIATE_TEST_SUITE_P(DotSegments, UriNormalize,
                         ::testing::Values(
                             // empty / dot → root
                             NormalizeCase{"", "/"}, NormalizeCase{".", "/"},
                             // absolute path: `..` pops, `.` and `//` collapse, trailing slash stripped
                             NormalizeCase{"/a/b/../c/./d//", "/a/c/d"},
                             // relative path: leading `..` are retained (cannot escape above the relative root)
                             NormalizeCase{"../a/./b/../../c", "../c"},
                             // spec D4: a `/../`-escape attempt at the absolute root is clamped (no traversal above /)
                             NormalizeCase{"/../../../etc/passwd", "/etc/passwd"}, NormalizeCase{"/a/../../b", "/b"},
                             // spec D4: Windows backslashes are standardized to '/', and a drive-letter-style
                             // path normalizes its separators + dot segments like any other.
                             NormalizeCase{"a\\b\\..\\c", "a/c"}, NormalizeCase{"C:\\Users\\.\\qb\\..\\io", "C:/Users/io"}));

// =============================================================================
// RFC 3986 CHARACTER CLASSIFIERS — the free `qb::io::is_*` predicates
//
// uri.h exposes a family of inline classifier predicates (is_gen_delim, is_sub_delim,
// is_reserved, is_user_info_character, is_query_character, is_fragment_character). The
// parse / encode paths only graze them, so each grammar branch is driven directly here
// with the exact characters from the relevant RFC 3986 production.
// =============================================================================

/**
 * @test `is_gen_delim` accepts exactly the RFC 3986 gen-delims and rejects everything else.
 * @brief gen-delims = `: / ? # [ ] @`. Drives uri.h is_gen_delim for both the true and the
 *        default-false arm.
 */
TEST(UriClassifiers, GenDelimMatchesRfc3986Set) {
    for (char c : std::string(":/?#[]@"))
        EXPECT_TRUE(qb::io::is_gen_delim(c)) << "gen-delim: " << c;

    // A representative spread of NON gen-delims (alnum, unreserved, sub-delims, controls).
    for (char c : std::string("aZ0-._~!$&'()*+,;= "))
        EXPECT_FALSE(qb::io::is_gen_delim(c)) << "not a gen-delim: " << c;
    EXPECT_FALSE(qb::io::is_gen_delim('\0'));
    EXPECT_FALSE(qb::io::is_gen_delim('\x01'));
}

/**
 * @test `is_sub_delim` accepts exactly the RFC 3986 sub-delims, one switch arm per character.
 * @brief sub-delims = `! $ & ' ( ) * + , ; =`. Every `case` label in the predicate's switch is
 *        hit, plus the default-false branch.
 */
TEST(UriClassifiers, SubDelimMatchesEverySwitchArm) {
    const std::string sub_delims = "!$&'()*+,;=";
    for (char c : sub_delims)
        EXPECT_TRUE(qb::io::is_sub_delim(c)) << "sub-delim: " << c;
    EXPECT_EQ(sub_delims.size(), 11u) << "RFC 3986 defines exactly 11 sub-delims";

    // gen-delims and unreserved are NOT sub-delims (default arm).
    for (char c : std::string(":/?#[]@aZ0-._~"))
        EXPECT_FALSE(qb::io::is_sub_delim(c)) << "not a sub-delim: " << c;
    EXPECT_FALSE(qb::io::is_sub_delim(' '));
    EXPECT_FALSE(qb::io::is_sub_delim('%'));
}

/**
 * @test `is_reserved` is the union of gen-delims and sub-delims, and excludes unreserved.
 * @brief Pins both legs of the `is_gen_delim(c) || is_sub_delim(c)` disjunction: a pure gen-delim,
 *        a pure sub-delim, and an unreserved char that satisfies neither.
 */
TEST(UriClassifiers, ReservedIsUnionOfGenAndSubDelims) {
    EXPECT_TRUE(qb::io::is_reserved('/')) << "gen-delim leg";
    EXPECT_TRUE(qb::io::is_reserved('@'));
    EXPECT_TRUE(qb::io::is_reserved('!')) << "sub-delim leg";
    EXPECT_TRUE(qb::io::is_reserved(';'));

    // unreserved characters are never reserved.
    for (char c : std::string("aZ0-._~"))
        EXPECT_FALSE(qb::io::is_reserved(c)) << "unreserved is not reserved: " << c;
    EXPECT_FALSE(qb::io::is_reserved(' '));
}

/**
 * @test `is_user_info_character` admits unreserved + sub-delims + `%` + `:`, and rejects others.
 * @brief userinfo = *( unreserved / pct-encoded / sub-delims / ":" ). Drives the `%`/`:` extras
 *        and a gen-delim rejection that distinguishes userinfo from authority.
 */
TEST(UriClassifiers, UserInfoCharacterSet) {
    EXPECT_TRUE(qb::io::is_user_info_character('a')); // unreserved
    EXPECT_TRUE(qb::io::is_user_info_character('~'));
    EXPECT_TRUE(qb::io::is_user_info_character('!')); // sub-delim
    EXPECT_TRUE(qb::io::is_user_info_character('%')); // pct-encoded marker
    EXPECT_TRUE(qb::io::is_user_info_character(':')); // user:pass separator

    EXPECT_FALSE(qb::io::is_user_info_character('@')) << "'@' terminates userinfo";
    EXPECT_FALSE(qb::io::is_user_info_character('/'));
    EXPECT_FALSE(qb::io::is_user_info_character('['));
    EXPECT_FALSE(qb::io::is_user_info_character(' '));
}

/**
 * @test `is_query_character` extends path characters with `?`, and `is_fragment_character` mirrors it.
 * @brief query = *( pchar / "/" / "?" ); fragment shares the exact same legal set. The two
 *        predicates must agree character-for-character, and both must admit the extra `?`.
 */
TEST(UriClassifiers, QueryAndFragmentShareTheSameSetPlusQuestionMark) {
    EXPECT_TRUE(qb::io::is_query_character('?')) << "'?' is legal inside a query";
    EXPECT_TRUE(qb::io::is_query_character('/'));
    EXPECT_TRUE(qb::io::is_query_character('a'));
    EXPECT_TRUE(qb::io::is_query_character('@'));
    EXPECT_TRUE(qb::io::is_query_character(':'));
    EXPECT_FALSE(qb::io::is_query_character('#')) << "'#' starts the fragment, ends the query";
    EXPECT_FALSE(qb::io::is_query_character(' '));

    // fragment is defined as identical to query — assert byte-for-byte agreement.
    for (int c = 0; c < 256; ++c)
        EXPECT_EQ(qb::io::is_fragment_character(c), qb::io::is_query_character(c)) << "fragment vs query divergence at byte " << c;
    EXPECT_TRUE(qb::io::is_fragment_character('?'));
    EXPECT_FALSE(qb::io::is_fragment_character('#'));
}

// =============================================================================
// QUERY MAP ACCESSORS — const queries(), query() miss, query_or() hit
// =============================================================================

/**
 * @test The const `queries()` accessor exposes the fully-decoded multi-value map.
 * @brief Distinct from `encoded_queries()` (raw string): this returns the parsed
 *        icase_unordered_map<vector<string>>, so duplicate keys collapse into a value vector and
 *        percent-escapes are already decoded. Pins the const overload (uri.h queries() const).
 */
TEST(UriParse, ConstQueriesMapExposesDecodedMultiValues) {
    const uri   u("http://host/p?dup=1&dup=2&dup=3&solo=%7Bx%7D");
    const auto &qmap = u.queries();

    ASSERT_TRUE(qmap.has("dup"));
    ASSERT_TRUE(qmap.has("solo"));
    EXPECT_EQ(qmap.size(), 2u) << "duplicate keys collapse into one entry";

    const auto dup_it = qmap.find("dup");
    ASSERT_NE(dup_it, qmap.cend());
    const auto &dup_values = dup_it->second;
    ASSERT_EQ(dup_values.size(), 3u);
    EXPECT_EQ(dup_values[0], "1");
    EXPECT_EQ(dup_values[1], "2");
    EXPECT_EQ(dup_values[2], "3");

    const auto solo_it = qmap.find("solo");
    ASSERT_NE(solo_it, qmap.cend());
    EXPECT_EQ(solo_it->second.front(), "{x}") << "values are percent-decoded in the map";
}

/**
 * @test `query()` returns the shared empty string on every miss kind; `query_or()` returns its hit.
 * @brief Covers the two fall-through paths the happy-path tests skip:
 *        - query() with an absent key, a present key but out-of-range index → the static empty ref;
 *        - query_or() with a present key + in-range index → the stored value (not the fallback).
 */
TEST(UriParse, QueryMissReturnsEmptyAndQueryOrHitReturnsStoredValue) {
    const uri u("http://host/p?present=A&multi=x&multi=y");

    // query() misses → empty string reference (uri.h query() fall-through).
    EXPECT_EQ(u.query("absent"), "") << "absent key → empty";
    EXPECT_EQ(u.query("present", 1), "") << "present key, index past end → empty";
    EXPECT_EQ(u.query("multi", 5), "") << "index well beyond size → empty";

    // The empty results are a real, stable reference (never a dangling temporary).
    const std::string &miss = u.query("absent");
    EXPECT_TRUE(miss.empty());

    // query_or() HIT path: present key, in-range index → stored value, fallback ignored.
    EXPECT_EQ(u.query_or("present", "fb"), "A");
    EXPECT_EQ(u.query_or("multi", "fb", 0), "x");
    EXPECT_EQ(u.query_or("multi", "fb", 1), "y") << "in-range index returns the stored value";

    // query_or() MISS paths return the fallback by value.
    EXPECT_EQ(u.query_or("absent", "fb"), "fb");
    EXPECT_EQ(u.query_or("present", "fb", 9), "fb") << "out-of-range index → fallback";
}

/**
 * @test The non-const `queries()` accessor returns a mutable map an caller can edit in place.
 * @brief Pins the mutable overload (uri.h queries() non-const): editing the returned reference is
 *        observed by a subsequent `query()` lookup against the same object.
 */
TEST(UriParse, MutableQueriesAccessorAllowsInPlaceEdit) {
    uri u("http://host/p?k=orig");
    EXPECT_EQ(u.query("k"), "orig");

    u.queries()["k"]        = {"edited"};
    u.queries()["injected"] = {"new1", "new2"};

    EXPECT_EQ(u.query("k"), "edited");
    EXPECT_EQ(u.query("injected", 0), "new1");
    EXPECT_EQ(u.query("injected", 1), "new2");
}

// =============================================================================
// IDN / PUNYCODE host (spec D4 addition)
// =============================================================================

/**
 * @test An already-punycoded IDN host label is carried through the byte-level parser verbatim.
 * @brief `qb::io::uri` does NOT transcode Unicode → ACE; callers pass the `xn--` form. This pins
 *        that a host containing a punycode label parses as a normal DNS name (AF_INET, scheme/
 *        path/query intact) and is not mangled or rejected.
 */
TEST(UriParse, PunycodeIdnHostIsCarriedVerbatim) {
    // "münchen.de" in ACE form. The parser must keep the xn-- label exactly.
    const uri u("https://xn--mnchen-3ya.de:8443/pfad?lang=de");
    ASSERT_TRUE(u.is_valid());
    EXPECT_EQ(u.scheme(), "https");
    EXPECT_EQ(u.host(), "xn--mnchen-3ya.de");
    EXPECT_EQ(u.u_port(), 8443u);
    EXPECT_EQ(u.path(), "/pfad");
    EXPECT_EQ(u.query("lang"), "de");
    EXPECT_EQ(u.af(), AF_INET) << "a punycoded DNS host is an ordinary IPv4-family authority";
}

// =============================================================================
// RE-ASSIGNED TEXT, COPIED VALUE, BRACKETED AUTHORITY (Huly QB-350, QB-351, QB-352)
// =============================================================================

/**
 * @test Assigning new text to a uri classifies its family from that text, exactly as a fresh uri of it does.
 * @brief Huly QB-350: the classifier read the family the previous text left (`_af`) and never left AF_UNIX, so a
 *        uri that once held `unix://...` kept routing to a socket path after `= "tcp://127.0.0.1:4242"`.
 */
TEST(UriParse, AssignedTextIsClassifiedLikeAFreshUriOfIt) {
    for (const char *text : {"tcp://127.0.0.1:4242/p?a=1", "tcp://[::1]:4242/p", "tcp://example.com:4242/p", "/relative/only"}) {
        const uri fresh(text);

        uri moved_in("unix:///tmp/qb.sock");
        ASSERT_EQ(moved_in.af(), AF_UNIX);
        moved_in = std::string(text); // operator=(std::string &&)

        uri               copied_in("unix:///tmp/qb.sock");
        const std::string source(text);
        copied_in = source; // operator=(std::string const &)

        for (const uri *u : {&moved_in, &copied_in}) {
            EXPECT_EQ(u->af(), fresh.af()) << text << ": the family of the previous text survived the assignment";
            EXPECT_EQ(u->scheme(), fresh.scheme()) << text;
            EXPECT_EQ(u->host(), fresh.host()) << text;
            EXPECT_EQ(u->port(), fresh.port()) << text;
            EXPECT_EQ(u->path(), fresh.path()) << text;
            EXPECT_EQ(u->query("a"), fresh.query("a")) << text;
        }
    }

    // The other way round, and between the two IP families: the text decides every time.
    uri u("tcp://[::1]:80/");
    ASSERT_EQ(u.af(), AF_INET6);
    u = std::string("unix:///var/run/app.sock");
    EXPECT_EQ(u.af(), AF_UNIX);
    EXPECT_EQ(u.path(), "/var/run/app.sock");
    u = std::string("tcp://10.0.0.1:80/");
    EXPECT_EQ(u.af(), AF_INET);
    u = std::string("tcp://[::1]:80/");
    EXPECT_EQ(u.af(), AF_INET6);

    // A family given to the CONSTRUCTOR is the caller's hint for text that names none; the assignment of a new text
    // drops it, as constructing a uri from that text does.
    uri hinted("/tmp/x.sock", AF_UNIX);
    ASSERT_EQ(hinted.af(), AF_UNIX);
    hinted = std::string("tcp://127.0.0.1:80/");
    EXPECT_EQ(hinted.af(), uri("tcp://127.0.0.1:80/").af());
}

/**
 * @test Copies and moves of a uri carry the query map as it is, edits included, and never point into the source.
 * @brief Huly QB-351: `queries()` hands out the map mutably (the qbm-http validator edits it in place), and the copy and
 *        move operations re-parsed the source TEXT -- every edit (replaced, removed, added parameter) was lost.
 */
TEST(UriParse, CopiesAndMovesKeepTheEditedQueryMap) {
    const auto edited = [] {
        uri u("http://user@host.example:8081/a/b?k=orig&drop=1&multi=x#frag");
        u.queries()["k"] = {"edited"};
        u.queries().erase("drop");
        u.queries()["added"] = {"a1", "a2"};
        return u;
    };
    const auto expect_edited = [](uri const &u, const char *how) {
        EXPECT_EQ(u.query("k"), "edited") << how << ": a replaced value came back";
        EXPECT_FALSE(u.queries().has("drop")) << how << ": a removed parameter came back";
        EXPECT_EQ(u.query("added", 1), "a2") << how << ": an added parameter was lost";
        EXPECT_EQ(u.query("multi"), "x") << how;
        EXPECT_EQ(u.scheme(), "http") << how;
        EXPECT_EQ(u.user_info(), "user") << how;
        EXPECT_EQ(u.host(), "host.example") << how;
        EXPECT_EQ(u.u_port(), 8081u) << how;
        EXPECT_EQ(u.path(), "/a/b") << how;
        EXPECT_EQ(u.fragment(), "frag") << how;
        EXPECT_TRUE(u.is_valid()) << how;
        // The views name this object's own text: none of them may outlive a source that is gone.
        const char *const begin = u.source().data();
        const char *const end   = begin + u.source().size();
        for (std::string_view const part : {u.scheme(), u.user_info(), u.host(), u.path(), u.encoded_queries(), u.fragment()})
            EXPECT_TRUE(part.data() >= begin && part.data() + part.size() <= end) << how << ": a view points outside the text";
    };

    const uri source = edited();
    expect_edited(uri(source), "copy-construct");
    uri copy_assigned("ftp://other/");
    copy_assigned = source;
    expect_edited(copy_assigned, "copy-assign");

    uri       to_move = edited();
    const uri moved(std::move(to_move));
    expect_edited(moved, "move-construct");
    uri to_move_assign = edited();
    uri move_assigned("ftp://other/");
    move_assigned = std::move(to_move_assign);
    expect_edited(move_assigned, "move-assign");

    // The source is destroyed before the copy is read: nothing may point into it.
    uri survivor;
    {
        uri scoped = edited();
        survivor   = scoped;
    }
    expect_edited(survivor, "copy of a destroyed source");
    uri moved_survivor;
    {
        uri scoped     = edited();
        moved_survivor = std::move(scoped);
    }
    expect_edited(moved_survivor, "move of a destroyed source");

    // A short text lives inside std::string's own buffer, which a move copies: the views follow the new buffer.
    uri small("h://a/b?q=1");
    small.queries()["q"] = {"2"};
    const uri small_moved(std::move(small));
    EXPECT_EQ(small_moved.host(), "a");
    EXPECT_EQ(small_moved.path(), "/b");
    EXPECT_EQ(small_moved.query("q"), "2");
    EXPECT_EQ(small_moved.source().data(), small_moved.host().data() - 4) << "the host view does not name the moved text";

    // An invalid uri copies as invalid, and a default port (static storage, not the text) survives the copy.
    const uri invalid("http://exa mple.com/");
    EXPECT_FALSE(uri(invalid).is_valid());
    const uri defaulted("https://host/");
    const uri defaulted_copy(defaulted);
    EXPECT_EQ(defaulted_copy.port(), "443");
}

/**
 * @test After a bracketed host the authority ends, or continues with `:` and a port; anything else is rejected.
 * @brief Huly QB-352: text after `]` that was not a `:port` was silently dropped, so `http://[::1]junk/` parsed as a
 *        valid uri of host `::1`. RFC 3986 3.2.2: IP-literal = "[" ... "]", followed only by [ ":" port ].
 *        An empty port is the scheme's default (RFC 3986 3.2.3 and 6.2.3), after a bracketed host and after a name
 *        alike: a name used to keep the colon (`example.com:`, classified AF_INET6 for it).
 */
TEST(UriParse, ABracketedHostEndsTheAuthorityOrIsFollowedByAPort) {
    // Valid: the host alone, with a port, up to a path / query / fragment boundary, a zone identifier.
    struct Valid {
        const char *text;
        const char *host;
        unsigned    port;
        const char *path;
    };
    for (const Valid &c :
         {Valid{"http://[::1]/p", "::1", 80, "/p"}, Valid{"http://[::1]:8080/p", "::1", 8080, "/p"}, Valid{"http://[::1]", "::1", 80, "/"},
          Valid{"http://[::1]?q=1", "::1", 80, "/"}, Valid{"http://[::1]:81#f", "::1", 81, "/"},
          Valid{"https://[fe80::1%25eth0]:8443/x", "fe80::1%25eth0", 8443, "/x"},
          Valid{"https://user@[2001:db8::1]:9/x", "2001:db8::1", 9, "/x"}}) {
        const uri u(c.text);
        ASSERT_TRUE(u.is_valid()) << c.text;
        EXPECT_EQ(u.host(), c.host) << c.text;
        EXPECT_EQ(u.u_port(), c.port) << c.text;
        EXPECT_EQ(u.path(), c.path) << c.text;
        EXPECT_EQ(u.af(), AF_INET6) << c.text;
    }

    // A suffix after the bracket that is not ":port" is not part of any authority: the uri is invalid.
    for (const char *text : {"http://[::1]junk/p", "http://[::1]junk:80/p", "http://[::1]]/p", "http://[::1][::2]/p", "http://[::1]x"})
        EXPECT_FALSE(uri(text).is_valid()) << text << ": the text after ']' was dropped instead of rejected";

    // The port itself: digits only (an out-of-range one parses, and u_port() refuses it), and the bracket must close.
    EXPECT_FALSE(uri("http://[::1]:8a/p").is_valid());
    EXPECT_FALSE(uri("http://[::1:80/p").is_valid());
    const uri out_of_range("http://[::1]:99999/");
    EXPECT_TRUE(out_of_range.is_valid());
    EXPECT_EQ(out_of_range.u_port(), 0u);

    // An empty port is the scheme's default, after a bracket and after a name.
    const uri empty_v6("https://[::1]:/p");
    ASSERT_TRUE(empty_v6.is_valid());
    EXPECT_EQ(empty_v6.host(), "::1");
    EXPECT_EQ(empty_v6.port(), "443") << "an empty port is the scheme's default";
    EXPECT_EQ(empty_v6.path(), "/p");
    const uri empty_name("http://example.com:/p");
    ASSERT_TRUE(empty_name.is_valid());
    EXPECT_EQ(empty_name.host(), "example.com") << "the ':' of an empty port is not part of the host";
    EXPECT_EQ(empty_name.u_port(), 80u);
    EXPECT_EQ(empty_name.af(), AF_INET) << "a name is not an IPv6 literal because of the ':' of an empty port";
    const uri empty_unknown("x-custom://example.com:/p");
    EXPECT_EQ(empty_unknown.host(), "example.com");
    EXPECT_TRUE(empty_unknown.port().empty()) << "no default to fall back on";
}

TEST(UriParse, EmptyQueryPresenceSurvivesOwnedValueOperations) {
    for (const std::string &text :
         {std::string{"http://h/?"}, std::string{"http://h/?#f"}, std::string{"http://host.example/"} + std::string(180, 'x') + "?"}) {
        uri copied, moved, copy_assigned, move_assigned;
        {
            uri source{text};
            copied = uri(source);
            uri to_move{source};
            moved         = uri(std::move(to_move));
            copy_assigned = source;
            uri to_assign{source};
            move_assigned = std::move(to_assign);
        }
        for (const uri *value : {&copied, &moved, &copy_assigned, &move_assigned}) {
            ASSERT_TRUE(value->is_valid());
            EXPECT_TRUE(value->has_query());
            EXPECT_TRUE(value->encoded_queries().empty());
            EXPECT_EQ(value->encoded_queries().data(), value->source().data() + value->source().find('?') + 1);
        }
        std::swap(copied, moved);
        EXPECT_TRUE(copied.has_query());
        EXPECT_TRUE(moved.has_query());
    }
}

TEST(UriParse, QueryPresenceFollowsDelimiterNotMapOrFragment) {
    uri value{"http://h/?x=1"};
    ASSERT_TRUE(value.has_query());
    value.queries().clear();
    EXPECT_TRUE(value.has_query());
    EXPECT_EQ(value.encoded_queries(), "x=1");
    value = std::string{"http://h/path#?not-a-query"};
    EXPECT_FALSE(value.has_query());
    EXPECT_TRUE(value.encoded_queries().empty());
    value = std::string{"http://h/?"};
    EXPECT_TRUE(value.has_query());
    value = uri{};
    EXPECT_FALSE(value.has_query());
}

// =============================================================================
// ORIGIN-FORM REQUEST TARGETS (Huly QB-1177)
// =============================================================================

/**
 * @test origin_form() keeps the path a request target names: a leading `//` is an empty first segment, never an
 *       authority (RFC 9112 §3.2.1).
 * @brief The same text read as a reference (RFC 3986 §4.2) names the host `evil.test` and the path `/transfer`: an
 *        HTTP/1.1 server that read its targets that way served `/transfer` and reported the host the sender chose.
 */
TEST(UriParse, OriginFormReadsNoAuthority) {
    struct Case {
        std::string_view target, path, query;
    };
    for (auto const &c :
         {Case{"//evil.test/transfer?a=1", "//evil.test/transfer", "a=1"}, Case{"///x", "///x", ""}, Case{"//u@h:8/p", "//u@h:8/p", ""},
          Case{"//", "//", ""}, Case{"/", "/", ""}, Case{"/a/b", "/a/b", ""}, Case{"/a?b=//c", "/a", "b=//c"}}) {
        const auto u = uri::origin_form(std::string{c.target});
        EXPECT_TRUE(u.is_valid()) << c.target;
        EXPECT_EQ(u.source(), c.target);
        EXPECT_TRUE(u.scheme().empty()) << c.target;
        EXPECT_TRUE(u.user_info().empty()) << c.target;
        EXPECT_TRUE(u.host().empty()) << c.target;
        EXPECT_TRUE(u.port().empty()) << c.target;
        EXPECT_EQ(u.path(), c.path) << c.target;
        EXPECT_EQ(u.encoded_queries(), c.query) << c.target;
        EXPECT_EQ(u.has_query(), !c.query.empty()) << c.target;
    }
    EXPECT_EQ(uri::origin_form("//evil.test/transfer?a=1&b=%7Bx%7D").query("b"), "{x}") << "the query map is the reference's";

    const uri reference{std::string{"//evil.test/transfer?a=1"}};
    EXPECT_EQ(reference.host(), "evil.test") << "the reading origin_form() exists to avoid";
    EXPECT_EQ(reference.path(), "/transfer");
}

/**
 * @test origin_form() refuses a text that is not an absolute path, and a path byte the reference parse refuses.
 * @brief An asterisk-form, an absolute-form and an authority-form target are not origin-form: is_valid() is false and
 *        nothing is read from them -- no scheme, no host. The reference parse still reads the first three.
 */
TEST(UriParse, OriginFormRefusesWhatIsNotAnAbsolutePath) {
    for (std::string_view const target : {"", "*", "a/b", "?q", "http://h/p", "h:80", "/a|b", "/a b", "/a[b]"}) {
        const auto u = uri::origin_form(std::string{target});
        EXPECT_FALSE(u.is_valid()) << '"' << target << '"';
        EXPECT_TRUE(u.scheme().empty()) << target;
        EXPECT_TRUE(u.host().empty()) << target;
        EXPECT_TRUE(u.path().empty()) << target;
    }
    EXPECT_TRUE(uri{std::string{"*"}}.is_valid());
    EXPECT_TRUE(uri{std::string{"http://h/p"}}.is_valid());
    EXPECT_EQ(uri{std::string{"http://h/p"}}.host(), "h");
}

/**
 * @test A copy and a move carry the origin-form reading: they take the parsed components, never re-parse the text.
 * @brief `//a/b` fits std::string's own buffer, which a move copies: the views must follow the text, and the copy
 *        must not read the text again as a reference.
 */
TEST(UriParse, OriginFormSurvivesCopyAndMove) {
    const auto original = uri::origin_form("//a/b?k=v");
    uri        copied{original};
    uri        assigned;
    assigned            = original;
    auto source_of_move = uri::origin_form("//a/b?k=v");
    uri  moved{std::move(source_of_move)}; // a named object: the move constructor runs, no elision
    auto source_of_move_assignment = uri::origin_form("//a/b?k=v");
    uri  move_assigned;
    move_assigned = std::move(source_of_move_assignment);
    for (uri const *u : {&copied, &assigned, &moved, &move_assigned}) {
        EXPECT_TRUE(u->is_valid());
        EXPECT_TRUE(u->host().empty());
        EXPECT_EQ(u->path(), "//a/b");
        EXPECT_EQ(u->query("k"), "v");
        EXPECT_EQ(u->path().data(), u->source().data()) << "the path views this uri's own text";
    }
}

/**
 * @test authority_form() reads the host and the port of a CONNECT request, which a reference reads as a scheme.
 * @brief `example.com:443` as a reference is the scheme `example.com` and the path `443`; in authority-form (RFC 9112
 *        §3.2.3) it is the host `example.com` and the port `443`, with no scheme and the path `/`. An IP literal keeps
 *        its brackets out of host(), as in a reference's authority.
 */
TEST(UriParse, AuthorityFormReadsAHostAndAPort) {
    struct Case {
        std::string_view target, host, port;
    };
    for (auto const &c :
         {Case{"example.com:443", "example.com", "443"}, Case{"10.0.0.1:8080", "10.0.0.1", "8080"},
          Case{"[2001:db8::1]:443", "2001:db8::1", "443"}, Case{"EXAMPLE.com:1", "EXAMPLE.com", "1"}}) {
        const auto u = uri::authority_form(std::string{c.target});
        EXPECT_TRUE(u.is_valid()) << c.target;
        EXPECT_EQ(u.source(), c.target);
        EXPECT_TRUE(u.scheme().empty()) << c.target;
        EXPECT_TRUE(u.user_info().empty()) << c.target;
        EXPECT_EQ(u.host(), c.host) << c.target;
        EXPECT_EQ(u.port(), c.port) << c.target;
        EXPECT_EQ(u.path(), "/") << c.target;
        EXPECT_FALSE(u.has_query()) << c.target;
    }
    const uri reference{std::string{"example.com:443"}};
    EXPECT_EQ(reference.scheme(), "example.com") << "the reading authority_form() exists to avoid";
    EXPECT_EQ(reference.path(), "443");
}

/**
 * @test authority_form() refuses a text that is not a host and a port, and reads nothing from it.
 */
TEST(UriParse, AuthorityFormRefusesWhatIsNotAHostAndAPort) {
    for (std::string_view const target :
         {"", "example.com", "example.com:", "example.com:x", ":443", "[]:443", "[::1]", "a:b:443", "u@example.com:443", "example.com:443/p",
          "example.com:443?q", "example.com:443#f", "//example.com:443", "http://example.com:443", "exa mple.com:443"}) {
        const auto u = uri::authority_form(std::string{target});
        EXPECT_FALSE(u.is_valid()) << '"' << target << '"';
        EXPECT_TRUE(u.scheme().empty()) << target;
        EXPECT_TRUE(u.host().empty()) << target;
        EXPECT_TRUE(u.port().empty()) << target;
    }
}

namespace uri_parse_test {

// At namespace scope, not in a member function: there a qualified call considers the private members as candidates,
// and a private `parse(bool)` -- a `const char *` converts to `bool` by a standard conversion, to `std::string const &`
// only by a user-defined one -- would win and make the call ill-formed (Huly QB-1177, its first review). clang lets
// the same call compile inside a gtest body, a member function of another class: the test must be made here.
static_assert(requires { qb::io::uri::parse("http://h/p"); }, "uri::parse(\"...\") must name the public factory");

[[nodiscard]] qb::io::uri
parse_a_literal() {
    return qb::io::uri::parse("http://h/p");
}

} // namespace uri_parse_test

/**
 * @test The static factory takes a string literal: a private parse overload must never capture the call.
 * @brief The static_assert and the namespace-scope call above are the regression test; this one runs the result.
 */
TEST(UriParse, ParseFactoryTakesAStringLiteral) {
    EXPECT_EQ(uri_parse_test::parse_a_literal().host(), "h");
    EXPECT_EQ(uri::parse("http://h/p", AF_INET).path(), "/p");
}
