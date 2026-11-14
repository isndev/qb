/**
 * @file qb/tests/io/unit/core/uri-alloc.cpp
 * @brief A `qb::io::uri` read or parse whose allocation fails reports a failed read, and the process goes on (Huly
 *        QB-1248).
 *
 * The readers of a uri are `noexcept` and allocate: the copy of the text, then the query map -- its buckets (two
 * rehashes for the six keys read here), its nodes, each key's vector, a key or a value decoded past the small-string
 * buffer. A `std::bad_alloc` out of any of them called `std::terminate()`. Each allocation of each reader is made to
 * fail in turn, from the first, until a run reaches none -- the control, which counts them and makes the account
 * complete: the read returns false, the uri is left a default-constructed one that is not valid, and the next read on
 * it works. What is not `noexcept` throws: the copy of the text in the copying constructor and assignment (the uri
 * assigned unchanged), and `decode()` and `encode()`, as a std::string does. `normalize_path()` returns false and
 * leaves its path as it was.
 *
 * Alone in its binary: the cases replace the global allocation functions to make the Nth allocation of the calling
 * thread fail. Skipped under MSVC's debug standard library, whose noexcept std::string operations allocate.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 * @ingroup IO
 */

#include <cstddef>
#include <cstdlib>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <gtest/gtest.h>

#include <qb/io/uri.h>

// The Nth allocation of the calling thread fails, while armed. Every form of the global allocation functions is
// replaced, the nothrow and the array ones included: a sanitizer's runtime supplies its own for whatever form is left
// out, and a block one of those allocated must not reach free().
namespace uri_alloc_test::injection {

thread_local int  countdown = -1;    ///< -1: off; otherwise the allocations left before the one that fails.
thread_local int  seen      = 0;     ///< Allocations seen while armed.
thread_local bool fired     = false; ///< Whether the armed allocation was reached, and failed.

inline bool
fail_now() noexcept {
    if (countdown < 0)
        return false;
    ++seen;
    if (countdown-- == 0) {
        countdown = -1;
        fired     = true;
        return true;
    }
    return false;
}

inline void *
plain(std::size_t size) {
    if (fail_now())
        throw std::bad_alloc();
    if (void *p = std::malloc(size ? size : 1))
        return p;
    throw std::bad_alloc();
}

inline void *
aligned(std::size_t size, std::align_val_t alignment) {
    if (fail_now())
        throw std::bad_alloc();
    const std::size_t a = static_cast<std::size_t>(alignment);
#if defined(_WIN32)
    void *p = _aligned_malloc(size ? size : 1, a);
#else
    void *p = nullptr;
    if (::posix_memalign(&p, a < sizeof(void *) ? sizeof(void *) : a, size ? size : 1) != 0)
        p = nullptr;
#endif
    if (p)
        return p;
    throw std::bad_alloc();
}

inline void
release_aligned(void *p) noexcept {
#if defined(_WIN32)
    _aligned_free(p);
#else
    std::free(p);
#endif
}

template <typename Allocate>
inline void *
or_null(Allocate &&allocate) noexcept {
    try {
        return allocate();
    } catch (...) {
        return nullptr;
    }
}

} // namespace uri_alloc_test::injection

// clang-format off
namespace inj = uri_alloc_test::injection;
void *operator new(std::size_t n) { return inj::plain(n); }
void *operator new[](std::size_t n) { return inj::plain(n); }
void *operator new(std::size_t n, std::nothrow_t const &) noexcept { return inj::or_null([n] { return inj::plain(n); }); }
void *operator new[](std::size_t n, std::nothrow_t const &) noexcept { return inj::or_null([n] { return inj::plain(n); }); }
void *operator new(std::size_t n, std::align_val_t a) { return inj::aligned(n, a); }
void *operator new[](std::size_t n, std::align_val_t a) { return inj::aligned(n, a); }
void *operator new(std::size_t n, std::align_val_t a, std::nothrow_t const &) noexcept { return inj::or_null([n, a] { return inj::aligned(n, a); }); }
void *operator new[](std::size_t n, std::align_val_t a, std::nothrow_t const &) noexcept { return inj::or_null([n, a] { return inj::aligned(n, a); }); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
void operator delete(void *p, std::nothrow_t const &) noexcept { std::free(p); }
void operator delete[](void *p, std::nothrow_t const &) noexcept { std::free(p); }
void operator delete(void *p, std::align_val_t) noexcept { inj::release_aligned(p); }
void operator delete[](void *p, std::align_val_t) noexcept { inj::release_aligned(p); }
void operator delete(void *p, std::size_t, std::align_val_t) noexcept { inj::release_aligned(p); }
void operator delete[](void *p, std::size_t, std::align_val_t) noexcept { inj::release_aligned(p); }
void operator delete(void *p, std::align_val_t, std::nothrow_t const &) noexcept { inj::release_aligned(p); }
void operator delete[](void *p, std::align_val_t, std::nothrow_t const &) noexcept { inj::release_aligned(p); }
// clang-format on

namespace uri_alloc_test {

using qb::io::uri;

/// No reader here makes more allocations than this; a loop that reaches it has lost its control.
constexpr int max_allocations = 64;

/// MSVC's debug standard library (`_ITERATOR_DEBUG_LEVEL` above 0) gives every std::string a container proxy it
/// allocates in operations the standard makes noexcept -- the default and the move constructors among them --, so a
/// failure injected there terminates inside the library, whatever qb catches: in the uri every factory builds and in
/// every move. What this binary sweeps holds for a release standard library; MSVC's Debug builds are not configured
/// with it (tests/io/unit/CMakeLists.txt), and a multi-config build skips it here.
#if defined(_ITERATOR_DEBUG_LEVEL) && _ITERATOR_DEBUG_LEVEL != 0
constexpr bool standard_library_allocates_in_noexcept = true;
#else
constexpr bool standard_library_allocates_in_noexcept = false;
#endif

class UriAlloc : public ::testing::Test {
protected:
    void
    SetUp() override {
        if (standard_library_allocates_in_noexcept)
            GTEST_SKIP() << "MSVC's debug standard library allocates a container proxy in noexcept string operations: an "
                            "injected failure there terminates in the library";
    }
};

// Every text is longer than any small-string buffer (15 bytes in libstdc++ and MSVC's library, 22 in libc++), and so
// are a key and a value of its query: the copy allocates, and so do their decodings. Six keys: the map rehashes twice.
constexpr std::string_view long_key   = "a-key-long-enough-to-leave-the-small-buffer";
constexpr std::string_view long_value = "a-value-long-enough-to-leave-the-small-buffer";
#define QB_URI_ALLOC_QUERY                                                                                              \
    "?first=one&second=a-value-long-enough-to-leave-the-small-buffer&a-key-long-enough-to-leave-the-small-buffer=three" \
    "&fourth=4&fifth=5&sixth=6&first=two"
constexpr std::string_view origin_target    = "/a/path/long/enough/to/leave/the/small/buffer" QB_URI_ALLOC_QUERY;
constexpr std::string_view absolute_target  = "http://a-host-long-enough.example/a/path" QB_URI_ALLOC_QUERY;
constexpr std::string_view authority_target = "a-host-name-long-enough-for-the-heap.example:443";
constexpr std::string_view reference_text   = "http://a-host-long-enough.example:8080/a/path" QB_URI_ALLOC_QUERY;
#undef QB_URI_ALLOC_QUERY

/// What one call saw of its `failing`-th allocation (from 0).
struct Outcome {
    bool fired = false; ///< The armed allocation was reached, and failed.
    int  seen  = 0;     ///< Allocations counted while armed, the failed one included.
};

/// Calls @p call with the `failing`-th allocation of this thread, from 0, made to fail; disarmed on the way out, an
/// exception included.
template <typename Call>
Outcome
with_failing_allocation(int failing, Call &&call) {
    injection::seen      = 0;
    injection::fired     = false;
    injection::countdown = failing;
    try {
        call();
    } catch (...) {
        injection::countdown = -1;
        throw;
    }
    const Outcome run{injection::fired, injection::seen};
    injection::countdown = -1;
    return run;
}

/// A uri an allocation failure left: a default-constructed uri that is not valid.
void
expect_reset(uri const &u) {
    EXPECT_FALSE(u.is_valid());
    EXPECT_EQ(u.af(), AF_INET);
    EXPECT_TRUE(u.source().empty()) << u.source();
    EXPECT_TRUE(u.scheme().empty());
    EXPECT_TRUE(u.user_info().empty());
    EXPECT_TRUE(u.host().empty());
    EXPECT_TRUE(u.port().empty());
    EXPECT_TRUE(u.path().empty());
    EXPECT_FALSE(u.has_query());
    EXPECT_TRUE(u.encoded_queries().empty());
    EXPECT_TRUE(u.queries().empty());
    EXPECT_TRUE(u.fragment().empty());
}

/// The query every text above carries, as its map holds it.
void
expect_queries(uri const &u) {
    EXPECT_EQ(u.query("first", 0), "one");
    EXPECT_EQ(u.query("first", 1), "two");
    EXPECT_EQ(u.query("second"), long_value);
    EXPECT_EQ(u.query(long_key), "three");
    EXPECT_EQ(u.query("fourth"), "4");
    EXPECT_EQ(u.query("fifth"), "5");
    EXPECT_EQ(u.query("sixth"), "6");
    EXPECT_EQ(u.queries().size(), 6u);
}

/// Runs @p read with each of its allocations made to fail in turn, from the first, until a run reaches none: the
/// control, which counts them. @p prepare runs before each, unarmed. @p read returns what the call reported, @p uri_of
/// the address of the uri it left; after a failed allocation @p read_again must read into that uri what @p expect
/// checks. Returns the allocations the control counted.
template <typename Prepare, typename Read, typename UriOf, typename ReadAgain, typename Expect>
int
each_allocation_fails(Prepare &&prepare, Read &&read, UriOf &&uri_of, ReadAgain &&read_again, Expect &&expect) {
    for (int failing = 0; failing < max_allocations; ++failing) {
        prepare();
        bool          reported = true;
        const Outcome run      = with_failing_allocation(failing, [&] { reported = read(); });
        if (!run.fired) { // the control: no allocation is left to fail, and the read is the ordinary one
            EXPECT_TRUE(reported);
            EXPECT_EQ(run.seen, failing) << "the control counts the allocations the runs before it made fail";
            expect(*uri_of());
            return failing;
        }
        SCOPED_TRACE("allocation #" + std::to_string(failing + 1) + " fails");
        EXPECT_FALSE(reported) << "an allocation failure is a failed read";
        expect_reset(*uri_of());
        EXPECT_TRUE(read_again()) << "a uri an allocation failure left reads the next text";
        expect(*uri_of());
    }
    ADD_FAILURE() << "every one of " << max_allocations << " allocations was reached: no control";
    return max_allocations;
}

void
expect_origin(uri const &u) {
    EXPECT_TRUE(u.is_valid());
    EXPECT_EQ(u.source(), origin_target);
    EXPECT_EQ(u.path(), "/a/path/long/enough/to/leave/the/small/buffer");
    EXPECT_TRUE(u.host().empty());
    expect_queries(u);
}

void
expect_absolute(uri const &u) {
    EXPECT_TRUE(u.is_valid());
    EXPECT_EQ(u.source(), absolute_target);
    EXPECT_EQ(u.scheme(), "http");
    EXPECT_EQ(u.host(), "a-host-long-enough.example");
    EXPECT_EQ(u.path(), "/a/path");
    expect_queries(u);
}

void
expect_authority(uri const &u) {
    EXPECT_TRUE(u.is_valid());
    EXPECT_EQ(u.source(), authority_target);
    EXPECT_EQ(u.host(), "a-host-name-long-enough-for-the-heap.example");
    EXPECT_EQ(u.u_port(), 443);
}

void
expect_reference(uri const &u) {
    EXPECT_TRUE(u.is_valid());
    EXPECT_EQ(u.source(), reference_text);
    EXPECT_EQ(u.scheme(), "http");
    EXPECT_EQ(u.host(), "a-host-long-enough.example");
    EXPECT_EQ(u.u_port(), 8080);
    EXPECT_EQ(u.path(), "/a/path");
    expect_queries(u);
}

/// What a copying assignment that throws must leave as it was: a family another than the one a new text starts from.
constexpr std::string_view previous_text = "tcp://[::1]:9/p";

/// Makes @p u a uri no read has touched, its storage included: destroyed and built again where it stands. An
/// assignment of `uri()` does not: libstdc++'s move assignment from a string in its small buffer keeps the storage the
/// target string holds, so the uri a run before had read into came back with room for the text.
void
build_anew(uri &u) {
    std::destroy_at(&u);
    std::construct_at(&u);
}

void
expect_previous(uri const &u) {
    EXPECT_TRUE(u.is_valid());
    EXPECT_EQ(u.source(), previous_text);
    EXPECT_EQ(u.af(), AF_INET6);
    EXPECT_EQ(u.host(), "::1");
    EXPECT_EQ(u.u_port(), 9);
    EXPECT_EQ(u.path(), "/p");
}

} // namespace uri_alloc_test

using namespace uri_alloc_test;

// The copy of the target, then the query map. The uri is built anew before each run (build_anew): one assigned
// `uri()` kept, with libstdc++, the room the run before had left it for the text, so the copy -- the first allocation
// of every run after the first -- was no longer made: the query map's first allocation was then never the one to
// fail, and the authority-form case below, whose one allocation is that copy, counted none in its control (red on
// g++ 14 and clang 19 with libstdc++; libc++ and MSVC's library release that storage).
TEST_F(UriAlloc, ReadOriginFormReportsEachFailedAllocationAsAFailedRead) {
    uri        u;
    const auto reads = each_allocation_fails([&] { build_anew(u); }, [&] { return u.read_origin_form(origin_target); }, [&] { return &u; },
                                             [&] { return u.read_origin_form(origin_target); }, expect_origin);
    EXPECT_EQ(reads, 1 + 2 + 6 + 6 + 1 + 2) << "the copy of the text, then the query map's account (the case below)";
}

// The text fits the storage the uri already holds, so every allocation of the read is the query map's, inside the
// parse: the read returns what the parse returned.
TEST_F(UriAlloc, ParseReportsAQueryMapThatCannotBeAllocatedAsAFailedParse) {
    const std::string room(origin_target.size(), '/'); // a path and no query: storage for the text, and nothing else
    uri               u;
    const char       *storage = nullptr;
    const auto        reads   = each_allocation_fails(
        [&] {
            u = uri();
            ASSERT_TRUE(u.read_origin_form(room));
            storage = u.source().data();
        },
        [&] { return u.read_origin_form(origin_target); },
        [&] {
            EXPECT_EQ(u.source().data(), storage) << "the text was copied into new storage: the failure was not the parse's";
            return &u;
        },
        [&] { return u.read_origin_form(origin_target); }, expect_origin);
    // The whole account, the same for every release standard library: the map's two bucket arrays (4, then 8: the fifth
    // key rehashes it), a node and a vector a key, the second value of `first` regrowing its vector, and the decodings
    // of the long key and the long value -- each key moved into its node, never copied.
    EXPECT_EQ(reads, 2 + 6 + 6 + 1 + 2);
}

TEST_F(UriAlloc, ReadAbsoluteFormReportsEachFailedAllocationAsAFailedRead) {
    uri        u;
    const auto reads = each_allocation_fails([&] { build_anew(u); }, [&] { return u.read_absolute_form(absolute_target); }, [&] { return &u; },
                                             [&] { return u.read_absolute_form(absolute_target); }, expect_absolute);
    EXPECT_EQ(reads, 1 + 2 + 6 + 6 + 1 + 2) << "the copy of the text, then the query map's account";
}

// An authority-form target holds no query: the copy of the text is its one allocation.
TEST_F(UriAlloc, ReadAuthorityFormReportsAFailedCopyAsAFailedRead) {
    uri        u;
    const auto reads = each_allocation_fails([&] { build_anew(u); }, [&] { return u.read_authority_form(authority_target); },
                                             [&] { return &u; }, [&] { return u.read_authority_form(authority_target); }, expect_authority);
    EXPECT_EQ(reads, 1);
}

// The noexcept constructor takes the text it is given, and parses it: every allocation is the query map's.
TEST_F(UriAlloc, TheMovingConstructorLeavesAnInvalidUriWhenItsQueryMapCannotBeAllocated) {
    std::optional<uri> u;
    std::string        text;
    const auto         reads = each_allocation_fails(
        [&] {
            u.reset();
            text = reference_text;
        },
        [&] { return u.emplace(std::move(text)).is_valid(); }, [&] { return &*u; },
        [&] {
            *u = std::string{reference_text};
            return u->is_valid();
        },
        expect_reference);
    EXPECT_GE(reads, 9) << "the query map";
}

// The factory parse() copies the text; the moving assignment and the three request-target factories take it, their
// caller having made the copy: each reports an allocation that fails inside as a uri that is not valid.
TEST_F(UriAlloc, TheFactoriesAndTheMovingAssignmentLeaveAnInvalidUriWhenTheyCannotAllocate) {
    const std::string  text{reference_text};
    std::string        given;
    std::optional<uri> u;
    const auto         read_again = [&] {
        *u = std::string{text};
        return u->is_valid();
    };

    const auto by_factory = each_allocation_fails([&] { u.reset(); }, [&] { return u.emplace(uri::parse(text)).is_valid(); },
                                                  [&] { return &*u; }, read_again, expect_reference);
    EXPECT_GE(by_factory, 10) << "the copy of the text, and the query map";

    const auto by_move = each_allocation_fails(
        [&] {
            u.emplace();
            given = text;
        },
        [&] { return (*u = std::move(given)).is_valid(); }, [&] { return &*u; }, read_again, expect_reference);
    EXPECT_EQ(by_move, by_factory - 1) << "the text is taken, not copied";

    const auto by_origin_form = each_allocation_fails(
        [&] {
            u.reset();
            given = origin_target;
        },
        [&] { return u.emplace(uri::origin_form(std::move(given))).is_valid(); }, [&] { return &*u; },
        [&] { return u->read_origin_form(origin_target); }, expect_origin);
    EXPECT_EQ(by_origin_form, by_move) << "the same query map";

    const auto by_absolute_form = each_allocation_fails(
        [&] {
            u.reset();
            given = absolute_target;
        },
        [&] { return u.emplace(uri::absolute_form(std::move(given))).is_valid(); }, [&] { return &*u; },
        [&] { return u->read_absolute_form(absolute_target); }, expect_absolute);
    EXPECT_EQ(by_absolute_form, by_move) << "the same query map";

    const auto by_authority_form = each_allocation_fails(
        [&] {
            u.reset();
            given = authority_target;
        },
        [&] { return u.emplace(uri::authority_form(std::move(given))).is_valid(); }, [&] { return &*u; },
        [&] { return u->read_authority_form(authority_target); }, expect_authority);
    EXPECT_EQ(by_authority_form, 0) << "a host and a port: nothing to allocate once the text is taken";
}

// The copying constructor and assignment are not noexcept: the copy of the text throws std::bad_alloc, as a
// std::string's does -- the assigned uri as it was --, and the query map's failures leave a uri that is not valid.
TEST_F(UriAlloc, TheCopyingConstructorAndAssignmentThrowOnTheCopyAndReportTheQueryMap) {
    const std::string  text{reference_text};
    std::optional<uri> u;
    for (const bool assign : {false, true}) {
        SCOPED_TRACE(assign ? "operator=(std::string const &)" : "uri(std::string const &)");
        int failing = 0;
        for (; failing < max_allocations; ++failing) {
            if (assign)
                u.emplace(std::string{previous_text});
            else
                u.reset();
            bool    threw    = false;
            bool    reported = true;
            Outcome run;
            try {
                run = with_failing_allocation(failing, [&] { reported = (assign ? (*u = text) : u.emplace(text)).is_valid(); });
            } catch (std::bad_alloc const &) {
                threw = true;
            }
            if (failing == 0) { // the copy of the text, the first allocation
                EXPECT_TRUE(threw) << "the copy's failure is the caller's";
                if (assign)
                    expect_previous(*u);
                else
                    EXPECT_FALSE(u.has_value());
                continue;
            }
            EXPECT_FALSE(threw) << "allocation #" << failing + 1 << ": the query map's failure is a failed read";
            if (!run.fired) {
                EXPECT_TRUE(reported);
                EXPECT_EQ(run.seen, failing);
                expect_reference(*u);
                break;
            }
            SCOPED_TRACE("allocation #" + std::to_string(failing + 1) + " fails");
            EXPECT_FALSE(reported);
            expect_reset(*u);
        }
        EXPECT_LT(failing, max_allocations) << "no control";
        EXPECT_GE(failing, 10) << "the copy of the text, and the query map";
    }
}

// decode() and encode() are not noexcept: a result that cannot be allocated throws, as a std::string does.
TEST_F(UriAlloc, DecodeAndEncodeThrowWhenTheirResultCannotBeAllocated) {
    constexpr std::string_view encoded = "a+value+long+enough%2Fto+leave+the+small+buffer";
    constexpr std::string_view decoded = "a value long enough/to leave the small buffer";
    struct Codec {
        const char *name;
        std::string (*call)(std::string_view);
        std::string_view input;
        std::string_view output;
    };
    const Codec codecs[] = {
        {"decode(std::string_view)", [](std::string_view in) { return uri::decode(in); }, encoded, decoded},
        {"decode(const char *, std::size_t)", [](std::string_view in) { return uri::decode(in.data(), in.size()); }, encoded, decoded},
        {"encode(std::string_view)", [](std::string_view in) { return uri::encode(in); }, decoded, encoded},
        {"encode(const char *, std::size_t)", [](std::string_view in) { return uri::encode(in.data(), in.size()); }, decoded, encoded},
    };
    for (auto const &codec : codecs) {
        SCOPED_TRACE(codec.name);
        std::string result = "unset";
        EXPECT_THROW(with_failing_allocation(0, [&] { result = codec.call(codec.input); }), std::bad_alloc);
        EXPECT_EQ(result, "unset") << "nothing was assigned";
        const Outcome control = with_failing_allocation(1, [&] { result = codec.call(codec.input); });
        EXPECT_FALSE(control.fired) << "the result is one allocation";
        EXPECT_EQ(control.seen, 1);
        EXPECT_EQ(result, codec.output);
    }
}

// The path is written once, with its result: a result that cannot be allocated leaves it as it was, its backslashes
// included.
TEST_F(UriAlloc, NormalizePathLeavesThePathAsItWasWhenItsResultCannotBeAllocated) {
    const std::string input    = R"(C:\a-directory-long-enough-for-the-heap\.\another-one\..\and-a-last-one-past-the-buffer\file)";
    const std::string expected = "C:/a-directory-long-enough-for-the-heap/and-a-last-one-past-the-buffer/file";
    std::string       path;
    int               failing = 0;
    for (; failing < max_allocations; ++failing) {
        path                     = input;
        bool          normalized = true;
        const Outcome run        = with_failing_allocation(failing, [&] { normalized = uri::normalize_path(path); });
        if (!run.fired) {
            EXPECT_TRUE(normalized);
            EXPECT_EQ(run.seen, failing);
            EXPECT_EQ(path, expected);
            break;
        }
        SCOPED_TRACE("allocation #" + std::to_string(failing + 1) + " fails");
        EXPECT_FALSE(normalized);
        EXPECT_EQ(path, input);
    }
    EXPECT_LT(failing, max_allocations) << "no control";
    EXPECT_GE(failing, 3) << "the segments, their copies past the small buffer, the result";
}
