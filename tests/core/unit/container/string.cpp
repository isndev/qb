/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/container/string.cpp
 * @brief `qb::string<N>` — the fixed-capacity, stack-allocated string (`qb/string.h`).
 *
 * Pure value-type logic, fully deterministic: NO engine, loop, daemon, or network. Drives the
 * whole surface — every ctor / assignment / accessor / iterator flavour, capacity & resize,
 * mutators, substr/compare, find/rfind (char & substring), the C++20 starts/ends/contains trio,
 * all comparison and concatenation operators, std::string / string_view conversions, stream I/O,
 * and constexpr construction. Assertions check computed/derived truth (hand-verified search
 * positions against an annotated corpus, OOB throws, capacity-truncation semantics), not echoes.
 *
 * Added over the original: an explicit assertion on the *moved-from* state (this is a fixed
 * buffer with `= default` move — no ownership transfer, so the source stays valid and unchanged);
 * an interior-NUL `assign(ptr, len)` case proving `size()` honours the explicit length rather than
 * `strlen` (the bytes, including the embedded NUL, survive); and find/rfind start-position edge
 * cases (pos past the end, pos 0, and a substring present only before pos). Then the whole-length
 * contract that follows from it (Huly QB-386: comparisons, searches and stream insertion over the
 * size, not up to an interior NUL) and assign from a view of the string itself (Huly QB-387); a
 * char buffer read up to its first NUL, and the stream operators under a throwing stream.
 */

#include <ios>
#include <istream>
#include <ostream>
#include <sstream>
#include <streambuf>
#include <string>
#include <string_view>
#include <utility>
#include <type_traits>

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <gtest/gtest.h>
#include <qb/string.h>

// The bytes the program asks the global allocator for while `string_alloc_probe::counting` is set: the oracle of the
// bounded extraction (Huly QB-1002). The replacement is the standard's own -- malloc / free underneath -- and counts
// nothing outside the window a test opens. EVERY scalar form is replaced, the nothrow pair included: a sanitizer
// runtime supplies its own of each form, and a block taken by one family and given back to the other is an
// alloc-dealloc mismatch (gtest's stable_sort takes its buffer with the nothrow form and frees it with the sized one).
// The array and aligned forms are left whole to one family -- the runtime's, or the library's, which call these.
namespace string_alloc_probe {
std::atomic<bool>        counting{false};
std::atomic<std::size_t> bytes{0};

inline void *
allocate(std::size_t size) noexcept {
    if (counting.load(std::memory_order_relaxed))
        bytes.fetch_add(size, std::memory_order_relaxed);
    return std::malloc(size ? size : 1);
}
} // namespace string_alloc_probe

void *
operator new(std::size_t size) {
    if (void *p = string_alloc_probe::allocate(size))
        return p;
    throw std::bad_alloc();
}

void *
operator new(std::size_t size, std::nothrow_t const &) noexcept {
    return string_alloc_probe::allocate(size);
}

void
operator delete(void *p) noexcept {
    std::free(p);
}

void
operator delete(void *p, std::nothrow_t const &) noexcept {
    std::free(p);
}

#if defined(__cpp_sized_deallocation)
void
operator delete(void *p, std::size_t) noexcept {
    std::free(p);
}
#endif

namespace {

// Test fixture for basic string operations
class StringTest : public ::testing::Test {
protected:
    void
    SetUp() override {
        // Common setup for string tests
    }

    // Helper function to create test strings
    template <std::size_t N>
    qb::string<N>
    make_test_string(const char *str) {
        return qb::string<N>(str);
    }
};

// Test fixture for string algorithms and search operations
class StringAlgorithmTest : public ::testing::Test {
protected:
    void
    SetUp() override {
        test_str_  = qb::string<50>("Hello, World! This is a test string.");
        empty_str_ = qb::string<10>();
    }

    qb::string<50> test_str_;
    qb::string<10> empty_str_;
};

// Test fixture for string capacity and memory operations
class StringCapacityTest : public ::testing::Test {
protected:
    void
    SetUp() override {
        small_str_ = qb::string<10>("test");
        large_str_ = qb::string<100>("This is a much longer test string that exceeds normal limits");
    }

    qb::string<10>  small_str_;
    qb::string<100> large_str_;
};

// Construction tests

TEST_F(StringTest, DefaultConstruction) {
    qb::string<30> str;
    EXPECT_TRUE(str.empty());
    EXPECT_EQ(str.size(), 0);
    EXPECT_EQ(str.length(), 0);
    EXPECT_STREQ(str.c_str(), "");
    EXPECT_EQ(str.capacity(), 30);
    EXPECT_EQ(str.max_size(), 30);
}

TEST_F(StringTest, CStringLiteralConstruction) {
    qb::string<30> str("Hello");
    EXPECT_FALSE(str.empty());
    EXPECT_EQ(str.size(), 5);
    EXPECT_EQ(str.length(), 5);
    EXPECT_STREQ(str.c_str(), "Hello");
}

TEST_F(StringTest, CStringPointerConstruction) {
    const char    *cstr = "World";
    qb::string<30> str(cstr);
    EXPECT_EQ(str.size(), 5);
    EXPECT_STREQ(str.c_str(), "World");
}

TEST_F(StringTest, CStringWithSizeConstruction) {
    const char    *cstr = "Hello World";
    qb::string<30> str(cstr, 5);
    EXPECT_EQ(str.size(), 5);
    EXPECT_STREQ(str.c_str(), "Hello");
}

TEST_F(StringTest, FillConstruction) {
    qb::string<30> str(10, 'A');
    EXPECT_EQ(str.size(), 10);
    EXPECT_STREQ(str.c_str(), "AAAAAAAAAA");
}

TEST_F(StringTest, StdStringConstruction) {
    std::string    std_str = "Standard string";
    qb::string<30> str(std_str);
    EXPECT_EQ(str.size(), 15);
    EXPECT_STREQ(str.c_str(), "Standard string");
}

TEST_F(StringTest, CopyConstruction) {
    qb::string<30> original("Original");
    qb::string<30> copy(original);
    EXPECT_EQ(copy.size(), 8);
    EXPECT_STREQ(copy.c_str(), "Original");
    EXPECT_EQ(copy, original);
}

TEST_F(StringTest, MoveConstruction) {
    qb::string<30> original("Original");
    qb::string<30> moved(std::move(original));
    EXPECT_EQ(moved.size(), 8);
    EXPECT_STREQ(moved.c_str(), "Original");
    // qb::string is a fixed inline buffer with a defaulted (trivial-copy) move ctor: there is no
    // heap ownership to steal, so the moved-FROM object is left valid AND unchanged (equal to the
    // value moved out). This is a stronger guarantee than std::string's "valid but unspecified".
    EXPECT_EQ(original.size(), 8u);
    EXPECT_STREQ(original.c_str(), "Original");
    EXPECT_EQ(original, moved);
}

TEST_F(StringTest, TruncationOnOverflow) {
    // Test that strings are properly truncated when they exceed capacity
    qb::string<5> str("This is a very long string");
    EXPECT_EQ(str.size(), 5);
    EXPECT_STREQ(str.c_str(), "This ");
}

// Assignment tests

TEST_F(StringTest, CopyAssignment) {
    qb::string<30> str1("First");
    qb::string<30> str2("Second");
    str1 = str2;
    EXPECT_EQ(str1.size(), 6);
    EXPECT_STREQ(str1.c_str(), "Second");
}

TEST_F(StringTest, MoveAssignment) {
    qb::string<30> str1("First");
    qb::string<30> str2("Second");
    str1 = std::move(str2);
    EXPECT_EQ(str1.size(), 6);
    EXPECT_STREQ(str1.c_str(), "Second");
    // As with move-construction: defaulted move-assign over a fixed buffer leaves the moved-FROM
    // string valid and unchanged (still "Second").
    EXPECT_EQ(str2.size(), 6u);
    EXPECT_STREQ(str2.c_str(), "Second");
}

TEST_F(StringTest, CStringLiteralAssignment) {
    qb::string<30> str;
    str = "Assigned";
    EXPECT_EQ(str.size(), 8);
    EXPECT_STREQ(str.c_str(), "Assigned");
}

TEST_F(StringTest, CStringPointerAssignment) {
    qb::string<30> str;
    const char    *cstr = "Pointer";
    str                 = cstr;
    EXPECT_EQ(str.size(), 7);
    EXPECT_STREQ(str.c_str(), "Pointer");
}

TEST_F(StringTest, CharacterAssignment) {
    qb::string<30> str;
    str = 'X';
    EXPECT_EQ(str.size(), 1);
    EXPECT_STREQ(str.c_str(), "X");
}

TEST_F(StringTest, StdStringAssignment) {
    qb::string<30> str;
    std::string    std_str = "Standard";
    str                    = std_str;
    EXPECT_EQ(str.size(), 8);
    EXPECT_STREQ(str.c_str(), "Standard");
}

// Element access tests

TEST_F(StringTest, IndexOperator) {
    qb::string<30> str("Hello");
    EXPECT_EQ(str[0], 'H');
    EXPECT_EQ(str[1], 'e');
    EXPECT_EQ(str[4], 'o');

    // Modify through index
    str[0] = 'h';
    EXPECT_EQ(str[0], 'h');
    EXPECT_STREQ(str.c_str(), "hello");
}

TEST_F(StringTest, AtMethod) {
    qb::string<30> str("Hello");
    EXPECT_EQ(str.at(0), 'H');
    EXPECT_EQ(str.at(4), 'o');

    // Test bounds checking
    EXPECT_THROW(str.at(5), std::out_of_range);
    EXPECT_THROW(str.at(100), std::out_of_range);
}

TEST_F(StringTest, FrontAndBack) {
    qb::string<30> str("Hello");
    EXPECT_EQ(str.front(), 'H');
    EXPECT_EQ(str.back(), 'o');

    // Modify front and back
    str.front() = 'h';
    str.back()  = 'O';
    EXPECT_STREQ(str.c_str(), "hellO");
}

TEST_F(StringTest, DataAndCStr) {
    qb::string<30> str("Hello");
    EXPECT_STREQ(str.data(), "Hello");
    EXPECT_STREQ(str.c_str(), "Hello");
    EXPECT_EQ(str.data(), str.c_str());
}

// Iterator tests

TEST_F(StringTest, Iterators) {
    qb::string<30> str("Hello");

    // Forward iteration
    std::string result;
    for (auto it = str.begin(); it != str.end(); ++it) {
        result += *it;
    }
    EXPECT_EQ(result, "Hello");

    // Range-based for loop
    result.clear();
    for (char c : str) {
        result += c;
    }
    EXPECT_EQ(result, "Hello");
}

TEST_F(StringTest, ReverseIterators) {
    qb::string<30> str("Hello");

    std::string result;
    for (auto it = str.rbegin(); it != str.rend(); ++it) {
        result += *it;
    }
    EXPECT_EQ(result, "olleH");
}

TEST_F(StringTest, ConstIterators) {
    const qb::string<30> str("Hello");

    std::string result;
    for (auto it = str.cbegin(); it != str.cend(); ++it) {
        result += *it;
    }
    EXPECT_EQ(result, "Hello");

    result.clear();
    for (auto it = str.crbegin(); it != str.crend(); ++it) {
        result += *it;
    }
    EXPECT_EQ(result, "olleH");
}

// Capacity tests

TEST_F(StringCapacityTest, CapacityAndSize) {
    EXPECT_EQ(small_str_.capacity(), 10);
    EXPECT_EQ(small_str_.max_size(), 10);
    EXPECT_EQ(small_str_.size(), 4);
    EXPECT_EQ(small_str_.length(), 4);
    EXPECT_FALSE(small_str_.empty());

    qb::string<10> empty;
    EXPECT_TRUE(empty.empty());
    EXPECT_EQ(empty.size(), 0);
}

TEST_F(StringCapacityTest, Resize) {
    qb::string<20> str("Hello");

    // Resize larger
    str.resize(10, 'X');
    EXPECT_EQ(str.size(), 10);
    EXPECT_STREQ(str.c_str(), "HelloXXXXX");

    // Resize smaller
    str.resize(3);
    EXPECT_EQ(str.size(), 3);
    EXPECT_STREQ(str.c_str(), "Hel");

    // Resize to capacity limit
    str.resize(20, 'Y');
    EXPECT_EQ(str.size(), 20);
}

// Operations tests

TEST_F(StringTest, Clear) {
    qb::string<30> str("Hello World");
    EXPECT_FALSE(str.empty());

    str.clear();
    EXPECT_TRUE(str.empty());
    EXPECT_EQ(str.size(), 0);
    EXPECT_STREQ(str.c_str(), "");
}

TEST_F(StringTest, Swap) {
    qb::string<30> str1("First");
    qb::string<30> str2("Second");

    str1.swap(str2);
    EXPECT_STREQ(str1.c_str(), "Second");
    EXPECT_STREQ(str2.c_str(), "First");

    // Test non-member swap
    swap(str1, str2);
    EXPECT_STREQ(str1.c_str(), "First");
    EXPECT_STREQ(str2.c_str(), "Second");
}

// `swap()` is the one mutator whose correctness is not obvious from reading it. It saves the raw
// `std::array` base, copy-assigns the *other string* over `*this`, then assigns that saved BASE
// back into `other` — and a `base_t` reaches `assign(data(), size())` with `size()` being the full
// capacity, so `other` is rebuilt from `_Size` bytes and only afterwards has its `_size` restored.
// Nothing re-terminates at the restored length: `c_str()` stays correct **only** because the
// memcpy carries the source's own embedded '\0' along with the characters.
//
// The case above cannot show that. Both operands are short and both buffers were freshly
// value-initialised, so every byte past the text is already 0 — any implementation looks right.
// These two pin the property where it actually has to hold.

TEST_F(StringTest, SwapIntoASlotThatHeldALongerStringStaysTerminated) {
    qb::string<30> shorter("ab");
    qb::string<30> longer("XXXXXXXXXXXXXXXXXXXXXXXXXXXX"); // 28 chars: fills the tail with non-zero

    longer.swap(shorter);

    EXPECT_EQ(longer.size(), 2u);
    EXPECT_STREQ(longer.c_str(), "ab") << "the short text landed in a buffer whose tail still holds the previous, longer "
                                          "content, and nothing re-terminated at the restored length";
    EXPECT_EQ(std::strlen(longer.c_str()), longer.size()) << "c_str() must not run past size()";

    EXPECT_EQ(shorter.size(), 28u);
    EXPECT_STREQ(shorter.c_str(), "XXXXXXXXXXXXXXXXXXXXXXXXXXXX");
    EXPECT_EQ(std::strlen(shorter.c_str()), shorter.size());
}

TEST_F(StringTest, SwapAtFullCapacityStaysTerminated) {
    // At exactly _Size there is no '\0' anywhere inside [0, _Size): the terminator lives in the
    // one extra slot the array carries, which is precisely the byte the base-assignment path
    // rewrites. Both directions must survive.
    qb::string<10> full("0123456789");
    qb::string<10> tiny("z");
    ASSERT_EQ(full.size(), 10u);
    ASSERT_EQ(full.capacity(), 10u);

    full.swap(tiny);

    EXPECT_EQ(full.size(), 1u);
    EXPECT_STREQ(full.c_str(), "z");
    EXPECT_EQ(std::strlen(full.c_str()), 1u);

    EXPECT_EQ(tiny.size(), 10u);
    EXPECT_STREQ(tiny.c_str(), "0123456789");
    EXPECT_EQ(std::strlen(tiny.c_str()), 10u) << "a full-capacity string must stay terminated by the extra array slot";
}

// String operations tests

TEST_F(StringTest, Substr) {
    qb::string<30> str("Hello World");

    auto sub1 = str.substr(0, 5);
    EXPECT_STREQ(sub1.c_str(), "Hello");

    auto sub2 = str.substr(6);
    EXPECT_STREQ(sub2.c_str(), "World");

    auto sub3 = str.substr(6, 3);
    EXPECT_STREQ(sub3.c_str(), "Wor");

    // Test out of bounds
    EXPECT_THROW(str.substr(20), std::out_of_range);
}

TEST_F(StringTest, Compare) {
    qb::string<30> str1("Apple");
    qb::string<30> str2("Banana");
    qb::string<30> str3("Apple");

    EXPECT_LT(str1.compare(str2), 0);
    EXPECT_GT(str2.compare(str1), 0);
    EXPECT_EQ(str1.compare(str3), 0);

    // Compare with C-string
    EXPECT_EQ(str1.compare("Apple"), 0);
    EXPECT_LT(str1.compare("Banana"), 0);

    // Compare substring
    EXPECT_EQ(str1.compare(0, 3, qb::string<10>("App")), 0);
}

// Search operations tests

TEST_F(StringAlgorithmTest, Find) {
    // Let's verify the actual test string: "Hello, World! This is a test string."
    // Positions:                            0123456789012345678901234567890123456
    //                                                  1         2         3

    // Find substring
    EXPECT_EQ(test_str_.find("World"), 7);
    EXPECT_EQ(test_str_.find("test"), 24); // "test" starts at position 24
    EXPECT_EQ(test_str_.find("notfound"), qb::string<50>::npos);

    // Find character
    EXPECT_EQ(test_str_.find('H'), 0);
    EXPECT_EQ(test_str_.find('!'), 12);
    EXPECT_EQ(test_str_.find('z'), qb::string<50>::npos);

    // Find with position
    EXPECT_EQ(test_str_.find('i', 20), 32); // 'i' in "string" at position 32
    EXPECT_EQ(test_str_.find('e', 2), 25);  // 'e' in "test" at position 25
}

TEST_F(StringAlgorithmTest, RFind) {
    // Find last occurrence
    EXPECT_EQ(test_str_.rfind('s'), 29); // Last 's' in "string" at position 29
    EXPECT_EQ(test_str_.rfind('i'), 32); // Last 'i' in "string" at position 32
    EXPECT_EQ(test_str_.rfind('z'), qb::string<50>::npos);

    // Find last with position
    EXPECT_EQ(test_str_.rfind('s', 30), 29); // Last 's' before/at position 30 is at 29

    // Find last substring
    qb::string<20> substr_test("test is test");
    EXPECT_EQ(substr_test.rfind("test"), 8);
}

TEST_F(StringAlgorithmTest, FindRFindStartPositionEdges) {
    // Corpus: "Hello, World! This is a test string." (size 36)
    const auto sz = test_str_.size();
    ASSERT_EQ(sz, 36u);

    // find(char, pos): pos >= size() short-circuits to npos (contract at string.h: `if (pos >=
    // _size) return npos`), so a start past the end finds nothing even for a present character.
    EXPECT_EQ(test_str_.find('H', sz), qb::string<50>::npos) << "pos == size() -> npos";
    EXPECT_EQ(test_str_.find('H', sz + 100), qb::string<50>::npos) << "pos past end -> npos";
    // pos == 0 searches the whole string (the leading 'H' is at index 0).
    EXPECT_EQ(test_str_.find('H', 0), 0u);
    // find substring with pos past end -> npos.
    EXPECT_EQ(test_str_.find("World", sz), qb::string<50>::npos);

    // Substring present only BEFORE pos -> not found (search starts at data()+pos): "Hello" is at
    // index 0, so searching from pos 1 onward must miss it.
    EXPECT_EQ(test_str_.find("Hello"), 0u);
    EXPECT_EQ(test_str_.find("Hello", 1), qb::string<50>::npos) << "a match wholly before pos must not be reported";

    // rfind(char) clamps pos to size()-1 rather than short-circuiting, so a past-end pos still
    // searches the whole string and finds the LAST occurrence (here 'l' at index 10 in "World").
    EXPECT_EQ(test_str_.rfind('l', sz + 100), test_str_.rfind('l'));
    // rfind with pos 0 only considers index 0 ('H'); a char absent there -> npos.
    EXPECT_EQ(test_str_.rfind('H', 0), 0u);
    EXPECT_EQ(test_str_.rfind('e', 0), qb::string<50>::npos) << "rfind(ch, 0) inspects only index 0";
}

// Modifiers tests

TEST_F(StringTest, Append) {
    qb::string<30> str("Hello");

    // Append string
    str.append(qb::string<10>(" World"));
    EXPECT_STREQ(str.c_str(), "Hello World");

    // Append C-string
    str.append("!");
    EXPECT_STREQ(str.c_str(), "Hello World!");

    // Append character
    str.append('?');
    EXPECT_STREQ(str.c_str(), "Hello World!?");

    // Append multiple characters
    str.append(3, 'X');
    EXPECT_STREQ(str.c_str(), "Hello World!?XXX");
}

TEST_F(StringTest, PushBackPopBack) {
    qb::string<30> str("Hello");

    str.push_back('!');
    EXPECT_STREQ(str.c_str(), "Hello!");
    EXPECT_EQ(str.size(), 6);

    str.pop_back();
    EXPECT_STREQ(str.c_str(), "Hello");
    EXPECT_EQ(str.size(), 5);

    // Test pop_back on empty string
    qb::string<10> empty;
    empty.pop_back(); // Should not crash
    EXPECT_TRUE(empty.empty());
}

TEST_F(StringTest, AppendOperators) {
    qb::string<30> str("Hello");

    str += " World";
    EXPECT_STREQ(str.c_str(), "Hello World");

    str += '!';
    EXPECT_STREQ(str.c_str(), "Hello World!");

    str += qb::string<10>(" Test");
    EXPECT_STREQ(str.c_str(), "Hello World! Test");
}

// C++20 features tests

TEST_F(StringTest, StartsWith) {
    qb::string<30> str("Hello World");

    EXPECT_TRUE(str.starts_with("Hello"));
    EXPECT_TRUE(str.starts_with("H"));
    EXPECT_TRUE(str.starts_with('H'));
    EXPECT_FALSE(str.starts_with("World"));
    EXPECT_FALSE(str.starts_with('W'));

    qb::string<10> prefix("Hello");
    EXPECT_TRUE(str.starts_with(prefix));
}

TEST_F(StringTest, EndsWith) {
    qb::string<30> str("Hello World");

    EXPECT_TRUE(str.ends_with("World"));
    EXPECT_TRUE(str.ends_with("d"));
    EXPECT_TRUE(str.ends_with('d'));
    EXPECT_FALSE(str.ends_with("Hello"));
    EXPECT_FALSE(str.ends_with('H'));

    qb::string<10> suffix("World");
    EXPECT_TRUE(str.ends_with(suffix));
}

TEST_F(StringTest, Contains) {
    qb::string<30> str("Hello World");

    EXPECT_TRUE(str.contains("World"));
    EXPECT_TRUE(str.contains("llo"));
    EXPECT_TRUE(str.contains('o'));
    EXPECT_FALSE(str.contains("xyz"));
    EXPECT_FALSE(str.contains('z'));

    qb::string<10> substring("llo W");
    EXPECT_TRUE(str.contains(substring));
}

// Comparison operators tests

TEST_F(StringTest, EqualityOperators) {
    qb::string<30> str1("Hello");
    qb::string<30> str2("Hello");
    qb::string<30> str3("World");

    EXPECT_TRUE(str1 == str2);
    EXPECT_FALSE(str1 == str3);
    EXPECT_TRUE(str1 != str3);
    EXPECT_FALSE(str1 != str2);

    // Compare with C-string
    EXPECT_TRUE(str1 == "Hello");
    EXPECT_FALSE(str1 == "World");
    EXPECT_TRUE("Hello" == str1);
    EXPECT_FALSE("World" == str1);
}

TEST_F(StringTest, RelationalOperators) {
    qb::string<30> str1("Apple");
    qb::string<30> str2("Banana");
    qb::string<30> str3("Apple");

    EXPECT_TRUE(str1 < str2);
    EXPECT_FALSE(str2 < str1);
    EXPECT_FALSE(str1 < str3);

    EXPECT_TRUE(str2 > str1);
    EXPECT_FALSE(str1 > str2);
    EXPECT_FALSE(str1 > str3);

    EXPECT_TRUE(str1 <= str2);
    EXPECT_TRUE(str1 <= str3);
    EXPECT_FALSE(str2 <= str1);

    EXPECT_TRUE(str2 >= str1);
    EXPECT_TRUE(str1 >= str3);
    EXPECT_FALSE(str1 >= str2);

    // Compare with C-strings
    EXPECT_TRUE(str1 < "Banana");
    EXPECT_TRUE(str1 <= "Apple");
    EXPECT_TRUE(str2 > "Apple");
    EXPECT_TRUE(str1 >= "Apple");
}

// Non-member functions tests

TEST_F(StringTest, ConcatenationOperators) {
    qb::string<20> str1("Hello");
    qb::string<20> str2(" World");

    // String + String
    auto result1 = str1 + str2;
    EXPECT_STREQ(result1.c_str(), "Hello World");

    // String + C-string
    auto result2 = str1 + "!";
    EXPECT_STREQ(result2.c_str(), "Hello!");

    // C-string + String
    auto result3 = "Hi " + str2;
    EXPECT_STREQ(result3.c_str(), "Hi  World");

    // String + char
    auto result4 = str1 + '!';
    EXPECT_STREQ(result4.c_str(), "Hello!");

    // char + String
    auto result5 = '!' + str1;
    EXPECT_STREQ(result5.c_str(), "!Hello");
}

// Conversion tests

TEST_F(StringTest, StdStringConversion) {
    qb::string<30> qb_str("Hello World");

    // Implicit conversion to std::string
    std::string std_str = qb_str;
    EXPECT_EQ(std_str, "Hello World");

    // Explicit conversion to std::string
    std::string std_str2 = static_cast<std::string>(qb_str);
    EXPECT_EQ(std_str2, "Hello World");
}

TEST_F(StringTest, StringViewConversion) {
    qb::string<30> qb_str("Hello World");

    // Implicit conversion to std::string_view
    std::string_view sv = qb_str;
    EXPECT_EQ(sv, "Hello World");
    EXPECT_EQ(sv.size(), 11);
}

// Stream operators tests

TEST_F(StringTest, OutputOperator) {
    qb::string<30>     str("Hello World");
    std::ostringstream oss;
    oss << str;
    EXPECT_EQ(oss.str(), "Hello World");
}

TEST_F(StringTest, InputOperator) {
    qb::string<30>     str;
    std::istringstream iss("InputTest");
    iss >> str;
    EXPECT_STREQ(str.c_str(), "InputTest");

    // Test with spaces (should stop at first space)
    qb::string<30>     str2;
    std::istringstream iss2("Input Test");
    iss2 >> str2;
    EXPECT_STREQ(str2.c_str(), "Input");
}

// Assign method tests

TEST_F(StringTest, AssignMethods) {
    qb::string<30> str;

    // Assign C-string with length
    str.assign("Hello World", 5);
    EXPECT_STREQ(str.c_str(), "Hello");

    // Assign C-string literal
    str.assign("Test");
    EXPECT_STREQ(str.c_str(), "Test");

    // Assign std::string
    std::string std_str = "Standard";
    str.assign(std_str);
    EXPECT_STREQ(str.c_str(), "Standard");

    // Assign with fill
    str.assign(5, 'A');
    EXPECT_STREQ(str.c_str(), "AAAAA");
}

TEST_F(StringTest, AssignWithInteriorNul) {
    // assign(ptr, len) memcpy's exactly `len` bytes and records `_size = len`, so an embedded
    // '\0' is preserved as data — size() honours the explicit length, NOT strlen(). This is the
    // contract the binary-payload callers rely on; a strlen-based assign would truncate at the NUL.
    const char     raw[] = {'a', 'b', '\0', 'c', 'd'}; // 5 bytes, NUL in the middle
    qb::string<30> str;
    str.assign(raw, sizeof(raw));
    EXPECT_EQ(str.size(), 5u) << "size() must equal the explicit length, not strlen() (=2)";
    EXPECT_EQ(str[0], 'a');
    EXPECT_EQ(str[1], 'b');
    EXPECT_EQ(str[2], '\0');
    EXPECT_EQ(str[3], 'c');
    EXPECT_EQ(str[4], 'd');
    // A string_view over the buffer also spans all 5 bytes (length-driven, NUL-tolerant)...
    std::string_view sv = str;
    EXPECT_EQ(sv.size(), 5u);
    EXPECT_EQ(sv, std::string_view(raw, sizeof(raw)));
    // ...while c_str()/strlen would stop at the interior NUL (documents the divergence).
    EXPECT_EQ(std::char_traits<char>::length(str.c_str()), 2u);
    // The ctor form (ptr, len) honours the length identically.
    qb::string<30> ctor(raw, sizeof(raw));
    EXPECT_EQ(ctor.size(), 5u);
    EXPECT_EQ(ctor, str);
}

TEST_F(StringTest, ComparisonsSearchesAndOutputSpanTheWholeLength) {
    // Huly QB-386: the value is its size() characters, not the bytes up to the first NUL. Every comparison, search
    // and stream insertion below used to stop at the interior '\0' (strcmp / strstr / strlen over c_str()), so two
    // strings differing only after it compared equal and a needle past it was never found.
    const char     ab_raw[] = {'a', '\0', 'b'};
    const char     ac_raw[] = {'a', '\0', 'c'};
    qb::string<16> ab(ab_raw, sizeof(ab_raw));
    qb::string<16> ac(ac_raw, sizeof(ac_raw));

    // Equality and order: same prefix, different byte after the NUL.
    EXPECT_FALSE(ab == ac) << "the comparison stopped at the interior NUL";
    EXPECT_TRUE(ab != ac);
    EXPECT_LT(ab.compare(ac), 0);
    EXPECT_GT(ac.compare(ab), 0);
    EXPECT_TRUE(ab < ac);
    EXPECT_FALSE(ac <= ab);
    // A `char const *` operand is read up to ITS NUL and compared with OUR whole length, as std::string does.
    EXPECT_FALSE(ab == "a");
    EXPECT_TRUE(ab != "a");
    EXPECT_GT(ab.compare("a"), 0);
    EXPECT_FALSE("a" == ab);
    // String-like operands of every kind compare over both lengths.
    EXPECT_FALSE(ab == std::string("a"));
    EXPECT_TRUE(ab == std::string(ab_raw, sizeof(ab_raw)));
    EXPECT_FALSE(ab == std::string_view(ac_raw, sizeof(ac_raw)));
    EXPECT_TRUE(ab == qb::string<8>(ab_raw, sizeof(ab_raw)));
    EXPECT_FALSE(ab == qb::string<8>(ac_raw, sizeof(ac_raw)));

    // Searches run over the whole length, and a needle carrying a NUL is searched whole.
    const char     hay_raw[] = {'x', 'x', '\0', 'y', 'y'};
    qb::string<16> hay(hay_raw, sizeof(hay_raw));
    EXPECT_EQ(hay.find("yy"), 3u) << "the search stopped at the interior NUL";
    EXPECT_EQ(hay.find(qb::string<16>("yy")), 3u);
    EXPECT_TRUE(hay.contains("yy"));
    const char ny_raw[] = {'\0', 'y'};
    EXPECT_EQ(hay.find(qb::string<16>(ny_raw, sizeof(ny_raw))), 2u) << "a needle with a NUL was cut to \"\"";
    EXPECT_EQ(hay.rfind("y"), 4u);
    const char nyy_raw[] = {'\0', 'y', 'y'};
    const char nzz_raw[] = {'\0', 'z', 'z'};
    EXPECT_TRUE(hay.ends_with(qb::string<16>(nyy_raw, sizeof(nyy_raw))));
    EXPECT_FALSE(hay.ends_with(qb::string<16>(nzz_raw, sizeof(nzz_raw)))) << "a suffix with a NUL was cut to \"\"";
    qb::string<16> abx(ab_raw, sizeof(ab_raw));
    abx.append("X");
    EXPECT_TRUE(abx.starts_with(ab));
    EXPECT_FALSE(abx.starts_with(ac)) << "a prefix with a NUL was cut to \"a\"";

    // Stream insertion writes the whole length, as for a std::string.
    std::ostringstream oss;
    oss << ab;
    EXPECT_EQ(oss.str(), std::string(ab_raw, sizeof(ab_raw)));

    // NUL-free strings are unchanged (the common case): the same answers as before.
    qb::string<16> plain("abc");
    EXPECT_TRUE(plain == "abc");
    EXPECT_TRUE(plain == std::string("abc"));
    EXPECT_LT(plain.compare("abd"), 0);
    EXPECT_EQ(plain.find("bc"), 1u);
}

TEST_F(StringTest, AssignFromAViewOfItselfIsWellDefined) {
    // Huly QB-387: assign() copied with memcpy, and a source that is a view of the same string overlaps the
    // destination -- undefined behaviour, which ASan reports as memcpy-param-overlap (a release build usually
    // copies forward and gets the right bytes by luck, so the ASan run is the witness that matters).
    qb::string<16> s("abcdef");
    s.assign(std::string_view(s).substr(1)); // [1,6) onto [0,5): overlapping
    EXPECT_EQ(std::string_view(s), "bcdef");
    s.assign(std::string_view(s).substr(0, 3)); // a prefix: the same start
    EXPECT_EQ(std::string_view(s), "bcd");
    s = std::string_view(s); // the whole string onto itself
    EXPECT_EQ(std::string_view(s), "bcd");
    s.assign(s.data() + 2, 1); // one byte
    EXPECT_EQ(std::string_view(s), "d");
    s.assign(s.data(), 0); // zero bytes
    EXPECT_TRUE(s.empty());
    // Control: an unrelated source.
    const std::string other = "xyz";
    s.assign(other);
    EXPECT_EQ(std::string_view(s), "xyz");
}

TEST_F(StringTest, ACharBufferIsReadUpToItsFirstNul) {
    // A non-const char array -- a buffer -- reaches the array forms of the constructor, assign() and operator=,
    // which took `N - 1` characters of ANY array: the buffer's tail of NULs became characters (size() 31 below), and
    // with comparisons spanning the whole length (Huly QB-386) such a string no longer equals "abc". It is read as
    // std::string reads it now: up to the first NUL.
    char           buf[32] = "abc";
    qb::string<64> from_ctor(buf);
    EXPECT_EQ(from_ctor.size(), 3u) << "the buffer's tail of NULs was taken as characters";
    EXPECT_TRUE(from_ctor == "abc");
    qb::string<64> from_assign;
    from_assign.assign(buf);
    EXPECT_EQ(from_assign.size(), 3u);
    qb::string<64> from_operator;
    from_operator = buf;
    EXPECT_EQ(from_operator.size(), 3u);
    EXPECT_TRUE(from_operator == std::string("abc"));
    // A buffer with no NUL at all is read whole -- never past its end, where strlen would over-read.
    char          raw[3] = {'x', 'y', 'z'};
    qb::string<8> unterminated(raw);
    EXPECT_EQ(std::string_view(unterminated), "xyz") << "the last character of an unterminated buffer was dropped";
    // Controls. A string LITERAL is read the same either way -- the array form since Huly QB-1002, the `const char *`
    // form and its strlen before: all its characters, or those before an inner NUL, as std::string reads it.
    EXPECT_EQ(qb::string<16>("hello").size(), 5u);
    EXPECT_EQ(qb::string<16>("a\0b").size(), 1u);
    // The (pointer, length) forms keep interior NULs.
    EXPECT_EQ(qb::string<16>("a\0b", 3).size(), 3u);
}

// A stream that can write nothing: every put reports failure, so an ostream over it sets badbit.
class FailingStreambuf : public std::streambuf {
protected:
    int_type
    overflow(int_type) override {
        return traits_type::eof();
    }
    std::streamsize
    xsputn(char const *, std::streamsize) override {
        return 0;
    }
};

// operator<< and operator>> are noexcept, and a stream whose exceptions() the program enabled throws from inside
// them: that was std::terminate (the class of Huly QB-388). In a child process: the failure must stay in the
// stream's state, the target of a failed read unchanged. std::_Exit, as the type-id death tests: std::exit would run
// LeakSanitizer's exit check under ASan.
TEST(StringStreamDeathTest, AStreamThatThrowsNeverTerminatesTheInsertionOrTheExtraction) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
        {
            FailingStreambuf failing;
            std::ostream     out(&failing);
            out.exceptions(std::ios::badbit);
            const qb::string<16> payload("payload");
            out << payload; // the write fails: badbit, and a throw
            std::istringstream in("");
            in.exceptions(std::ios::failbit);
            qb::string<16> target("kept");
            in >> target; // nothing to read: failbit, and a throw
            std::_Exit(out.bad() && in.fail() && std::string_view(target) == "kept" ? 0 : 1);
        },
        ::testing::ExitedWithCode(0), "");
}

// Huly QB-1002 (1): a CONST char array -- a string literal is one -- took the `char const *` overloads of the
// constructor, assign() and operator=, whose strlen reads past an array with no NUL (std::string does the same). Every
// array now takes the bounded array forms: up to its first NUL, never past its end. The arrays sit in one object so
// the over-read is deterministic: it reads on into `after`.
TEST_F(StringTest, AConstCharArrayIsReadWithinItsBounds) {
    struct Adjacent {
        const char raw[3];
        const char after[5];
    };
    static_assert(sizeof(Adjacent) == 8, "the two arrays must be contiguous for the over-read to be deterministic");
    const Adjacent adjacent{{'x', 'y', 'z'}, {'Q', 'Q', 'Q', 'Q', '\0'}};

    const qb::string<16> constructed(adjacent.raw);
    EXPECT_EQ(std::string_view(constructed), "xyz") << "strlen read past the array, into the next one";
    qb::string<16> assigned;
    assigned.assign(adjacent.raw);
    EXPECT_EQ(std::string_view(assigned), "xyz");
    qb::string<16> operated;
    operated = adjacent.raw;
    EXPECT_EQ(std::string_view(operated), "xyz");

    // A literal is a terminated const array: read up to its (first) NUL, as before, as std::string reads it.
    EXPECT_EQ(std::string_view(qb::string<16>("hello")), "hello");
    EXPECT_EQ(qb::string<16>("a\0b").size(), 1u);
    qb::string<16> literal_assigned;
    literal_assigned = "world";
    EXPECT_EQ(std::string_view(literal_assigned), "world");
    // A pointer is still read up to its NUL, a string-like object over its size.
    const char *pointer = "pointer";
    EXPECT_EQ(std::string_view(qb::string<16>(pointer)), "pointer");
    EXPECT_EQ(qb::string<16>(std::string("s\0z", 3)).size(), 3u);
}

// Huly QB-1002 (2): extraction read the whole word into a std::string, then kept what fitted -- a long token allocated
// its whole length. It now keeps the first characters as it reads and drops the rest: the word is consumed whole,
// as for std::string, and nothing is buffered.
TEST_F(StringTest, ExtractionKeepsTheCapacityAndNeverBuffersTheWord) {
    std::istringstream in("  abcdefgh next");
    qb::string<4>      word("old");
    in >> word;
    EXPECT_EQ(std::string_view(word), "abcd");
    std::string rest;
    in >> rest;
    EXPECT_EQ(rest, "next") << "the rest of the long word was not consumed with it";

    // width() bounds the word as it bounds a std::string, and is reset.
    std::istringstream bounded("abcdefgh");
    bounded.width(3);
    qb::string<8> head;
    bounded >> head;
    EXPECT_EQ(std::string_view(head), "abc");
    EXPECT_EQ(bounded.width(), 0);
    bounded >> rest;
    EXPECT_EQ(rest, "defgh");

    // End of stream: eofbit after a word; failbit with nothing to read, the string left as it was (as std::string's).
    std::istringstream last("tail");
    qb::string<8>      tail;
    last >> tail;
    EXPECT_EQ(std::string_view(tail), "tail");
    EXPECT_TRUE(last.eof());
    EXPECT_FALSE(last.fail());
    std::string        reference("kept");
    std::istringstream nothing("   ");
    nothing >> reference;
    EXPECT_TRUE(nothing.fail());
    EXPECT_EQ(reference, "kept") << "the std::string behaviour this mirrors";
    std::istringstream nothing_again("   ");
    nothing_again >> tail;
    EXPECT_TRUE(nothing_again.fail());
    EXPECT_EQ(std::string_view(tail), "tail") << "a read that found no word changed the string";

    // Past the sentry the target holds what was read, as std::string's: nothing at a white space a noskipws stream
    // cannot skip, and the word before a throw the stream asked for (contained here, the stream keeps the failure).
    std::istringstream at_space_reference(" word");
    std::string        emptied("kept");
    at_space_reference >> std::noskipws >> emptied;
    EXPECT_TRUE(at_space_reference.fail());
    EXPECT_EQ(emptied, "") << "the std::string behaviour this mirrors";
    std::istringstream at_space(" word");
    qb::string<8>      at_space_word("kept");
    at_space >> std::noskipws >> at_space_word;
    EXPECT_TRUE(at_space.fail());
    EXPECT_TRUE(at_space_word.empty()) << "a read stopped at a white space kept the old word";
    std::istringstream throwing_reference("last");
    throwing_reference.exceptions(std::ios::eofbit);
    std::string read_before_throw;
    EXPECT_ANY_THROW(throwing_reference >> read_before_throw);
    EXPECT_EQ(read_before_throw, "last") << "the std::string behaviour this mirrors";
    std::istringstream throwing("last");
    throwing.exceptions(std::ios::eofbit);
    qb::string<8> last_word("kept");
    throwing >> last_word;
    EXPECT_TRUE(throwing.eof());
    EXPECT_EQ(std::string_view(last_word), "last") << "the word read before the eofbit throw was dropped";

    // The bound: a one-MiB word leaves the allocator alone.
    std::istringstream huge(std::string(1u << 20, 'w') + " after");
    qb::string<8>      kept;
    string_alloc_probe::bytes.store(0);
    string_alloc_probe::counting.store(true);
    huge >> kept;
    string_alloc_probe::counting.store(false);
    EXPECT_LT(string_alloc_probe::bytes.load(), std::size_t{4096}) << "the word was buffered before it was cut";
    EXPECT_EQ(std::string_view(kept), "wwwwwwww");
    huge >> rest;
    EXPECT_EQ(rest, "after");
}

// Huly QB-1002 (3): find(needle, pos) answered npos for pos == size() whatever the needle -- an empty needle is found
// at every position up to size(), as std::string finds it -- so an empty string did not contain "".
TEST_F(StringAlgorithmTest, AnEmptyNeedleIsFoundUpToTheSize) {
    const std::string mirror(test_str_);
    const auto        size = test_str_.size();
    EXPECT_EQ(test_str_.find("", size), size) << "an empty needle is found at size()";
    EXPECT_EQ(test_str_.find(qb::string<4>(), size), size);
    for (std::size_t pos = 0; pos <= size + 2; ++pos) {
        EXPECT_EQ(test_str_.find("", pos), mirror.find("", pos)) << pos;
        EXPECT_EQ(test_str_.find("ing", pos), mirror.find("ing", pos)) << pos;
        EXPECT_EQ(test_str_.find(qb::string<4>("is"), pos), mirror.find("is", pos)) << pos;
        EXPECT_EQ(test_str_.find('.', pos), mirror.find('.', pos)) << pos;
    }

    qb::string<8> empty;
    EXPECT_EQ(empty.find(""), 0u);
    EXPECT_TRUE(empty.contains("")) << "std::string(\"\").find(\"\") is 0";
    EXPECT_TRUE(empty.contains(qb::string<8>()));
    EXPECT_TRUE(empty_str_.contains(""));
    EXPECT_FALSE(empty.contains("x"));
}

// Huly QB-1002 (decision 2): every other member that takes a C string -- compare, find, rfind, append, +=,
// starts_with, ends_with, contains, the six comparisons, == and != with the array on the left, operator+ on either
// side -- took a char array as `const char *` and read it with strlen, past the end of an array that has no NUL. Each
// has a bounded array form now. Two witnesses per operation: `adjacent.raw`, an unterminated array followed by more
// text in the same object, turns the over-read into a wrong answer in any build; `lone`, an unterminated array alone
// on the stack, turns it into an AddressSanitizer stack-buffer-overflow under the sanitize preset.
struct UnterminatedOperand {
    const char raw[3];
    const char after[5];
};
static_assert(sizeof(UnterminatedOperand) == 8, "the arrays must be contiguous for the over-read to land in `after`");

template <typename Check>
void
against_unterminated_arrays(Check check) {
    const UnterminatedOperand adjacent{{'x', 'y', 'z'}, {'Q', 'Q', 'Q', 'Q', '\0'}};
    check(adjacent.raw, "followed by \"QQQQ\" in the same object");
    const char lone[3] = {'x', 'y', 'z'};
    check(lone, "alone on the stack");
}

TEST(StringArrayOperand, CompareReadsTheArrayWithinItsBounds) {
    against_unterminated_arrays(
        [](const char (&operand)[3], const char *where) { EXPECT_EQ(qb::string<16>("xyz").compare(operand), 0) << where; });
}

TEST(StringArrayOperand, FindReadsTheArrayWithinItsBounds) {
    against_unterminated_arrays([](const char (&operand)[3], const char *where) {
        const qb::string<16> haystack("axyzb");
        EXPECT_EQ(haystack.find(operand), 1u) << where;
        EXPECT_EQ(haystack.find(operand, 2), qb::string<16>::npos) << where;
    });
}

TEST(StringArrayOperand, RfindReadsTheArrayWithinItsBounds) {
    against_unterminated_arrays([](const char (&operand)[3], const char *where) {
        const qb::string<16> haystack("xyzxyz");
        EXPECT_EQ(haystack.rfind(operand), 3u) << where;
        EXPECT_EQ(haystack.rfind(operand, 2), 0u) << where;
    });
}

TEST(StringArrayOperand, AppendReadsTheArrayWithinItsBounds) {
    against_unterminated_arrays([](const char (&operand)[3], const char *where) {
        qb::string<16> text("a");
        text.append(operand);
        EXPECT_EQ(std::string_view(text), "axyz") << where;
    });
}

TEST(StringArrayOperand, PlusAssignReadsTheArrayWithinItsBounds) {
    against_unterminated_arrays([](const char (&operand)[3], const char *where) {
        qb::string<16> text("a");
        text += operand;
        EXPECT_EQ(std::string_view(text), "axyz") << where;
    });
}

TEST(StringArrayOperand, StartsWithReadsTheArrayWithinItsBounds) {
    against_unterminated_arrays(
        [](const char (&operand)[3], const char *where) { EXPECT_TRUE(qb::string<16>("xyz!").starts_with(operand)) << where; });
}

TEST(StringArrayOperand, EndsWithReadsTheArrayWithinItsBounds) {
    against_unterminated_arrays(
        [](const char (&operand)[3], const char *where) { EXPECT_TRUE(qb::string<16>("!xyz").ends_with(operand)) << where; });
}

TEST(StringArrayOperand, ContainsReadsTheArrayWithinItsBounds) {
    against_unterminated_arrays(
        [](const char (&operand)[3], const char *where) { EXPECT_TRUE(qb::string<16>("axyzb").contains(operand)) << where; });
}

TEST(StringArrayOperand, EqualityReadsTheArrayWithinItsBounds) {
    against_unterminated_arrays([](const char (&operand)[3], const char *where) { EXPECT_TRUE(qb::string<16>("xyz") == operand) << where; });
}

TEST(StringArrayOperand, InequalityReadsTheArrayWithinItsBounds) {
    against_unterminated_arrays([](const char (&operand)[3], const char *where) { EXPECT_FALSE(qb::string<16>("xyz") != operand) << where; });
}

TEST(StringArrayOperand, LessThanReadsTheArrayWithinItsBounds) {
    against_unterminated_arrays([](const char (&operand)[3], const char *where) {
        EXPECT_FALSE(qb::string<16>("xyz") < operand) << where; // "xyz" < "xyzQQQQ" when read past the end
    });
}

TEST(StringArrayOperand, GreaterThanReadsTheArrayWithinItsBounds) {
    against_unterminated_arrays([](const char (&operand)[3], const char *where) {
        EXPECT_TRUE(qb::string<16>("xyzA") > operand) << where; // 'A' < 'Q'
    });
}

TEST(StringArrayOperand, LessOrEqualReadsTheArrayWithinItsBounds) {
    against_unterminated_arrays([](const char (&operand)[3], const char *where) { EXPECT_FALSE(qb::string<16>("xyzA") <= operand) << where; });
}

TEST(StringArrayOperand, GreaterOrEqualReadsTheArrayWithinItsBounds) {
    against_unterminated_arrays([](const char (&operand)[3], const char *where) { EXPECT_TRUE(qb::string<16>("xyz") >= operand) << where; });
}

TEST(StringArrayOperand, EqualityWithTheArrayOnTheLeftReadsItWithinItsBounds) {
    against_unterminated_arrays([](const char (&operand)[3], const char *where) { EXPECT_TRUE(operand == qb::string<16>("xyz")) << where; });
}

TEST(StringArrayOperand, InequalityWithTheArrayOnTheLeftReadsItWithinItsBounds) {
    against_unterminated_arrays([](const char (&operand)[3], const char *where) { EXPECT_FALSE(operand != qb::string<16>("xyz")) << where; });
}

TEST(StringArrayOperand, ConcatenationOnTheRightReadsTheArrayWithinItsBounds) {
    against_unterminated_arrays(
        [](const char (&operand)[3], const char *where) { EXPECT_EQ(std::string_view(qb::string<16>("a") + operand), "axyz") << where; });
}

TEST(StringArrayOperand, ConcatenationOnTheLeftReadsTheArrayWithinItsBounds) {
    against_unterminated_arrays(
        [](const char (&operand)[3], const char *where) { EXPECT_EQ(std::string_view(operand + qb::string<16>("b")), "xyzb") << where; });
}

// A buffer (a non-const array) takes the same array forms: the reference to const binds it, and the pointer form,
// deduced as `char *`, is the less specialised.
TEST(StringArrayOperand, ACharBufferTakesTheArrayFormsToo) {
    struct {
        char raw[3];
        char after[5];
    } buffer{{'x', 'y', 'z'}, {'Q', 'Q', 'Q', 'Q', '\0'}};
    static_assert(sizeof(buffer) == 8, "contiguous");
    qb::string<16> text("a");
    text.append(buffer.raw);
    text += buffer.raw;
    EXPECT_EQ(std::string_view(text), "axyzxyz");
    EXPECT_EQ(qb::string<16>("axyzb").find(buffer.raw), 1u);
    EXPECT_TRUE(qb::string<16>("xyz") == buffer.raw);
    EXPECT_TRUE(buffer.raw == qb::string<16>("xyz"));
    EXPECT_EQ(qb::string<16>("xyz").compare(buffer.raw), 0);
    EXPECT_EQ(std::string_view(qb::string<16>("a") + buffer.raw), "axyz");
}

// The call shapes callers write still compile, unambiguously, and answer as std::string answers: a literal, a
// terminated const array, a buffer, `const char *`, `char *`, std::string, std::string_view, a qb::string of this
// capacity and of another, a char. A requires-expression is false for an ambiguous call, so each row below is also
// the proof that the array forms and the pointer forms never tie.
using S16 = qb::string<16>;
using S8  = qb::string<8>;

template <typename A>
constexpr bool every_search_takes = requires(S16 const &s, A &&a) {
    s.compare(a);
    s.find(a);
    s.find(a, std::size_t{1});
    s.starts_with(a);
    s.ends_with(a);
    s.contains(a);
};
template <typename A>
constexpr bool rfind_takes = requires(S16 const &s, A &&a) { s.rfind(a); };
template <typename A>
constexpr bool every_append_takes = requires(S16 &s, A &&a) {
    s.append(a);
    s += a;
};
template <typename A>
constexpr bool equality_takes = requires(S16 const &s, A &&a) {
    s == a;
    s != a;
    a == s;
    a != s;
};
template <typename A>
constexpr bool ordering_takes = requires(S16 const &s, A &&a) {
    s < a;
    s > a;
    s <= a;
    s >= a;
};
template <typename A>
constexpr bool concatenation_takes = requires(S16 const &s, A &&a) {
    s + a;
    a + s;
};

// clang-format off
static_assert(every_search_takes<const char (&)[4]> && every_search_takes<const char (&)[3]> && every_search_takes<char (&)[8]>
              && every_search_takes<const char *&> && every_search_takes<char *&> && every_search_takes<const std::string &>
              && every_search_takes<std::string_view &> && every_search_takes<const S16 &> && every_search_takes<const S8 &>);
static_assert(requires(S16 const &s, char c) {
    s.find(c);
    s.rfind(c);
    s.starts_with(c);
    s.ends_with(c);
    s.contains(c);
});
static_assert(rfind_takes<const char (&)[4]> && rfind_takes<const char (&)[3]> && rfind_takes<char (&)[8]>
              && rfind_takes<const char *&> && rfind_takes<char *&> && rfind_takes<char &>);
static_assert(every_append_takes<const char (&)[4]> && every_append_takes<const char (&)[3]> && every_append_takes<char (&)[8]>
              && every_append_takes<const char *&> && every_append_takes<char *&> && every_append_takes<const std::string &>
              && every_append_takes<std::string_view &> && every_append_takes<const S16 &> && every_append_takes<const S8 &>
              && every_append_takes<char &>);
static_assert(equality_takes<const char (&)[4]> && equality_takes<const char (&)[3]> && equality_takes<char (&)[8]>
              && equality_takes<const char *&> && equality_takes<char *&> && equality_takes<const std::string &>
              && equality_takes<std::string_view &> && equality_takes<const S16 &> && equality_takes<const S8 &>);
static_assert(ordering_takes<const char (&)[4]> && ordering_takes<const char (&)[3]> && ordering_takes<char (&)[8]>
              && ordering_takes<const char *&> && ordering_takes<char *&> && ordering_takes<const std::string &>
              && ordering_takes<const S16 &> && ordering_takes<const S8 &>);
static_assert(concatenation_takes<const char (&)[4]> && concatenation_takes<const char (&)[3]> && concatenation_takes<char (&)[8]>
              && concatenation_takes<const char *&> && concatenation_takes<char *&> && concatenation_takes<const S16 &>
              && concatenation_takes<const S8 &> && concatenation_takes<char &>);
// clang-format on

// Which arrays may have their last element read (the MSVC length tests it to keep a literal's strlen folded): only a
// const array, a literal included -- initialised whole by rule. A mutable buffer's tail may never have been written,
// so it is read by strnlen alone. `element_of` has the head and the parameter of every array form of qb::string, so
// the language deduces here the element type each of them deduces for the same argument.
template <qb::internal::char_element C, std::size_t N>
C *element_of(C (&)[N]);

template <typename A>
constexpr bool reads_last_element_of = qb::internal::reads_last_element<std::remove_pointer_t<decltype(element_of(std::declval<A>()))>>;

static_assert(reads_last_element_of<const char (&)[4]>, "a literal: initialised whole");
static_assert(reads_last_element_of<const char (&)[32]>, "a const array: initialised whole");
static_assert(!reads_last_element_of<char (&)[32]>, "a mutable buffer: its last element is never read");
static_assert(!reads_last_element_of<char (&)[1]>, "a mutable buffer of one");

// A buffer snprintf filled: "key" and its NUL, the rest never written. Every operation reads it up to its NUL, through
// strnlen (valgrind memcheck reported the last-element test on such a buffer: a branch on an uninitialised value).
TEST(StringArrayOperand, ABufferWhoseTailWasNeverWrittenIsReadUpToItsNul) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%s", "key");
    const S16 key("key");
    EXPECT_TRUE(key == buffer && buffer == key && !(key != buffer) && !(buffer != key));
    EXPECT_EQ(key.compare(buffer), 0);
    EXPECT_FALSE(key < buffer || key > buffer);
    EXPECT_TRUE(key <= buffer && key >= buffer);
    EXPECT_EQ(S16("a key").find(buffer), 2u);
    EXPECT_EQ(S16("keykey").rfind(buffer), 3u);
    EXPECT_TRUE(key.starts_with(buffer) && key.ends_with(buffer) && key.contains(buffer));
    S16 text(buffer);
    text.append(buffer);
    text += buffer;
    EXPECT_EQ(std::string_view(text), "keykeykey");
    S16 assigned;
    assigned = buffer;
    EXPECT_EQ(std::string_view(assigned), "key");
    assigned.assign(buffer);
    EXPECT_EQ(std::string_view(assigned), "key");
    EXPECT_EQ(std::string_view(key + buffer), "keykey");
    EXPECT_EQ(std::string_view(buffer + key), "keykey");
}

TEST(StringArrayOperand, TheUsualCallShapesAnswerAsStdString) {
    const S16         s("abcabc");
    const std::string mirror("abcabc");
    const char        terminated[] = "bc";
    char              buffer[8]    = "bc";
    const char       *const_ptr    = "bc";
    char             *ptr          = buffer;
    const std::string str("bc");
    std::string_view  view("bc");
    const S16         same("bc");
    const S8          other("bc");
    const auto        sign = [](int v) {
        return (v > 0) - (v < 0);
    };

    // Searches, every shape against the std::string answer.
    const std::string_view needle("bc");
    {
        EXPECT_EQ(sign(s.compare("bc")), sign(mirror.compare(needle)));
        EXPECT_EQ(sign(s.compare(terminated)), sign(mirror.compare(needle)));
        EXPECT_EQ(sign(s.compare(buffer)), sign(mirror.compare(needle)));
        EXPECT_EQ(sign(s.compare(const_ptr)), sign(mirror.compare(needle)));
        EXPECT_EQ(sign(s.compare(ptr)), sign(mirror.compare(needle)));
        EXPECT_EQ(sign(s.compare(str)), sign(mirror.compare(needle)));
        EXPECT_EQ(sign(s.compare(view)), sign(mirror.compare(needle)));
        EXPECT_EQ(sign(s.compare(same)), sign(mirror.compare(needle)));
        EXPECT_EQ(sign(s.compare(other)), sign(mirror.compare(needle)));
        for (std::size_t pos = 0; pos <= s.size() + 1; ++pos) {
            EXPECT_EQ(s.find("bc", pos), mirror.find(needle, pos)) << pos;
            EXPECT_EQ(s.find(terminated, pos), mirror.find(needle, pos)) << pos;
            EXPECT_EQ(s.find(buffer, pos), mirror.find(needle, pos)) << pos;
            EXPECT_EQ(s.find(const_ptr, pos), mirror.find(needle, pos)) << pos;
            EXPECT_EQ(s.find(ptr, pos), mirror.find(needle, pos)) << pos;
            EXPECT_EQ(s.find(same, pos), mirror.find(needle, pos)) << pos;
            EXPECT_EQ(s.rfind("bc", pos), mirror.rfind(needle, pos)) << pos;
            EXPECT_EQ(s.rfind(terminated, pos), mirror.rfind(needle, pos)) << pos;
            EXPECT_EQ(s.rfind(buffer, pos), mirror.rfind(needle, pos)) << pos;
            EXPECT_EQ(s.rfind(const_ptr, pos), mirror.rfind(needle, pos)) << pos;
            EXPECT_EQ(s.rfind(ptr, pos), mirror.rfind(needle, pos)) << pos;
        }
    }
    EXPECT_EQ(s.find('c'), mirror.find('c'));
    EXPECT_EQ(s.rfind('c'), mirror.rfind('c'));
    EXPECT_TRUE(s.starts_with("ab") && s.starts_with('a') && !s.starts_with(terminated) && !s.starts_with(const_ptr));
    EXPECT_TRUE(s.ends_with(terminated) && s.ends_with(buffer) && s.ends_with(const_ptr) && s.ends_with(ptr) && s.ends_with(same)
                && s.ends_with(str) && s.ends_with(view) && s.ends_with('c'));
    EXPECT_TRUE(s.contains(terminated) && s.contains(buffer) && s.contains(const_ptr) && s.contains(ptr) && s.contains(same) && s.contains(str)
                && s.contains(view) && !s.contains('z'));

    // Comparisons, both sides.
    EXPECT_TRUE(same == "bc" && same == terminated && same == buffer && same == const_ptr && same == ptr && same == str && same == view
                && same == other && same == same);
    EXPECT_TRUE("bc" == same && terminated == same && buffer == same && const_ptr == same && ptr == same && str == same && view == same);
    EXPECT_FALSE(same != "bc" || same != terminated || same != const_ptr || same != str || same != other || terminated != same
                 || const_ptr != same);
    EXPECT_TRUE(s < "b" && s < terminated && s < buffer && s < const_ptr && s < ptr && s < str && s < same && s < other);
    EXPECT_TRUE(s <= "b" && s <= terminated && !(s > terminated) && !(s >= const_ptr) && same >= terminated && same <= buffer);

    // Appends and concatenations: what each shape adds.
    S16 text("x");
    text.append("y").append(terminated).append(buffer).append(const_ptr).append(ptr).append('!');
    EXPECT_EQ(std::string_view(text), "xybcbcbcbc!");
    S16 more;
    more += "<";
    more += terminated;
    more += buffer;
    more += const_ptr;
    more += ptr;
    more += str;
    more += view;
    EXPECT_EQ(std::string_view(more), "<bcbcbcbcbcbc");
    EXPECT_EQ(std::string_view(same + "!"), "bc!");
    EXPECT_EQ(std::string_view(same + terminated), "bcbc");
    EXPECT_EQ(std::string_view(same + buffer), "bcbc");
    EXPECT_EQ(std::string_view(same + const_ptr), "bcbc");
    EXPECT_EQ(std::string_view(same + ptr), "bcbc");
    EXPECT_EQ(std::string_view(same + other), "bcbc");
    EXPECT_EQ(std::string_view(same + '!'), "bc!");
    EXPECT_EQ(std::string_view("<" + same), "<bc");
    EXPECT_EQ(std::string_view(terminated + same), "bcbc");
    EXPECT_EQ(std::string_view(buffer + same), "bcbc");
    EXPECT_EQ(std::string_view(const_ptr + same), "bcbc");
    EXPECT_EQ(std::string_view(ptr + same), "bcbc");
    EXPECT_EQ(std::string_view('<' + same), "<bc");
}

// Edge cases and error handling

TEST_F(StringTest, EmptyStringOperations) {
    qb::string<30> empty;

    EXPECT_EQ(empty.find('a'), qb::string<30>::npos);
    EXPECT_EQ(empty.rfind('a'), qb::string<30>::npos);
    EXPECT_FALSE(empty.contains('a'));
    EXPECT_FALSE(empty.starts_with('a'));
    EXPECT_FALSE(empty.ends_with('a'));

    // Operations on empty string should not crash
    auto substr = empty.substr(0, 0);
    EXPECT_TRUE(substr.empty());

    empty.clear();    // Should not crash
    empty.pop_back(); // Should not crash
}

TEST_F(StringTest, CapacityLimits) {
    qb::string<5> small;

    // Test that operations respect capacity limits
    small.assign(10, 'X');
    EXPECT_EQ(small.size(), 5);
    EXPECT_STREQ(small.c_str(), "XXXXX");

    small.clear();
    small.append("This is too long");
    EXPECT_EQ(small.size(), 5);
    EXPECT_STREQ(small.c_str(), "This ");

    small.clear();
    small = "Also too long";
    EXPECT_EQ(small.size(), 5);
}

TEST_F(StringTest, LargeString) {
    qb::string<1000> large;
    std::string      test_data(500, 'A');

    large = test_data;
    EXPECT_EQ(large.size(), 500);
    EXPECT_EQ(large.find('A'), 0);
    EXPECT_EQ(large.rfind('A'), 499);
    EXPECT_TRUE(large.contains('A'));
    EXPECT_TRUE(large.starts_with('A'));
    EXPECT_TRUE(large.ends_with('A'));
}

// Performance and optimization tests

TEST_F(StringTest, SmallStringOptimization) {
    // Test with different sizes to verify size_type optimization
    qb::string<255>   medium_str("Medium string");
    qb::string<65535> large_str("Large string");
    qb::string<10>    tiny_str("Tiny");

    EXPECT_EQ(medium_str.size(), 13);
    EXPECT_EQ(large_str.size(), 12);
    EXPECT_EQ(tiny_str.size(), 4);

    // Verify all work correctly
    EXPECT_STREQ(medium_str.c_str(), "Medium string");
    EXPECT_STREQ(large_str.c_str(), "Large string");
    EXPECT_STREQ(tiny_str.c_str(), "Tiny");
}

TEST_F(StringTest, ConstexprSupport) {
    // Test constexpr construction where possible
    constexpr qb::string<10> const_str;
    EXPECT_TRUE(const_str.empty());
    EXPECT_EQ(const_str.size(), 0);
    EXPECT_EQ(const_str.max_size(), 10);
}

} // namespace