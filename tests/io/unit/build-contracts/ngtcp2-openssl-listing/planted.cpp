/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/build-contracts/ngtcp2-openssl-listing/planted.cpp
 * @brief One OpenSSL library as a Windows DLL names it, for the planted crypto helper to import (Huly QB-1184).
 *
 * Built twice per OpenSSL major on Windows: as libssl-<major>-x64.dll and libcrypto-<major>-x64.dll, the names
 * OpenSSL gives its own DLLs, each exporting the one function QB_PLANTED_EXPORT names. planted-helper.cpp imports
 * both, so the DLL it links lists them as a real libngtcp2_crypto_ossl lists OpenSSL's -- what FindNgtcp2.cmake reads
 * with dumpbin or objdump. Nothing loads either: they hold no OpenSSL, only its name.
 */

extern "C" __declspec(dllexport) int
QB_PLANTED_EXPORT() {
    return 1;
}
