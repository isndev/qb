/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file shared/x509_name_check.h
 * @brief Whether a certificate verifies for a host name or an IP address, its subject never consulted.
 *
 * The name a TLS client binds is matched against the certificate's subjectAltName; OpenSSL 4.1 no longer falls
 * back to the subject commonName (RFC 9525), OpenSSL 3 still does for an unflagged check. The cases that pin a
 * test certificate's subjectAltName (Huly QB-1178) therefore ask with X509_CHECK_FLAG_NEVER_CHECK_SUBJECT, so a
 * certificate that names its host in its CN alone is refused on every OpenSSL. They ask through X509_verify_cert()
 * with the name set on the verification parameters, the form OpenSSL 4.1 deprecates X509_check_host() and
 * X509_check_ip_asc() for, and that a TLS client's own check takes.
 * Used by system/tcp/ssl-socket-loopback.cpp and, through shared/tls_pump.h, system/tls/tls-peer-verification.cpp.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 * @ingroup Tests
 */

#ifndef QB_IO_TESTS_SHARED_X509_NAME_CHECK_H
#define QB_IO_TESTS_SHARED_X509_NAME_CHECK_H

#include <memory>
#include <string>

#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509v3.h>

namespace qb::io::test {

// What a reference name is checked as: a DNS name (X509_VERIFY_PARAM_set1_host) or an IP address in text form
// (X509_VERIFY_PARAM_set1_ip_asc).
enum class name_kind { host, ip };

// The error X509_verify_cert() reports for `leaf` against a store that trusts `anchor` and nothing else, with
// `name` as the reference identity and the subject never consulted for it: X509_V_OK when the chain verifies and
// the subjectAltName carries the name, -1 when the check could not be set up. A refusal for the name is
// X509_V_ERR_HOSTNAME_MISMATCH or X509_V_ERR_IP_ADDRESS_MISMATCH. OpenSSL checks the name once the chain is built
// to the anchor and before the signatures and the dates, so one of the two proves the chain, not the certificate's
// validity: a case pairs it with a positive check of the same certificate, which proves the rest.
[[nodiscard]] inline int
name_check_error(X509 *leaf, X509 *anchor, const std::string &name, name_kind kind) {
    std::unique_ptr<X509_STORE, decltype(&X509_STORE_free)>         store{X509_STORE_new(), &X509_STORE_free};
    std::unique_ptr<X509_STORE_CTX, decltype(&X509_STORE_CTX_free)> ctx{X509_STORE_CTX_new(), &X509_STORE_CTX_free};
    if (!leaf || !anchor || !store || !ctx || X509_STORE_add_cert(store.get(), anchor) != 1
        || X509_STORE_CTX_init(ctx.get(), store.get(), leaf, nullptr) != 1)
        return -1;
    X509_VERIFY_PARAM *param = X509_STORE_CTX_get0_param(ctx.get());
    X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NEVER_CHECK_SUBJECT);
    const int named = kind == name_kind::host ? X509_VERIFY_PARAM_set1_host(param, name.c_str(), name.size())
                                              : X509_VERIFY_PARAM_set1_ip_asc(param, name.c_str());
    if (named != 1)
        return -1;
    if (X509_verify_cert(ctx.get()) == 1)
        return X509_V_OK;
    const int error = X509_STORE_CTX_get_error(ctx.get());
    return error == X509_V_OK ? -1 : error;
}

// A name_check_error() result in words, for a failure message.
[[nodiscard]] inline std::string
name_check_text(int error) {
    return error == -1 ? std::string{"the check could not be set up"} : std::string{X509_verify_cert_error_string(error)};
}

} // namespace qb::io::test

#endif // QB_IO_TESTS_SHARED_X509_NAME_CHECK_H
