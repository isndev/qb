/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/json/jsonb-wrapper.cpp
 * @brief `qb::jsonb` wrapper + uuid↔json adapters (`qb/json.h`, `qb/uuid.h`) — pure serialization.
 *
 * No engine, loop, or daemon: exercises the wrapper surface (ctors/conversions, assignment/
 * indexing, `operator->`/iterators, type predicates + `dump()`, size/empty/clear/erase/contains,
 * cross `jsonb`↔`json` comparisons, `unwrap()`, `operator<<`, and `std::hash<jsonb>` via set
 * dedup) plus the uuid round-trip through `to_json`/`from_json`. Assertions pin serialized output
 * (`dump()=="42"`, `operator<<`==`dump()`) and prove hash+equality cooperate, not self-echoes.
 *
 * Added over the original: a NON-nil uuid round-trip (the original only round-tripped the nil
 * uuid, so a codec that mishandled non-zero bytes would pass); int64 and double `dump()` precision
 * cases that guard the known number-truncation bug class (the qb `pipe::put<json>` serializer once
 * truncated every number through get<int>/get<float>, turning int64 timestamps negative and losing
 * double precision — these assert full 64-bit and full double precision survive `dump()`); and a
 * `json::parse` malformed-input error case plus a parse-then-wrap success path.
 */

#include <cstdint>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_set>

#include <gtest/gtest.h>
#include <qb/json.h>
#include <qb/uuid.h>

using qb::json;
using qb::jsonb;

TEST(Jsonb, ConstructorsAndConversions) {
    jsonb def; // default → null
    EXPECT_TRUE(def.is_null());

    json  src = {{"k", 1}, {"v", "x"}};
    jsonb fromCopy(src); // copy ctor from json
    EXPECT_TRUE(fromCopy.is_object());
    jsonb fromMove(json{{"a", 2}}); // move ctor from json
    EXPECT_TRUE(fromMove.contains("a"));
    jsonb fromInit({1, 2, 3}); // initializer-list ctor → array
    EXPECT_TRUE(fromInit.is_array());
    EXPECT_EQ(fromInit.size(), 3u);

    // conversion operators to json& / const json&
    json       &ref  = fromCopy;
    const json &cref = std::as_const(fromCopy);
    EXPECT_EQ(ref, cref);
}

TEST(Jsonb, AssignmentAndIndexing) {
    jsonb j;
    j = json{{"n", 10}}; // copy assign from json
    EXPECT_EQ(j["n"], 10);
    j["m"] = 20;                          // mutable operator[](string)
    EXPECT_EQ(std::as_const(j)["m"], 20); // const operator[](string) → at

    jsonb arr;
    arr = json::array();
    arr.push_back(json(7));
    arr.push_back(json(8));
    EXPECT_EQ(arr[0], 7);                // mutable operator[](size_t)
    EXPECT_EQ(std::as_const(arr)[1], 8); // const operator[](size_t) → at

    jsonb mv;
    mv = json{{"z", 1}}; // move assign path
    EXPECT_TRUE(mv.contains("z"));
}

TEST(Jsonb, ArrowAndIterators) {
    jsonb j = json{{"a", 1}, {"b", 2}};
    EXPECT_EQ(j->size(), 2u);                     // mutable operator->
    EXPECT_TRUE(std::as_const(j)->contains("a")); // const operator->

    int count = 0;
    for (auto it = j.begin(); it != j.end(); ++it)
        ++count;
    EXPECT_EQ(count, 2);
    int          ccount = 0;
    const jsonb &cj     = j;
    for (auto it = cj.begin(); it != cj.end(); ++it)
        ++ccount;
    EXPECT_EQ(ccount, 2);
}

TEST(Jsonb, TypePredicatesAndDump) {
    EXPECT_TRUE(jsonb(json(nullptr)).is_null());
    EXPECT_TRUE(jsonb(json::object()).is_object());
    EXPECT_TRUE(jsonb(json::array()).is_array());
    EXPECT_TRUE(jsonb(json("s")).is_string());
    EXPECT_TRUE(jsonb(json(3)).is_number());
    EXPECT_TRUE(jsonb(json(true)).is_boolean());
    EXPECT_EQ(jsonb(json(42)).dump(), "42");
}

TEST(Jsonb, SizeEmptyClearEraseContains) {
    jsonb j = json{{"a", 1}, {"b", 2}};
    EXPECT_EQ(j.size(), 2u);
    EXPECT_FALSE(j.empty());
    EXPECT_TRUE(j.contains("a"));
    j.erase("a");
    EXPECT_FALSE(j.contains("a"));
    j.clear();
    EXPECT_TRUE(j.empty());
}

TEST(Jsonb, Comparisons) {
    jsonb a = json{{"x", 1}};
    jsonb b = json{{"x", 1}};
    jsonb c = json{{"x", 2}};
    EXPECT_TRUE(a == b);
    EXPECT_TRUE(a != c);
    json raw = {{"x", 1}};
    EXPECT_TRUE(a == raw); // jsonb == json
    EXPECT_TRUE(raw == b); // json == jsonb
}

TEST(Jsonb, UnwrapStreamAndHash) {
    jsonb j = json{{"k", 5}};
    EXPECT_EQ(j.unwrap(), std::as_const(j).unwrap());

    std::ostringstream os;
    os << j; // operator<<
    EXPECT_EQ(os.str(), j.dump());

    std::unordered_set<jsonb> set; // requires std::hash<jsonb>
    set.insert(jsonb(json{{"k", 5}}));
    set.insert(jsonb(json{{"k", 5}})); // duplicate
    set.insert(jsonb(json{{"k", 6}}));
    EXPECT_EQ(set.size(), 2u);
}

TEST(Jsonb, DumpPreservesInt64AndDoublePrecision) {
    // Guards the number-truncation bug class: the qb pipe `put<json>` serializer once read every
    // number through get<int>/get<unsigned>/get<float>, so an int64 timestamp went negative and a
    // high-precision double lost its tail. dump() must preserve full 64-bit integer and full double
    // precision. (We test the dump() path here; the pipe path is covered elsewhere, but the
    // representable-value contract is the same.)
    constexpr std::int64_t big = 1'673'785'845'123'456'789LL; // ~ns-since-epoch, > 2^31
    static_assert(big > static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max()),
                  "value must exceed 32-bit range to catch truncation");
    json  big_j(big);
    jsonb ji(big_j);
    EXPECT_EQ(ji.dump(), "1673785845123456789") << "int64 must survive dump() without 32-bit truncation";
    EXPECT_EQ(ji.unwrap().get<std::int64_t>(), big);

    // A large unsigned past INT32_MAX must not wrap negative.
    constexpr std::uint64_t ubig = 4'294'967'296ULL; // 2^32
    json                    ubig_j(ubig);
    jsonb                   ju(ubig_j);
    EXPECT_EQ(ju.dump(), "4294967296");
    EXPECT_EQ(ju.unwrap().get<std::uint64_t>(), ubig);

    // A double with a non-trivial mantissa must keep its precision through dump()/parse round-trip
    // (nlohmann emits the shortest round-trippable form, so parse(dump(x)) == x exactly).
    const double d = 3.141592653589793;
    json         d_j(d);
    jsonb        jd(d_j);
    EXPECT_DOUBLE_EQ(json::parse(jd.dump()).get<double>(), d) << "double must round-trip through dump() without float truncation";
}

TEST(Jsonb, ParseErrorAndParsedConstruction) {
    // Malformed JSON must throw a parse_error (nlohmann's exception type), not silently yield null.
    EXPECT_THROW((void) json::parse("{ this is : not json"), json::parse_error);
    EXPECT_THROW((void) json::parse(""), json::parse_error);

    // A wrapper built from a successfully parsed string exposes the parsed structure.
    jsonb parsed(json::parse(R"({"k": 5, "arr": [1, 2, 3]})"));
    EXPECT_TRUE(parsed.is_object());
    EXPECT_EQ(parsed["k"], 5);
    EXPECT_TRUE(parsed.contains("arr"));
    EXPECT_EQ(parsed["arr"].size(), 3u);
}

TEST(Json, UuidRoundTrip) {
    qb::uuid id{}; // nil uuid
    qb::json obj;
    uuids::to_json(obj, id); // serialize
    qb::uuid back{};
    uuids::from_json(obj, back); // deserialize
    EXPECT_EQ(id, back);
    EXPECT_TRUE(id.is_nil());
}

TEST(Json, NonNilUuidRoundTrip) {
    // The original only round-tripped the nil (all-zero) uuid, so a codec that mishandled non-zero
    // bytes would pass. Round-trip a specific non-nil uuid parsed from its canonical string form and
    // assert both the value survives AND the serialized JSON carries the canonical text.
    const auto parsed = uuids::uuid::from_string("12345678-90ab-cdef-1234-567890abcdef");
    ASSERT_TRUE(parsed.has_value());
    const qb::uuid id = *parsed;
    ASSERT_FALSE(id.is_nil());

    qb::json obj;
    uuids::to_json(obj, id);
    EXPECT_EQ(obj.get<std::string>(), "12345678-90ab-cdef-1234-567890abcdef")
        << "to_json must emit the canonical uuid text, not the nil/zeroed form";

    qb::uuid back{};
    uuids::from_json(obj, back);
    EXPECT_EQ(id, back);
    EXPECT_EQ(uuids::to_string(back), "12345678-90ab-cdef-1234-567890abcdef");
}
