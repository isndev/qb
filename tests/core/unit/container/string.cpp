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
 * cases (pos past the end, pos 0, and a substring present only before pos).
 */

#include <sstream>
#include <string>

#include <cstring>
#include <gtest/gtest.h>
#include <qb/string.h>

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