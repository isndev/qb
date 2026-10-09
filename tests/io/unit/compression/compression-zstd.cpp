/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/compression/compression-zstd.cpp
 * @brief The zstd codec against the provider contract of codec_contract.h (Huly QB-79).
 *
 * Built only with QB_WITH_ZSTD (registered `REQUIRES compression zstd`), so it never passes vacuously.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 * @ingroup Tests
 */

#include "codec_contract.h"

namespace contract = qb::io::test::codec_contract;
namespace builtin  = qb::compression::builtin;

TEST(CompressionZstd, IsRegisteredForBothDirectionsAfterZlib) {
    contract::expect_registered_after_zlib(builtin::algorithm::ZSTD);
}

TEST(CompressionZstd, RoundTripsThroughAnyInputPieceAndOutputWindow) {
    contract::expect_round_trips(builtin::algorithm::ZSTD);
}

TEST(CompressionZstd, ATruncatedStreamIsNeverDoneAndACorruptOneThrows) {
    contract::expect_truncation_never_done_and_corruption_throws(builtin::algorithm::ZSTD);
}

TEST(CompressionZstd, ResetMakesTheProvidersReusable) {
    contract::expect_reset_reuses(builtin::algorithm::ZSTD);
}

// A flush that did not fit its window is drained by the calls without input that follow (Huly QB-355).
TEST(CompressionZstd, AnEmptyCallWithoutIsLastDrainsAFlushThatDidNotFit) {
    contract::expect_empty_continuation_drains(builtin::algorithm::ZSTD, 7);
}
