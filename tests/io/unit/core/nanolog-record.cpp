/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/core/nanolog-record.cpp
 * @brief nanolog's record (`nanolog::NanoLogLine`, which `qb::io::log::stream` names) and its NonGuaranteedLogger ring.
 *
 * A record is encoded on the logging thread and decoded on nanolog's writer thread, later. Three contracts follow, each
 * of which the encoding broke:
 *
 *  - A record owns its text (Huly QB-366). Every `const char (&)[N]` was stored as a POINTER -- the form meant for
 *    string literals -- so a char buffer, a const local array or an array member was read by the writer thread after
 *    the logging code had changed it or freed it. Arrays are now copied, up to their first NUL and never past their
 *    end; borrowing static text is explicit (`NanoLogLine::string_literal_t`).
 *  - A text field is its whole length (Huly QB-367). It was written with a NUL terminator and read up to the first
 *    NUL, so a std::string with an inner NUL cut the record and the bytes after it were decoded as type tags.
 *  - A NonGuaranteedLogger ring is never empty (Huly QB-368). Its capacity, `MB * 4096` items, was computed in 32 bits
 *    and wrapped to 0 at 1 048 576 MB; the writer thread then computed `index % 0`. A capacity its 32-bit indices
 *    cannot count is refused before anything is allocated or started.
 *
 * The record is driven directly (stringify() is what the writer thread calls), the logger through a private copy of
 * nanolog.cpp like guaranteed-logger-buffer's: no global logger, no qb-io archive.
 */

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include <gtest/gtest.h>

#include <qb/vendor/nanolog/nanolog.cpp>

namespace nanolog_record_test {

nanolog::NanoLogLine
record() {
    return nanolog::NanoLogLine{nanolog::LogLevel::INFO, "nanolog-record.cpp", "fn", 1};
}

// What a record writes after its header `[time][level][thread][fn:line] `, without the line's '\n'.
std::string
payload_of(nanolog::NanoLogLine &line) {
    std::ostringstream out;
    line.stringify(out);
    const std::string text  = out.str();
    const auto        start = text.find("[fn:1] ");
    if (start == std::string::npos || text.empty() || text.back() != '\n')
        return "<malformed record: " + text + ">";
    return text.substr(start + 7, text.size() - (start + 7) - 1);
}

// A unique, empty directory per test, removed afterwards.
class ScratchDir {
public:
    ScratchDir() {
        const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
        _path            = std::filesystem::temp_directory_path() / (std::string("qb-nanolog-record-") + info->name());
        std::error_code ec;
        std::filesystem::remove_all(_path, ec);
        std::filesystem::create_directories(_path);
    }
    ~ScratchDir() {
        std::error_code ec;
        std::filesystem::remove_all(_path, ec);
    }
    std::filesystem::path const &
    path() const noexcept {
        return _path;
    }

private:
    std::filesystem::path _path;
};

} // namespace nanolog_record_test

// =============================================================================
// THE RECORD OWNS ITS TEXT (Huly QB-366)
// =============================================================================

/**
 * @test A char array is copied into the record: what the writer thread prints is what was logged.
 * @brief A buffer modified after the log statement is the deterministic witness (a pointer reads the new text), an
 *        array member freed before stringify() the lifetime one (a pointer reads freed memory: ASan reports it), an
 *        unterminated array the bound (a pointer reads past it).
 */
TEST(NanoLogRecord, ACharArrayIsCopiedNotBorrowed) {
    using nanolog_record_test::payload_of;

    auto line       = nanolog_record_test::record();
    char buffer[16] = "first";
    line << buffer;
    std::strcpy(buffer, "SECOND");
    EXPECT_EQ(payload_of(line), "first") << "the record read the buffer when it was written out, not when it was logged";

    struct Holder {
        const char text[8];
    };
    auto held       = std::make_unique<Holder>(Holder{"held"});
    auto from_freed = nanolog_record_test::record();
    from_freed << held->text << '|';
    held.reset();
    EXPECT_EQ(payload_of(from_freed), "held|") << "the record pointed into an array that is gone";

    // An array is a C string bounded by its size: up to its first NUL, never past its end.
    auto       bounded         = nanolog_record_test::record();
    const char unterminated[3] = {'a', 'b', 'c'};
    const char inner_nul[6]    = {'x', '\0', 'y', 'y', 'y', '\0'};
    bounded << unterminated << '|' << inner_nul << '|';
    EXPECT_EQ(payload_of(bounded), "abc|x|");

    // Controls: a literal, a std::string, a std::string_view and a C string read the same before and after.
    auto        controls = nanolog_record_test::record();
    std::string owned("owned");
    const char *pointer = "pointer";
    controls << "literal " << owned << ' ' << std::string_view("view") << ' ' << pointer << ' ' << 7 << ' ' << 2.5;
    EXPECT_EQ(payload_of(controls), "literal owned view pointer 7 2.5");
}

/**
 * @test Static text can still be borrowed, by saying so: `string_literal_t` stores the pointer, not the characters.
 */
TEST(NanoLogRecord, StaticTextIsBorrowedOnlyWhenTheCallerSaysSo) {
    static const char banner[] = "static banner";
    auto              line     = nanolog_record_test::record();
    line << nanolog::NanoLogLine::string_literal_t(banner) << " / " << 1u;
    EXPECT_EQ(nanolog_record_test::payload_of(line), "static banner / 1");
}

// =============================================================================
// A TEXT FIELD IS ITS WHOLE LENGTH (Huly QB-367)
// =============================================================================

/**
 * @test A string or string_view with NUL bytes is written whole, and the fields after it are intact.
 * @brief The text field was NUL-terminated and decoded up to its first NUL: the rest of the text was then read as
 *        type tags -- the record was cut, or decoded as other values.
 */
TEST(NanoLogRecord, ATextFieldKeepsItsInnerNulAndTheFieldsAfterIt) {
    using namespace std::string_literals;
    for (const std::string &text : {"ab\0cd"s, "\0ab"s, "ab\0"s, "\0"s, "a\0\x03\0b"s, "plain"s}) {
        auto line = nanolog_record_test::record();
        line << text << 42 << ' ' << std::string("tail");
        EXPECT_EQ(nanolog_record_test::payload_of(line), text + "42 tail") << "std::string of " << text.size() << " bytes";

        auto viewed = nanolog_record_test::record();
        viewed << std::string_view(text) << 4.5;
        EXPECT_EQ(nanolog_record_test::payload_of(viewed), text + "4.5") << "std::string_view of " << text.size() << " bytes";
    }

    // A record that spills to the heap buffer carries long text fields whole too.
    const std::string long_text = std::string(300, 'L') + '\0' + std::string(300, 'R');
    auto              spilled   = nanolog_record_test::record();
    spilled << long_text << '|' << long_text;
    EXPECT_EQ(nanolog_record_test::payload_of(spilled), long_text + "|" + long_text);
}

// =============================================================================
// A NONGUARANTEEDLOGGER RING IS NEVER EMPTY (Huly QB-368)
// =============================================================================

/**
 * @test A ring of 1 048 576 MB or more -- 2^32 items or more -- is refused before the file is opened or the thread
 *       started; the writer thread used to compute `index % 0` on a ring the 32-bit size had wrapped to empty.
 */
TEST(NonGuaranteedLoggerRing, ACapacityItsIndicesCannotCountIsRefusedBeforeTheWriterStarts) {
    const nanolog_record_test::ScratchDir dir;
    const auto                            base = dir.path() / "ring";
    for (const std::uint32_t mb : {std::uint32_t{1} << 20, std::numeric_limits<std::uint32_t>::max()}) {
        EXPECT_THROW(nanolog::NanoLogger(nanolog::NonGuaranteedLogger(mb), base, 1), std::length_error) << mb << " MB";
        EXPECT_FALSE(std::filesystem::exists(dir.path() / "ring.1.log")) << mb << " MB: the logger got as far as its file";
    }
}

/**
 * @test The capacity arithmetic at its bounds, without allocating them: 4096 items of 256 bytes per MB, at least one MB,
 *       at most the 1 048 575 MB whose 4 294 963 200 items a 32-bit index still counts.
 */
TEST(NonGuaranteedLoggerRing, CapacityIsComputedWithoutWrapping) {
    EXPECT_EQ(nanolog::ring_buffer_items(0), 4096u) << "a 0 MB ring is the 1 MB minimum";
    EXPECT_EQ(nanolog::ring_buffer_items(1), 4096u);
    EXPECT_EQ(nanolog::ring_buffer_items(3), 12288u);
    EXPECT_EQ(nanolog::ring_buffer_items(1048575), std::size_t{4294963200u}) << "the declared maximum";
    EXPECT_THROW((void) nanolog::ring_buffer_items(1048576), std::length_error) << "2^32 items wrapped to 0";
    EXPECT_THROW((void) nanolog::ring_buffer_items(std::numeric_limits<std::uint32_t>::max()), std::length_error);
}

/**
 * @test A NonGuaranteedLogger of ordinary size writes what it is given: the control of the refusal above.
 */
TEST(NonGuaranteedLoggerRing, AnOrdinaryRingWritesItsRecords) {
    const nanolog_record_test::ScratchDir dir;
    {
        nanolog::NanoLogger logger(nanolog::NonGuaranteedLogger(1), dir.path() / "ring", 1);
        auto                line = nanolog_record_test::record();
        line << "through the ring " << 3u;
        logger.add(std::move(line));
    } // the destructor drains the ring into the file
    std::ifstream     in(dir.path() / "ring.1.log");
    std::stringstream content;
    content << in.rdbuf();
    EXPECT_NE(content.str().find("[fn:1] through the ring 3"), std::string::npos) << content.str();
}
