/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/async/listener-deferred-throw.cpp
 * @brief A deferred callback that throws is contained by the drain that runs it, and reported -- and a report that
 *        cannot be written is contained too (Huly QB-1246).
 *
 * The drain of `listener::defer()` runs inside the loop, where an exception must not unwind through libev's C frames:
 * it contains what a callback throws and goes on with the next callback. Until 3.4 it said nothing (a pump called
 * where pumping is refused throws `std::logic_error`, which a release build swallowed). It now writes one WARN line
 * with the exception's text, from `listener::_deferred_threw()`, which is `noexcept` -- and the log line allocates: a
 * text past its inline buffer grows onto the heap. A callback that threw `std::bad_alloc` is the one whose report is
 * likeliest to fail, and a second `std::bad_alloc` out of the report was `std::terminate()`.
 *
 * Both are read here through the exception's own `what()`, which only the report calls: a probe exception counts its
 * readings, and the long one ARMS the allocator replaced below, so the next allocation -- the log line's buffer, which
 * the text outgrows -- fails. That case runs in a child (`EXPECT_EXIT`, the threadsafe death-test style re-executes
 * this binary): a terminate is an assertion here, not a crash of the suite. The child also asserts the allocation it
 * armed did fail, so a report that stopped allocating would not pass vacuously.
 */

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <string>
#include <gtest/gtest.h>
#include <qb/io/async.h>

// Not under clang's ThreadSanitizer: its runtime is a static archive whose new/delete object defines the forms
// replaced below, a duplicate definition at link (the reason tests/core/unit/container/string.cpp gives). There no
// allocation can be made to fail, and the child's case asserts only that the drain goes on.
#if defined(__clang__) && defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define QB_DEFERRED_THROW_REPLACES_ALLOCATOR 0
#endif
#endif
#if !defined(QB_DEFERRED_THROW_REPLACES_ALLOCATOR)
#define QB_DEFERRED_THROW_REPLACES_ALLOCATOR 1
#endif

// Whether the report reads `what()` at all in this build: the log macros are no-ops without a logging backend.
#if defined(QB_WITH_LOGGING) || defined(QB_STDOUT_LOGGING)
constexpr int kReportReadsWhat = 1;
#else
constexpr int kReportReadsWhat = 0;
#endif

namespace listener_deferred_throw_test {

std::atomic<bool> fail_next_allocation{false}; ///< armed by a probe's `what()`: the next allocation fails, once
std::atomic<int>  failed_allocations{0};       ///< the allocations that failed because they were armed

inline void *
allocate(std::size_t const size) {
    if (fail_next_allocation.exchange(false, std::memory_order_relaxed)) {
        failed_allocations.fetch_add(1, std::memory_order_relaxed);
        throw std::bad_alloc();
    }
    if (void *p = std::malloc(size ? size : 1))
        return p;
    throw std::bad_alloc();
}

/// A `std::exception` whose text is read only by the drain's report: it counts the readings, and the long one arms the
/// allocator, so the report's log line fails to grow its buffer.
struct Probe : std::exception {
    static inline std::atomic<int> readings{0};
    bool                           arm;
    explicit Probe(bool arm_allocator) noexcept
        : arm(arm_allocator) {}
    [[nodiscard]] char const *
    what() const noexcept override {
        readings.fetch_add(1, std::memory_order_relaxed);
        if (!arm)
            return "a deferred callback's own error";
        static std::string const long_text(4096, 'x'); // past the log line's inline buffer: it allocates
        fail_next_allocation.store(true, std::memory_order_relaxed);
        return long_text.c_str();
    }
};

} // namespace listener_deferred_throw_test

#if QB_DEFERRED_THROW_REPLACES_ALLOCATOR
// Every scalar and array form, malloc / free underneath: a sanitizer runtime supplies its own of each, and a block
// taken by one family and given back to the other is an alloc-dealloc mismatch. The aligned forms are left whole.
void *
operator new(std::size_t size) {
    return listener_deferred_throw_test::allocate(size);
}
void *
operator new(std::size_t size, std::nothrow_t const &) noexcept {
    try {
        return listener_deferred_throw_test::allocate(size);
    } catch (...) {
        return nullptr;
    }
}
void *
operator new[](std::size_t size) {
    return listener_deferred_throw_test::allocate(size);
}
void *
operator new[](std::size_t size, std::nothrow_t const &) noexcept {
    try {
        return listener_deferred_throw_test::allocate(size);
    } catch (...) {
        return nullptr;
    }
}
void
operator delete(void *p) noexcept {
    std::free(p);
}
void
operator delete(void *p, std::nothrow_t const &) noexcept {
    std::free(p);
}
void
operator delete[](void *p) noexcept {
    std::free(p);
}
void
operator delete[](void *p, std::nothrow_t const &) noexcept {
    std::free(p);
}
#if defined(__cpp_sized_deallocation)
void
operator delete(void *p, std::size_t) noexcept {
    std::free(p);
}
void
operator delete[](void *p, std::size_t) noexcept {
    std::free(p);
}
#endif
#endif // QB_DEFERRED_THROW_REPLACES_ALLOCATOR

using listener_deferred_throw_test::failed_allocations;
using listener_deferred_throw_test::Probe;

TEST(ListenerDeferredThrow, TheDrainReportsWhatADeferredCallbackThrew) {
    qb::io::async::init();
    qb::io::async::listener::current.clear();
    Probe::readings.store(0);
    bool after = false;
    qb::io::async::defer([] { throw Probe(false); });
    qb::io::async::defer([&after] { after = true; });
    qb::io::async::listener::current.run(EVRUN_NOWAIT);
    EXPECT_TRUE(after) << "the drain did not go on past the callback that threw";
    EXPECT_EQ(Probe::readings.load(), kReportReadsWhat) << "the drain contained the exception without reporting it: its text was never read";
    qb::io::async::listener::current.clear();
}

TEST(ListenerDeferredThrow, AReportThatCannotAllocateIsContainedToo) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
        {
            qb::io::async::init();
            bool after = false;
            qb::io::async::defer([] { throw Probe(true); });
            qb::io::async::defer([&after] { after = true; });
            qb::io::async::listener::current.run(EVRUN_NOWAIT);
            if (QB_DEFERRED_THROW_REPLACES_ALLOCATOR && kReportReadsWhat && failed_allocations.load() != 1)
                std::_Exit(3); // the armed allocation never failed: the case would pass vacuously
            std::_Exit(after ? 0 : 2);
        },
        ::testing::ExitedWithCode(0), "")
        << "the report of a deferred callback's exception threw out of its noexcept function (std::terminate), or the drain "
           "stopped (exit 2), or the injected failure never happened (exit 3)";
}
