/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/build-contracts/ngtcp2-openssl-listing/planted-helper.cpp
 * @brief A libngtcp2_crypto_ossl stand-in that imports a libssl and a libcrypto of one OpenSSL major (Huly QB-1184).
 *
 * Linked on Windows against two planted.cpp DLLs of one major, so its import table names libssl-<major>-x64.dll and
 * libcrypto-<major>-x64.dll, as the helper vcpkg or MSYS2 build names the OpenSSL it was built against. Its DLL is
 * named apart from its import library, as MinGW names it, so only a name read from the import library finds it.
 * qb-io-test-unit-ngtcp2-openssl-listing and qb-io-test-unit-quic-openssl-verdict read it as qb's configure reads
 * the real one; nothing loads it.
 */

extern "C" __declspec(dllimport) int qb_planted_libssl();
extern "C" __declspec(dllimport) int qb_planted_libcrypto();

extern "C" __declspec(dllexport) int
qb_planted_ngtcp2_crypto_ossl() {
    return qb_planted_libssl() + qb_planted_libcrypto();
}
