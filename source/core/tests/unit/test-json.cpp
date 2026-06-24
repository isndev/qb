/**
 * @file source/core/tests/unit/test-json.cpp
 * @brief Unit tests for qb/json.h — the qb::jsonb wrapper, uuid adapters, hash & stream support.
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
 */

#include <gtest/gtest.h>
#include <sstream>
#include <string>
#include <unordered_set>
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

TEST(Json, UuidRoundTrip) {
    qb::uuid id{}; // nil uuid
    qb::json obj;
    uuids::to_json(obj, id); // serialize
    qb::uuid back{};
    uuids::from_json(obj, back); // deserialize
    EXPECT_EQ(id, back);
}
