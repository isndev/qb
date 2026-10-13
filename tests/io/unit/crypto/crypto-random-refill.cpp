/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <qb/utility/build_macros.h>
#include <qb/io/crypto.h>

// This executable compiles the real sampler without qb-io's amalgamation. Its sole
// byte source is the fixture definition of secure_random_fill below, so no OpenSSL
// global RNG state or shipped code changes are needed to force the refill branch.
#include <qb/io/crypto.cpp>

namespace crypto_random_refill_test {

struct ByteSource {
    std::vector<std::vector<unsigned char>> pools;
    std::vector<std::size_t>                requested;
    std::size_t                             next = 0;
};

thread_local ByteSource *active_source = nullptr;

class ScopedSource {
    ByteSource *_previous;

public:
    explicit ScopedSource(ByteSource &source)
        : _previous(active_source) {
        active_source = &source;
    }
    ~ScopedSource() {
        active_source = _previous;
    }
    ScopedSource(const ScopedSource &)            = delete;
    ScopedSource &operator=(const ScopedSource &) = delete;
};

} // namespace crypto_random_refill_test

bool
qb::crypto::secure_random_fill(std::vector<unsigned char> &buffer) {
    auto *source = crypto_random_refill_test::active_source;
    if (!source)
        throw std::logic_error("sampler called without its owned byte source");
    source->requested.push_back(buffer.size());
    if (source->next == source->pools.size())
        return false; // also bounds a broken sampler that keeps asking for refills
    const auto &pool = source->pools[source->next++];
    if (pool.size() != buffer.size())
        throw std::logic_error("sampler requested an unexpected pool size");
    std::copy(pool.begin(), pool.end(), buffer.begin());
    return true;
}

namespace crypto_random_refill_test {

TEST(CryptoRandomRefill, ExhaustsInitialPoolAndUsesReplacementBytesInOrder) {
    // Eight initial bytes yield only one symbol. The second pool also rejects its
    // first byte: skipping rejection after a refill must change the exact result.
    ByteSource         source{{{255, 254, 253, 252, 251, 250, 249, 0}, {255, 1, 128, 2, 255, 255, 255, 255}}, {}};
    const ScopedSource scope{source};
    std::string        range(129, '\0');
    for (std::size_t i = 0; i < range.size(); ++i)
        range[i] = static_cast<char>(i + 1);

    const auto result = qb::crypto::generate_secure_random_string(4, range);
    EXPECT_EQ(result, (std::string{range[0], range[1], range[128], range[2]}));
    EXPECT_EQ(source.next, 2u);
    EXPECT_EQ(source.requested, (std::vector<std::size_t>{8, 8}));
}

TEST(CryptoRandomRefill, AcceptedInitialPoolNeedsNoRefill) {
    ByteSource         source{{{0, 1, 2, 0, 1, 2, 0, 1}}, {}};
    const ScopedSource scope{source};
    EXPECT_EQ(qb::crypto::generate_secure_random_string(4, "abc"), "abca");
    EXPECT_EQ(source.next, 1u);
    EXPECT_EQ(source.requested, (std::vector<std::size_t>{8}));
}

TEST(CryptoRandomRefill, RefillFailureCannotReturnPartialToken) {
    ByteSource         source{{{255, 255, 255, 255, 255, 255, 255, 0}}, {}};
    const ScopedSource scope{source};
    EXPECT_THROW(qb::crypto::generate_secure_random_string(4, std::string(129, 'x')), std::runtime_error);
    EXPECT_EQ(source.requested, (std::vector<std::size_t>{8, 8}));
}

} // namespace crypto_random_refill_test
