/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/compression/compression-brotli.cpp
 * @brief The brotli codec against the provider contract of codec_contract.h (Huly QB-79).
 *
 * Built only with QB_WITH_BROTLI (registered `REQUIRES compression brotli`), so it never passes vacuously.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 * @ingroup Tests
 */

#include "codec_contract.h"

namespace contract = qb::io::test::codec_contract;
namespace builtin  = qb::compression::builtin;

TEST(CompressionBrotli, IsRegisteredForBothDirectionsAfterZlib) {
    contract::expect_registered_after_zlib(builtin::algorithm::BROTLI);
}

TEST(CompressionBrotli, RoundTripsThroughAnyInputPieceAndOutputWindow) {
    contract::expect_round_trips(builtin::algorithm::BROTLI);
}

TEST(CompressionBrotli, ATruncatedStreamIsNeverDoneAndACorruptOneThrows) {
    contract::expect_truncation_never_done_and_corruption_throws(builtin::algorithm::BROTLI);
}

TEST(CompressionBrotli, ResetMakesTheProvidersReusable) {
    contract::expect_reset_reuses(builtin::algorithm::BROTLI);
}
