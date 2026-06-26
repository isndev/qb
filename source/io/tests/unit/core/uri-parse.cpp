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
#include <limits>
#include <string>
#include <string_view>
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
    const uri u(
        "https://user:pass@[2001:db8::1]/api/v1/resource"
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
    const char    *source;
    const char    *scheme;
    const char    *user_info; // "" when absent
    const char    *host;
    std::uint16_t  port;
    int            af;
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
        ResolveCase{"https://www.example.com/section1/section2/action?query1=value1&query2=value2",
                    "https", "", "www.example.com", 443, AF_INET},
        ResolveCase{"https://www.example.com:8080/section1/section2/action?query1=value1&query2=value2",
                    "https", "", "www.example.com", 8080, AF_INET},
        ResolveCase{"https://localhost/section1/section2/action?query1=value1&query2=value2",
                    "https", "", "localhost", 443, AF_INET},
        ResolveCase{"https://localhost:8080/section1/section2/action?query1=value1&query2=value2",
                    "https", "", "localhost", 8080, AF_INET},
        // host = IPv4 literal
        ResolveCase{"https://127.0.0.1/section1/section2/action?query1=value1&query2=value2",
                    "https", "", "127.0.0.1", 443, AF_INET},
        ResolveCase{"https://127.0.0.1:8080/section1/section2/action?query1=value1&query2=value2",
                    "https", "", "127.0.0.1", 8080, AF_INET},
        // host = IPv6 literal (bracketed) → AF_INET6
        ResolveCase{"https://[::1]/section1/section2/action?query1=value1&query2=value2",
                    "https", "", "::1", 443, AF_INET6},
        ResolveCase{"https://[::1]:8080/section1/section2/action?query1=value1&query2=value2",
                    "https", "", "::1", 8080, AF_INET6},
        // with userinfo across each host family
        ResolveCase{"https://user:password@www.example.com/section1/section2/action?query1=value1&query2=value2",
                    "https", "user:password", "www.example.com", 443, AF_INET},
        ResolveCase{"https://user:password@www.example.com:8080/section1/section2/action?query1=value1&query2=value2",
                    "https", "user:password", "www.example.com", 8080, AF_INET},
        ResolveCase{"https://user:password@localhost:8080/section1/section2/action?query1=value1&query2=value2",
                    "https", "user:password", "localhost", 8080, AF_INET},
        ResolveCase{"https://user:password@127.0.0.1/section1/section2/action?query1=value1&query2=value2",
                    "https", "user:password", "127.0.0.1", 443, AF_INET},
        ResolveCase{"https://user:password@[::1]:8080/section1/section2/action?query1=value1&query2=value2",
                    "https", "user:password", "::1", 8080, AF_INET6}));

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

    uri to_move(assigned);
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
    uri       unix_src("unix:///var/run/app.sock");
    ASSERT_EQ(unix_src.af(), AF_UNIX);
    uri move_assigned;
    move_assigned = std::move(unix_src);
    EXPECT_EQ(move_assigned.af(), AF_UNIX);
    EXPECT_EQ(move_assigned.scheme(), "unix");

    // Move-construct: unix.
    uri       unix_src2("unix://my.sock/service");
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

INSTANTIATE_TEST_SUITE_P(
    Corpus, UriRoundtrip,
    ::testing::Values(std::string("simple text"),
                      std::string("special: !@#$%^&*()"),
                      std::string(""),
                      std::string("unicode: \xC3\xA9\xC3\xA0\xC3\xBC"),
                      std::string("slashes/and?query=yes&more=true"),
                      std::string(1000, 'X'),
                      std::string("trailing%"),
                      std::string("%already%20encoded")));

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
    const auto &c = GetParam();
    std::string path = c.input;
    EXPECT_TRUE(uri::normalize_path(path)) << c.input;
    EXPECT_EQ(path, c.expected) << "normalize_path(\"" << c.input << "\")";
}

INSTANTIATE_TEST_SUITE_P(
    DotSegments, UriNormalize,
    ::testing::Values(
        // empty / dot → root
        NormalizeCase{"", "/"},
        NormalizeCase{".", "/"},
        // absolute path: `..` pops, `.` and `//` collapse, trailing slash stripped
        NormalizeCase{"/a/b/../c/./d//", "/a/c/d"},
        // relative path: leading `..` are retained (cannot escape above the relative root)
        NormalizeCase{"../a/./b/../../c", "../c"},
        // spec D4: a `/../`-escape attempt at the absolute root is clamped (no traversal above /)
        NormalizeCase{"/../../../etc/passwd", "/etc/passwd"},
        NormalizeCase{"/a/../../b", "/b"},
        // spec D4: Windows backslashes are standardized to '/', and a drive-letter-style
        // path normalizes its separators + dot segments like any other.
        NormalizeCase{"a\\b\\..\\c", "a/c"},
        NormalizeCase{"C:\\Users\\.\\qb\\..\\io", "C:/Users/io"}));

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
    EXPECT_TRUE(qb::io::is_reserved('/'))  << "gen-delim leg";
    EXPECT_TRUE(qb::io::is_reserved('@'));
    EXPECT_TRUE(qb::io::is_reserved('!'))  << "sub-delim leg";
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
    EXPECT_TRUE(qb::io::is_user_info_character('a'));   // unreserved
    EXPECT_TRUE(qb::io::is_user_info_character('~'));
    EXPECT_TRUE(qb::io::is_user_info_character('!'));   // sub-delim
    EXPECT_TRUE(qb::io::is_user_info_character('%'));   // pct-encoded marker
    EXPECT_TRUE(qb::io::is_user_info_character(':'));   // user:pass separator

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
    EXPECT_TRUE(qb::io::is_query_character('?'))    << "'?' is legal inside a query";
    EXPECT_TRUE(qb::io::is_query_character('/'));
    EXPECT_TRUE(qb::io::is_query_character('a'));
    EXPECT_TRUE(qb::io::is_query_character('@'));
    EXPECT_TRUE(qb::io::is_query_character(':'));
    EXPECT_FALSE(qb::io::is_query_character('#')) << "'#' starts the fragment, ends the query";
    EXPECT_FALSE(qb::io::is_query_character(' '));

    // fragment is defined as identical to query — assert byte-for-byte agreement.
    for (int c = 0; c < 256; ++c)
        EXPECT_EQ(qb::io::is_fragment_character(c), qb::io::is_query_character(c))
            << "fragment vs query divergence at byte " << c;
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
    const uri u("http://host/p?dup=1&dup=2&dup=3&solo=%7Bx%7D");
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
    EXPECT_EQ(u.query("absent"), "")            << "absent key → empty";
    EXPECT_EQ(u.query("present", 1), "")        << "present key, index past end → empty";
    EXPECT_EQ(u.query("multi", 5), "")          << "index well beyond size → empty";

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

    u.queries()["k"] = {"edited"};
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
