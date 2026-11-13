/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file shared/tls_wrapping_bio.h
 * @brief A BIO that stands in front of an `SSL` object's socket BIO and decides what each write or read it is handed
 *        does -- forwarded, refused "try again", failed with a system error --: how the TLS tests make a socket refuse
 *        one call, deterministically, on every system (Huly QB-1270).
 *
 * It WRAPS the socket BIO, holding a reference of its own that its destroy callback releases; it is never chained
 * onto it (`BIO_push`). OpenSSL before 3.5.6 leaks the transport BIO of a chain: when the `SSL` object lets go of its
 * BIOs -- what `tcp::ssl::socket` does when it detaches them (`SSL_set_bio(ssl, nullptr, nullptr)`) -- the record
 * layer releases only the top BIO of the chain with `BIO_free()`, never the one below it (openssl/openssl#30458, fixed
 * by 089907c9 and 714924fc). qb never chains a transport BIO; a test that does leaked the socket BIO under
 * LeakSanitizer on OpenSSL 3.5.5 (Ubuntu 26.04, the CI's `sanitize` lane). One BIO, no chain: whichever of the `SSL`
 * object and its record layer lets go last, the wrapper's destroy callback releases the socket BIO.
 *
 * Its type carries `BIO_TYPE_DESCRIPTOR` and every control is forwarded, `BIO_C_GET_FD` included: `SSL_get_rfd()` /
 * `SSL_get_wfd()` read the socket's descriptor through it, so `ssl::attach_socket()` keeps it in place and
 * `ssl::socket`'s end-of-session gate reads the descriptor it owns.
 */
#ifndef QB_IO_TEST_TLS_WRAPPING_BIO_H
#define QB_IO_TEST_TLS_WRAPPING_BIO_H

#include <gtest/gtest.h>

#include <openssl/bio.h>
#include <openssl/ssl.h>

namespace qb::io::test::tls_wrapping_bio {

/// The retry state of `inner`, after a call on it, carried to `self`: what `BIO_copy_next_retry()` does for a chain.
inline void
copy_retry(BIO *self, BIO *inner) {
    BIO_set_flags(self, BIO_get_retry_flags(inner));
    BIO_set_retry_reason(self, BIO_get_retry_reason(inner));
}

/// A write handed on to the wrapped BIO, its retry state carried back.
inline int
forward_write(BIO *self, BIO *inner, const char *in, int size) {
    const int ret = BIO_write(inner, in, size);
    if (ret <= 0)
        copy_retry(self, inner);
    return ret;
}

/// A read handed on to the wrapped BIO, its retry state carried back.
inline int
forward_read(BIO *self, BIO *inner, char *out, int size) {
    const int ret = BIO_read(inner, out, size);
    if (ret <= 0)
        copy_retry(self, inner);
    return ret;
}

/**
 * @brief The wrapper whose writes `Write` and reads `Read` decide: each is called with the wrapper (its retry flags
 *        already cleared) and the wrapped BIO, and answers as a BIO call does -- `forward_write()` / `forward_read()`
 *        hand the call on.
 */
template <int (*Write)(BIO *, BIO *, const char *, int), int (*Read)(BIO *, BIO *, char *, int)>
struct wrapper {
    static int
    write(BIO *bio, const char *in, int size) {
        BIO_clear_retry_flags(bio);
        auto *const inner = static_cast<BIO *>(BIO_get_data(bio));
        return inner ? Write(bio, inner, in, size) : -1;
    }

    static int
    read(BIO *bio, char *out, int size) {
        BIO_clear_retry_flags(bio);
        auto *const inner = static_cast<BIO *>(BIO_get_data(bio));
        return inner ? Read(bio, inner, out, size) : -1;
    }

    static long
    ctrl(BIO *bio, int cmd, long num, void *ptr) {
        auto *const inner = static_cast<BIO *>(BIO_get_data(bio));
        return inner ? BIO_ctrl(inner, cmd, num, ptr) : 0;
    }

    static int
    create(BIO *bio) {
        BIO_set_data(bio, nullptr);
        BIO_set_init(bio, 0);
        return 1;
    }

    /// The wrapper's last reference is gone: so is its reference to the socket BIO.
    static int
    destroy(BIO *bio) {
        auto *const inner = static_cast<BIO *>(BIO_get_data(bio));
        BIO_set_data(bio, nullptr);
        BIO_set_init(bio, 0);
        if (inner)
            BIO_free(inner);
        return 1;
    }

    static BIO_METHOD *
    method() {
        static BIO_METHOD *const methods = [] {
            BIO_METHOD *const m = BIO_meth_new(BIO_get_new_index() | BIO_TYPE_SOURCE_SINK | BIO_TYPE_DESCRIPTOR, "qb-test-wrapping-bio");
            BIO_meth_set_write(m, &wrapper::write);
            BIO_meth_set_read(m, &wrapper::read);
            BIO_meth_set_ctrl(m, &wrapper::ctrl);
            BIO_meth_set_create(m, &wrapper::create);
            BIO_meth_set_destroy(m, &wrapper::destroy);
            return m;
        }();
        return methods;
    }

    /// The wrapper in front of the BIO `ssl` writes through (`writes`) or reads through, from here on.
    static void
    install(SSL *ssl, bool writes) {
        BIO *const sock = writes ? SSL_get_wbio(ssl) : SSL_get_rbio(ssl);
        ASSERT_NE(sock, nullptr);
        BIO *const self = BIO_new(method());
        ASSERT_NE(self, nullptr);
        ASSERT_EQ(BIO_up_ref(sock), 1); // the wrapper's own reference: the SSL object lets go of the one it replaces
        BIO_set_data(self, sock);
        BIO_set_init(self, 1);
        if (writes)
            SSL_set0_wbio(ssl, self);
        else
            SSL_set0_rbio(ssl, self);
    }
};

} // namespace qb::io::test::tls_wrapping_bio

#endif // QB_IO_TEST_TLS_WRAPPING_BIO_H
