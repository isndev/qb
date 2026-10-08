/**
 * @file qb/io/crypto_asymmetric.cpp
 * @brief Implementation of asymmetric cryptographic utilities for the QB IO library
 *
 * This file provides implementations of modern asymmetric cryptographic operations such
 * as:
 * - Ed25519 for digital signatures
 * - X25519 for key exchange
 * - ECIES (Elliptic Curve Integrated Encryption Scheme) for hybrid encryption
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
 * @ingroup IO
 */

#include <cstring>
#include <fstream>
#include <memory>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/objects.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <qb/io/crypto.h>
#include <sstream>
#include <stdexcept>

namespace qb {

// Every OpenSSL handle below goes into its detail::openssl_ptr owner the line it is created, before any
// allocation or call that may throw (Huly QB-345): the hand-written free on each error path missed every
// throw that was not one of them -- a key_to_pem() failure, a std::vector or std::string allocation --
// and leaked the handles it held.

// Helper function for OpenSSL error handling
static std::string
get_openssl_asymmetric_error() {
    char          err_buf[256];
    unsigned long err = ERR_get_error();
    ERR_error_string_n(err, err_buf, sizeof(err_buf));
    return std::string(err_buf);
}

// Helper to convert EVP_PKEY to string
static std::string
key_to_pem(EVP_PKEY *pkey, bool is_private) {
    const detail::openssl_ptr<BIO> bio{BIO_new(BIO_s_mem())};
    if (!bio) {
        throw std::runtime_error("Failed to allocate memory for key conversion"); // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    int result;
    if (is_private) {
        result = PEM_write_bio_PrivateKey(bio.get(), pkey, NULL, NULL, 0, NULL, NULL);
    } else {
        result = PEM_write_bio_PUBKEY(bio.get(), pkey);
    }

    if (result != 1) {
        throw std::runtime_error("Failed to write key to PEM: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error()); // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    char      *pem_ptr;
    const long pem_size = BIO_get_mem_data(bio.get(), &pem_ptr);
    return std::string(pem_ptr, static_cast<std::size_t>(pem_size));
}

// Helper to convert PEM string to EVP_PKEY
static detail::openssl_ptr<EVP_PKEY>
pem_to_key(const std::string &pem_str, bool is_private) {
    const detail::openssl_ptr<BIO> bio{BIO_new_mem_buf(pem_str.c_str(), -1)};
    if (!bio) {
        throw std::runtime_error("Failed to allocate memory for key parsing"); // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    detail::openssl_ptr<EVP_PKEY> pkey{
        is_private ? PEM_read_bio_PrivateKey(bio.get(), NULL, NULL, NULL) : PEM_read_bio_PUBKEY(bio.get(), NULL, NULL, NULL)
    };
    if (!pkey) {
        throw std::runtime_error("Failed to parse PEM key: " + get_openssl_asymmetric_error());
    }

    return pkey;
}

// Helper to extract raw key bytes from EVP_PKEY
static std::vector<unsigned char>
get_raw_key_bytes(EVP_PKEY *pkey, bool is_private) {
    size_t key_len;
    if (EVP_PKEY_get_raw_private_key(pkey, NULL, &key_len) != 1 && EVP_PKEY_get_raw_public_key(pkey, NULL, &key_len) != 1) {
        throw std::runtime_error("Failed to determine key length: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());     // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    std::vector<unsigned char> key_bytes(key_len);
    if (is_private) {
        if (EVP_PKEY_get_raw_private_key(pkey, key_bytes.data(), &key_len) != 1) {
            throw std::runtime_error("Failed to extract private key bytes: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                     get_openssl_asymmetric_error());          // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        }
    } else {
        if (EVP_PKEY_get_raw_public_key(pkey, key_bytes.data(), &key_len) != 1) {
            throw std::runtime_error("Failed to extract public key bytes: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                     get_openssl_asymmetric_error());         // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        }
    }

    return key_bytes;
}

// The PEM key pair of a freshly generated key of type `type`, `configure` applied to the keygen context
// first (the RSA bits, the EC curve). The four generate_*_keypair() below share it.
template <typename Configure>
static std::pair<std::string, std::string>
generate_pem_keypair(int type, const char *name, Configure &&configure) {
    const detail::openssl_ptr<EVP_PKEY_CTX> ctx{EVP_PKEY_CTX_new_id(type, NULL)};
    if (!ctx) {
        throw std::runtime_error(std::string("Failed to create ") + name + " context: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());                         // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    if (EVP_PKEY_keygen_init(ctx.get()) != 1) {
        throw std::runtime_error(std::string("Failed to initialize ") + name + " key generation: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());                                    // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    configure(ctx.get());

    EVP_PKEY *generated = NULL;
    if (EVP_PKEY_keygen(ctx.get(), &generated) != 1) {
        throw std::runtime_error(std::string(name) + " key generation failed: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());                 // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }
    const detail::openssl_ptr<EVP_PKEY> pkey{generated};

    std::string private_key_pem = key_to_pem(pkey.get(), true);
    std::string public_key_pem  = key_to_pem(pkey.get(), false);
    return std::make_pair(std::move(private_key_pem), std::move(public_key_pem));
}

// Implementation of RSA key pair generation (PEM format).
// Declared in crypto.h but previously had no definition — any caller failed to
// link. Mirrors generate_ed25519_keypair's EVP keygen + key_to_pem flow.
std::pair<std::string, std::string>
crypto::generate_rsa_keypair(int bits) {
    if (bits < 2048) {
        throw std::runtime_error("RSA key size must be at least 2048 bits");
    }

    return generate_pem_keypair(EVP_PKEY_RSA, "RSA", [bits](EVP_PKEY_CTX *ctx) {
        if (EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, bits) != 1) {
            throw std::runtime_error("Failed to set RSA key size: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                     get_openssl_asymmetric_error()); // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        }
    });
}

// Implementation of EC key pair generation (PEM format). Curve is a short name
// such as "prime256v1" (P-256/ES256), "secp384r1", "secp521r1". Declared in
// crypto.h but previously had no definition.
std::pair<std::string, std::string>
crypto::generate_ec_keypair(const std::string &curve) {
    const int nid = OBJ_sn2nid(curve.c_str());
    if (nid == NID_undef) {
        throw std::runtime_error("Unknown EC curve: " + curve);
    }

    return generate_pem_keypair(EVP_PKEY_EC, "EC", [nid](EVP_PKEY_CTX *ctx) {
        if (EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx, nid) != 1) {
            throw std::runtime_error("Failed to set EC curve: " +     // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                     get_openssl_asymmetric_error()); // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        }
    });
}

// Implementation of Ed25519 key pair generation (PEM format)
std::pair<std::string, std::string>
crypto::generate_ed25519_keypair() {
    return generate_pem_keypair(EVP_PKEY_ED25519, "Ed25519", [](EVP_PKEY_CTX *) {});
}

// Implementation of Ed25519 key pair generation (raw bytes)
std::pair<std::vector<unsigned char>, std::vector<unsigned char>>
crypto::generate_ed25519_keypair_bytes() {
    // First generate the key pair in PEM format
    auto [private_key_pem, public_key_pem] = generate_ed25519_keypair();

    const auto pkey = pem_to_key(private_key_pem, true);
    return std::make_pair(get_raw_key_bytes(pkey.get(), true), get_raw_key_bytes(pkey.get(), false));
}

// The one-shot signature of `data` with `pkey` (Ed25519 signs the message itself, no digest).
static std::vector<unsigned char>
ed25519_sign_with(EVP_PKEY *pkey, const std::vector<unsigned char> &data) {
    const detail::openssl_ptr<EVP_MD_CTX> md_ctx{EVP_MD_CTX_new()};
    if (!md_ctx) {
        throw std::runtime_error("Failed to create signing context: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());       // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    if (EVP_DigestSignInit(md_ctx.get(), NULL, NULL, NULL, pkey) != 1) {
        throw std::runtime_error("Failed to initialize signing operation: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());             // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    // Determine the signature size
    size_t sig_len;
    if (EVP_DigestSign(md_ctx.get(), NULL, &sig_len, data.data(), data.size()) != 1) {
        throw std::runtime_error("Failed to determine signature size: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());         // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    std::vector<unsigned char> signature(sig_len);
    if (EVP_DigestSign(md_ctx.get(), signature.data(), &sig_len, data.data(), data.size()) != 1) {
        throw std::runtime_error("Signing failed: " + get_openssl_asymmetric_error()); // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    // Resize to actual signature length (which might be smaller than initially allocated)
    signature.resize(sig_len);
    return signature;
}

// Whether `signature` is `pkey`'s Ed25519 signature of `data`.
static bool
ed25519_verify_with(EVP_PKEY *pkey, const std::vector<unsigned char> &data, const std::vector<unsigned char> &signature) {
    const detail::openssl_ptr<EVP_MD_CTX> md_ctx{EVP_MD_CTX_new()};
    if (!md_ctx) {
        throw std::runtime_error("Failed to create verification context: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());            // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    if (EVP_DigestVerifyInit(md_ctx.get(), NULL, NULL, NULL, pkey) != 1) {
        throw std::runtime_error("Failed to initialize verification operation: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());                  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    return EVP_DigestVerify(md_ctx.get(), signature.data(), signature.size(), data.data(), data.size()) == 1;
}

// Implementation of Ed25519 signing with PEM key
std::vector<unsigned char>
crypto::ed25519_sign(const std::vector<unsigned char> &data, const std::string &private_key_pem) {
    return ed25519_sign_with(pem_to_key(private_key_pem, true).get(), data);
}

// Implementation of Ed25519 signing with raw key bytes
std::vector<unsigned char>
crypto::ed25519_sign(const std::vector<unsigned char> &data, const std::vector<unsigned char> &private_key_bytes) {
    const detail::openssl_ptr<EVP_PKEY> pkey{EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, private_key_bytes.data(),
                                                                          private_key_bytes.size())};
    if (!pkey) {
        throw std::runtime_error("Failed to create key from raw bytes: " + get_openssl_asymmetric_error());
    }
    return ed25519_sign_with(pkey.get(), data);
}

// Implementation of Ed25519 verification with PEM key
bool
crypto::ed25519_verify(const std::vector<unsigned char> &data, const std::vector<unsigned char> &signature, const std::string &public_key_pem) {
    return ed25519_verify_with(pem_to_key(public_key_pem, false).get(), data, signature);
}

// Implementation of Ed25519 verification with raw key bytes
bool
crypto::ed25519_verify(const std::vector<unsigned char> &data, const std::vector<unsigned char> &signature,
                       const std::vector<unsigned char> &public_key_bytes) {
    const detail::openssl_ptr<EVP_PKEY> pkey{EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, public_key_bytes.data(),
                                                                         public_key_bytes.size())};
    if (!pkey) {
        throw std::runtime_error("Failed to create key from raw bytes: " + get_openssl_asymmetric_error());
    }
    return ed25519_verify_with(pkey.get(), data, signature);
}

// Implementation of X25519 key pair generation (PEM format)
std::pair<std::string, std::string>
crypto::generate_x25519_keypair() {
    return generate_pem_keypair(EVP_PKEY_X25519, "X25519", [](EVP_PKEY_CTX *) {});
}

// Implementation of X25519 key pair generation (raw bytes)
std::pair<std::vector<unsigned char>, std::vector<unsigned char>>
crypto::generate_x25519_keypair_bytes() {
    // First generate the key pair in PEM format
    auto [private_key_pem, public_key_pem] = generate_x25519_keypair();

    const auto pkey = pem_to_key(private_key_pem, true);
    return std::make_pair(get_raw_key_bytes(pkey.get(), true), get_raw_key_bytes(pkey.get(), false));
}

// The shared secret of `priv_key` with `pub_key` (whose types the caller has made agree).
static std::vector<unsigned char>
derive_shared_secret(EVP_PKEY *priv_key, EVP_PKEY *pub_key) {
    const detail::openssl_ptr<EVP_PKEY_CTX> ctx{EVP_PKEY_CTX_new(priv_key, NULL)};
    if (!ctx) {
        throw std::runtime_error("Failed to create key exchange context: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());            // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    if (EVP_PKEY_derive_init(ctx.get()) != 1) {
        throw std::runtime_error("Failed to initialize key derivation: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());          // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    if (EVP_PKEY_derive_set_peer(ctx.get(), pub_key) != 1) {
        throw std::runtime_error("Failed to set peer key: " + get_openssl_asymmetric_error());
    }

    // Determine buffer length for shared secret
    size_t secret_len;
    if (EVP_PKEY_derive(ctx.get(), NULL, &secret_len) != 1) {
        throw std::runtime_error("Failed to determine shared secret length: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());               // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    std::vector<unsigned char> shared_secret(secret_len);
    if (EVP_PKEY_derive(ctx.get(), shared_secret.data(), &secret_len) != 1) {
        throw std::runtime_error("Key derivation failed: " + get_openssl_asymmetric_error()); // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    // Resize to actual secret length
    shared_secret.resize(secret_len);
    return shared_secret;
}

// Implementation of X25519 key exchange with PEM keys
std::vector<unsigned char>
crypto::x25519_key_exchange(const std::string &private_key_pem, const std::string &peer_public_key_pem) {
    // Parse the keys. Both are owned, so a malformed peer PEM throwing after the private key parsed
    // leaks nothing.
    const auto priv_key = pem_to_key(private_key_pem, true);
    const auto pub_key  = pem_to_key(peer_public_key_pem, false);

    // Reject a peer key of the wrong ALGORITHM here, before OpenSSL ever sees it.
    //
    // Both PEMs are caller-supplied, and in a key-agreement handshake the PEER one normally comes
    // off the wire — so its algorithm is chosen by the remote end, not by us. A well-formed
    // Ed25519 public key parses fine above (it is valid PEM, merely the wrong curve) and then
    // reaches EVP_PKEY_derive_set_peer() as a type mismatch, which OpenSSL treats as a CALLER bug
    // rather than an input error: crypto/evp/keymgmt_lib.c asserts
    // `match_type(pk->keymgmt, keymgmt)`. On an OpenSSL built with NDEBUG (the usual release
    // packaging on Linux/macOS) `ossl_assert` degrades to a plain test and the call merely returns
    // 0, so the throw below is reached and nothing looks wrong. On an OpenSSL built WITH
    // assertions (vcpkg's Windows debug triplet, and several distro debug builds) the same call
    // reaches OPENSSL_die() and **abort()s the process** — a remote peer picking the wrong key
    // type takes the server down.
    //
    // Doing the check ourselves makes the rejection qb's own and therefore independent of how the
    // linked OpenSSL happens to be configured.
    //
    // The test is deliberately "the two key types AGREE", not "both are X25519", even though the
    // header documents this as an X25519 helper: agreement is EXACTLY the precondition
    // EVP_PKEY_derive_set_peer() asserts on, so this guard cannot reject any input that used to
    // work (a matched X448 or EC pair still derives, as it did before). It only converts the
    // undefined-behaviour case into a clean exception.
    //
    // The raw-bytes overload below needs no equivalent guard: it builds BOTH keys itself with
    // EVP_PKEY_new_raw_{private,public}_key(EVP_PKEY_X25519, ...), so the types match by
    // construction and a wrong-algorithm peer cannot be expressed.
    if (EVP_PKEY_base_id(priv_key.get()) != EVP_PKEY_base_id(pub_key.get())) {
        throw std::runtime_error("x25519_key_exchange: peer public key algorithm does not match the private key's");
    }

    return derive_shared_secret(priv_key.get(), pub_key.get());
}

// Implementation of X25519 key exchange with raw key bytes
std::vector<unsigned char>
crypto::x25519_key_exchange(const std::vector<unsigned char> &private_key_bytes, const std::vector<unsigned char> &peer_public_key_bytes) {
    // Create keys from raw bytes
    const detail::openssl_ptr<EVP_PKEY> priv_key{EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, private_key_bytes.data(),
                                                                              private_key_bytes.size())};
    if (!priv_key) {
        throw std::runtime_error("Failed to create private key from raw bytes: " + get_openssl_asymmetric_error());
    }

    const detail::openssl_ptr<EVP_PKEY> pub_key{EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, NULL, peer_public_key_bytes.data(),
                                                                            peer_public_key_bytes.size())};
    if (!pub_key) {
        throw std::runtime_error("Failed to create public key from raw bytes: " + get_openssl_asymmetric_error());
    }

    return derive_shared_secret(priv_key.get(), pub_key.get());
}

// Implementation of ECIES encryption with raw key bytes
std::pair<std::vector<unsigned char>, std::vector<unsigned char>>
crypto::ecies_encrypt(const std::vector<unsigned char> &data, const std::vector<unsigned char> &recipient_public_key,
                      const std::vector<unsigned char> &optional_shared_info, ECIESMode mode) {
    // Generate ephemeral X25519 key pair
    auto [ephemeral_priv_key, ephemeral_pub_key] = generate_x25519_keypair_bytes();

    // Perform X25519 key exchange to derive shared secret
    std::vector<unsigned char> shared_secret = x25519_key_exchange(ephemeral_priv_key, recipient_public_key);

    // Derive encryption key and IV using HKDF
    std::vector<unsigned char> key_material = hkdf(shared_secret,
                                                   optional_shared_info, // Use shared info as salt
                                                   {},                   // Empty info
                                                   64,                   // 32 bytes for key, 16 for IV
                                                   DigestAlgorithm::SHA256);

    // Select symmetric algorithm + nonce length from the mode. GCM/ChaCha take a
    // 12-byte nonce, CBC a 16-byte IV. Slicing the exact length (not a fixed 16)
    // keeps AEAD nonces at the standard 96 bits now that the symmetric path
    // requires an exact IV length. This is byte-identical to the previous
    // behaviour: OpenSSL already consumed only the first 12 bytes of the old
    // 16-byte GCM/ChaCha IV, so the derived nonce is unchanged.
    SymmetricAlgorithm sym_algorithm;
    std::size_t        iv_len;
    switch (mode) {
        case ECIESMode::AES_GCM:
            sym_algorithm = SymmetricAlgorithm::AES_256_GCM;
            iv_len        = 12;
            break;
        case ECIESMode::CHACHA20:
            sym_algorithm = SymmetricAlgorithm::CHACHA20_POLY1305;
            iv_len        = 12;
            break;
        case ECIESMode::STANDARD:
        default:
            sym_algorithm = SymmetricAlgorithm::AES_256_CBC;
            iv_len        = 16;
            break;
    }

    // Extract key and IV from the derived material
    std::vector<unsigned char> symmetric_key(key_material.begin(), key_material.begin() + 32);
    std::vector<unsigned char> iv(key_material.begin() + 32, key_material.begin() + 32 + iv_len);

    // Encrypt the data using the derived key and IV
    std::vector<unsigned char> encrypted_data = encrypt(data, symmetric_key, iv, sym_algorithm);

    // Return ephemeral public key and encrypted data
    return std::make_pair(ephemeral_pub_key, encrypted_data);
}

// Implementation of ECIES decryption with raw key bytes
std::vector<unsigned char>
crypto::ecies_decrypt(const std::vector<unsigned char> &encrypted_data, const std::vector<unsigned char> &ephemeral_public_key,
                      const std::vector<unsigned char> &recipient_private_key, const std::vector<unsigned char> &optional_shared_info,
                      ECIESMode mode) {
    // Perform X25519 key exchange to derive shared secret
    std::vector<unsigned char> shared_secret = x25519_key_exchange(recipient_private_key, ephemeral_public_key);

    // Derive decryption key and IV using HKDF
    std::vector<unsigned char> key_material = hkdf(shared_secret,
                                                   optional_shared_info, // Use shared info as salt
                                                   {},                   // Empty info
                                                   64,                   // 32 bytes for key, 16 for IV
                                                   DigestAlgorithm::SHA256);

    // Select symmetric algorithm + nonce length from the mode (must mirror
    // ecies_encrypt): GCM/ChaCha 12-byte nonce, CBC 16-byte IV. Byte-identical
    // to the prior fixed-16 slice for the derived nonce (see ecies_encrypt).
    SymmetricAlgorithm sym_algorithm;
    std::size_t        iv_len;
    switch (mode) {
        case ECIESMode::AES_GCM:
            sym_algorithm = SymmetricAlgorithm::AES_256_GCM;
            iv_len        = 12;
            break;
        case ECIESMode::CHACHA20:
            sym_algorithm = SymmetricAlgorithm::CHACHA20_POLY1305;
            iv_len        = 12;
            break;
        case ECIESMode::STANDARD:
        default:
            sym_algorithm = SymmetricAlgorithm::AES_256_CBC;
            iv_len        = 16;
            break;
    }

    // Extract key and IV from the derived material
    std::vector<unsigned char> symmetric_key(key_material.begin(), key_material.begin() + 32);
    std::vector<unsigned char> iv(key_material.begin() + 32, key_material.begin() + 32 + iv_len);

    // Decrypt the data using the derived key and IV
    std::vector<unsigned char> decrypted_data = decrypt(encrypted_data, symmetric_key, iv, sym_algorithm);

    return decrypted_data;
}

// The digest signature of `data` with the PEM private key, `name` ("RSA", "EC") in the messages. The key
// is parsed first, so a malformed PEM is reported before an invalid digest, as before.
static std::vector<unsigned char>
digest_sign_pem(const std::vector<unsigned char> &data, const std::string &private_key, crypto::DigestAlgorithm digest, const char *name) {
    const auto pkey = pem_to_key(private_key, true);

    const EVP_MD *md = crypto::get_evp_md(digest);
    if (!md) {
        throw std::runtime_error(std::string("Invalid digest algorithm for ") + name + " signing");
    }

    const detail::openssl_ptr<EVP_MD_CTX> md_ctx{EVP_MD_CTX_new()};
    if (!md_ctx) {
        throw std::runtime_error("Failed to create signing context: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());       // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    if (EVP_DigestSignInit(md_ctx.get(), nullptr, md, nullptr, pkey.get()) != 1) {
        throw std::runtime_error(std::string("Failed to initialize ") + name + " signing operation: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());                                       // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    if (EVP_DigestSignUpdate(md_ctx.get(), data.data(), data.size()) != 1) {
        throw std::runtime_error(std::string("Failed to update ") + name + " signing context: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());                                 // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    size_t sig_len = 0;
    if (EVP_DigestSignFinal(md_ctx.get(), nullptr, &sig_len) != 1) {
        throw std::runtime_error(std::string("Failed to determine ") + name + " signature size: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());                                   // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    std::vector<unsigned char> signature(sig_len);
    if (EVP_DigestSignFinal(md_ctx.get(), signature.data(), &sig_len) != 1) {
        throw std::runtime_error(std::string(name) + " signing failed: " + get_openssl_asymmetric_error()); // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    // Resize to actual signature length
    signature.resize(sig_len);
    return signature;
}

// Whether `signature` is the digest signature of `data` by the PEM public key, `name` in the messages.
static bool
digest_verify_pem(const std::vector<unsigned char> &data, const std::vector<unsigned char> &signature, const std::string &public_key,
                  crypto::DigestAlgorithm digest, const char *name) {
    const auto pkey = pem_to_key(public_key, false);

    const EVP_MD *md = crypto::get_evp_md(digest);
    if (!md) {
        throw std::runtime_error(std::string("Invalid digest algorithm for ") + name + " verification");
    }

    const detail::openssl_ptr<EVP_MD_CTX> md_ctx{EVP_MD_CTX_new()};
    if (!md_ctx) {
        throw std::runtime_error("Failed to create verification context: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());            // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    if (EVP_DigestVerifyInit(md_ctx.get(), nullptr, md, nullptr, pkey.get()) != 1) {
        throw std::runtime_error(std::string("Failed to initialize ") + name + " verification operation: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());                                            // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    if (EVP_DigestVerifyUpdate(md_ctx.get(), data.data(), data.size()) != 1) {
        throw std::runtime_error(std::string("Failed to update ") + name + " verification context: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());                                      // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    return EVP_DigestVerifyFinal(md_ctx.get(), signature.data(), signature.size()) == 1;
}

// Implementation of RSA signature
std::vector<unsigned char>
crypto::rsa_sign(const std::vector<unsigned char> &data, const std::string &private_key, DigestAlgorithm digest) {
    return digest_sign_pem(data, private_key, digest, "RSA");
}

// Implementation of RSA verification
bool
crypto::rsa_verify(const std::vector<unsigned char> &data, const std::vector<unsigned char> &signature, const std::string &public_key,
                   DigestAlgorithm digest) {
    return digest_verify_pem(data, signature, public_key, digest, "RSA");
}

// Implementation of EC signature
std::vector<unsigned char>
crypto::ec_sign(const std::vector<unsigned char> &data, const std::string &private_key, DigestAlgorithm digest) {
    return digest_sign_pem(data, private_key, digest, "EC");
}

// Implementation of EC verification
bool
crypto::ec_verify(const std::vector<unsigned char> &data, const std::vector<unsigned char> &signature, const std::string &public_key,
                  DigestAlgorithm digest) {
    return digest_verify_pem(data, signature, public_key, digest, "EC");
}

} // namespace qb
