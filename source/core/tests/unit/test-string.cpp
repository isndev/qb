/**
 * @file test-string.cpp
 * @brief Unit tests for qb::string fixed-size string implementation
 *
 * This file contains comprehensive unit tests for the qb::string class,
 * testing all constructors, operators, methods, and edge cases.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2025 qb - isndev (cpp.actor)
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
 */

#include <gtest/gtest.h>
#include <qb/string.h>
#include <string>
#include <sstream>

namespace {

// Test fixture for basic string operations
class StringTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Common setup for string tests
    }

    // Helper function to create test strings
    template<std::size_t N>
    qb::string<N> make_test_string(const char* str) {
        return qb::string<N>(str);
    }
};

// Test fixture for string algorithms and search operations
class StringAlgorithmTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_str_ = qb::string<50>("Hello, World! This is a test string.");
        empty_str_ = qb::string<10>();
    }

    qb::string<50> test_str_;
    qb::string<10> empty_str_;
};

// Test fixture for string capacity and memory operations
class StringCapacityTest : public ::testing::Test {
protected:
    void SetUp() override {
        small_str_ = qb::string<10>("test");
        large_str_ = qb::string<100>("This is a much longer test string that exceeds normal limits");
    }

    qb::string<10> small_str_;
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
    const char* cstr = "World";
    qb::string<30> str(cstr);
    EXPECT_EQ(str.size(), 5);
    EXPECT_STREQ(str.c_str(), "World");
}

TEST_F(StringTest, CStringWithSizeConstruction) {
    const char* cstr = "Hello World";
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
    std::string std_str = "Standard string";
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
}

TEST_F(StringTest, CStringLiteralAssignment) {
    qb::string<30> str;
    str = "Assigned";
    EXPECT_EQ(str.size(), 8);
    EXPECT_STREQ(str.c_str(), "Assigned");
}

TEST_F(StringTest, CStringPointerAssignment) {
    qb::string<30> str;
    const char* cstr = "Pointer";
    str = cstr;
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
    std::string std_str = "Standard";
    str = std_str;
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
    str.back() = 'O';
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
    EXPECT_EQ(test_str_.find("test"), 24);  // "test" starts at position 24
    EXPECT_EQ(test_str_.find("notfound"), qb::string<50>::npos);
    
    // Find character
    EXPECT_EQ(test_str_.find('H'), 0);
    EXPECT_EQ(test_str_.find('!'), 12);
    EXPECT_EQ(test_str_.find('z'), qb::string<50>::npos);
    
    // Find with position
    EXPECT_EQ(test_str_.find('i', 20), 32);  // 'i' in "string" at position 32
    EXPECT_EQ(test_str_.find('e', 2), 25);   // 'e' in "test" at position 25
}

TEST_F(StringAlgorithmTest, RFind) {
    // Find last occurrence
    EXPECT_EQ(test_str_.rfind('s'), 29);  // Last 's' in "string" at position 29
    EXPECT_EQ(test_str_.rfind('i'), 32);  // Last 'i' in "string" at position 32
    EXPECT_EQ(test_str_.rfind('z'), qb::string<50>::npos);
    
    // Find last with position
    EXPECT_EQ(test_str_.rfind('s', 30), 29);  // Last 's' before/at position 30 is at 29
    
    // Find last substring
    qb::string<20> substr_test("test is test");
    EXPECT_EQ(substr_test.rfind("test"), 8);
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
    qb::string<30> str("Hello World");
    std::ostringstream oss;
    oss << str;
    EXPECT_EQ(oss.str(), "Hello World");
}

TEST_F(StringTest, InputOperator) {
    qb::string<30> str;
    std::istringstream iss("InputTest");
    iss >> str;
    EXPECT_STREQ(str.c_str(), "InputTest");
    
    // Test with spaces (should stop at first space)
    qb::string<30> str2;
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
    
    empty.clear(); // Should not crash
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
    std::string test_data(500, 'A');
    
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
    qb::string<255> medium_str("Medium string");
    qb::string<65535> large_str("Large string");
    qb::string<10> tiny_str("Tiny");
    
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