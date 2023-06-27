/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2021 isndev (www.qbaf.io). All rights reserved.
 *
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
 *         limitations under the License.
 */

#include <gtest/gtest.h>
#include <thread>
#include <qb/io/crypto.h>
#include <qb/io/gzip.h>

TEST(GZIP, All) {
    std::string from = qb::crypto::generate_random_string(128000, qb::crypto::range_alpha_numeric_special);
    std::string compressed_str = qb::gzip::compress(from.c_str(), from.size());
    EXPECT_EQ(from, qb::gzip::uncompress(compressed_str.c_str(), compressed_str.size()));

    qb::allocator::pipe<char> compressed_pipe;
    qb::gzip::to_compress to_c{from.c_str(), from.size()};
    compressed_pipe << to_c;
    EXPECT_EQ(compressed_str.size(), to_c.size_compressed);
    EXPECT_EQ(compressed_str.size(), compressed_pipe.size());
    EXPECT_EQ(compressed_str, std::string(compressed_pipe.begin(), compressed_pipe.size()));

    qb::gzip::to_uncompress to_uc{compressed_pipe.begin(), compressed_pipe.size()};
    qb::allocator::pipe<char> uncompressed_pipe;
    uncompressed_pipe << to_uc;
    EXPECT_EQ(from.size(), to_uc.size_uncompressed);
    EXPECT_EQ(from.size(), uncompressed_pipe.size());
    EXPECT_EQ(from, std::string(uncompressed_pipe.begin(), uncompressed_pipe.size()));
}