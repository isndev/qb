/**
 * @file qb/io/src/tcp/ssl/socket.cpp
 * @brief Implementation of SSL/TLS socket functionality
 *
 * This file contains the implementation of secure socket operations using SSL/TLS
 * in the QB framework. It provides encrypted communication channels over TCP,
 * including certificate validation, secure handshake operations, and encrypted
 * data transmission.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2025 qb - isndev (cpp.actor)
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

#include <qb/io/tcp/ssl/socket.h>
#include <openssl/err.h>    // For error handling
#include <openssl/x509v3.h> // For advanced certificate properties and OCSP, SANs
#include <openssl/dh.h>     // For Diffie-Hellman parameters
#include <openssl/bn.h>     // For BIGNUM, needed for DH parameters file loading if not using PEM_read_bio_DHparams
#include <openssl/asn1.h>   // For ASN1_INTEGER, ASN1_TIME
#include <fstream>          // For reading DH parameters file if needed
#include <vector>
#include <iomanip> // For std::hex, std::setw, std::setfill
#include <sstream> // For std::ostringstream

// Helper function to convert ASN1_TIME to time_t
static time_t asn1_time_to_time_t(const ASN1_TIME *time) {
    if (!time) return 0;
    struct tm t;
    const char *str = (const char *)time->data;
    size_t i = 0;

    memset(&t, 0, sizeof(t));

    if (time->type == V_ASN1_UTCTIME) { /* YYMMDDHHMMSSZ */
        t.tm_year = (str[i++] - '0') * 10; t.tm_year += (str[i++] - '0');
        if (t.tm_year < 70) t.tm_year += 100; // Adjust for 2-digit year (UTCTIME is 1950-2049)
    } else if (time->type == V_ASN1_GENERALIZEDTIME) { /* YYYYMMDDHHMMSSZ */
        t.tm_year = (str[i++] - '0') * 1000; t.tm_year += (str[i++] - '0') * 100;
        t.tm_year += (str[i++] - '0') * 10; t.tm_year += (str[i++] - '0');
        t.tm_year -= 1900;
    }
    t.tm_mon = (str[i++] - '0') * 10; t.tm_mon += (str[i++] - '0'); --t.tm_mon; // Month is 0-11
    t.tm_mday = (str[i++] - '0') * 10; t.tm_mday += (str[i++] - '0');
    t.tm_hour = (str[i++] - '0') * 10; t.tm_hour += (str[i++] - '0');
    t.tm_min = (str[i++] - '0') * 10; t.tm_min += (str[i++] - '0');
    t.tm_sec = (str[i++] - '0') * 10; t.tm_sec += (str[i++] - '0');

    return mktime(&t); // mktime expects local time, OpenSSL times are usually UTC. GMT adjustment might be needed based on system.
}

// Helper to convert ASN1_INTEGER to hex string
static std::string asn1_integer_to_hex_string(const ASN1_INTEGER *bs) {
    if (!bs) return "";
    std::ostringstream oss;
    for (int i = 0; i < bs->length; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(bs->data[i]);
    }
    return oss.str();
}

namespace qb::io::ssl {

Certificate
get_certificate(SSL *ssl) {
    X509       *cert = nullptr;
    if (ssl) cert = SSL_get_peer_certificate(ssl);
    
    Certificate ret{};
    if (!cert && ssl) {
         return ret;
    } else if (!cert && !ssl) {
        return ret;
    }

    char *line;
    if (cert != nullptr) {
        line        = X509_NAME_oneline(X509_get_subject_name(cert), nullptr, 0);
        if (line) { ret.subject = line; OPENSSL_free(line); }
        line       = X509_NAME_oneline(X509_get_issuer_name(cert), nullptr, 0);
        if (line) { ret.issuer = line; OPENSSL_free(line); }
        ret.version = X509_get_version(cert);

        const ASN1_INTEGER *serial = X509_get_serialNumber(cert);
        ret.serial_number = asn1_integer_to_hex_string(serial);

        const ASN1_TIME *not_before_asn1 = X509_get0_notBefore(cert);
        ret.not_before = asn1_time_to_time_t(not_before_asn1);

        const ASN1_TIME *not_after_asn1 = X509_get0_notAfter(cert);
        ret.not_after = asn1_time_to_time_t(not_after_asn1);

        int sig_nid = X509_get_signature_nid(cert);
        const char *sig_algo_name = OBJ_nid2ln(sig_nid);
        if (sig_algo_name) ret.signature_algorithm = sig_algo_name;

        GENERAL_NAMES *sans = static_cast<GENERAL_NAMES*>(X509_get_ext_d2i(cert, NID_subject_alt_name, nullptr, nullptr));
        if (sans) {
            for (int i = 0; i < sk_GENERAL_NAME_num(sans); ++i) {
                GENERAL_NAME *san = sk_GENERAL_NAME_value(sans, i);
                std::string san_str;
                if (san->type == GEN_DNS) {
                    ASN1_IA5STRING *dns_name = san->d.dNSName;
                    if (dns_name && dns_name->data && dns_name->length > 0) {
                        san_str = std::string("DNS:") + reinterpret_cast<char*>(dns_name->data);
                    }
                } else if (san->type == GEN_IPADD) {
                    ASN1_OCTET_STRING *ip_addr = san->d.iPAddress;
                    if (ip_addr && ip_addr->data) {
                        san_str = "IP:";
                        if (ip_addr->length == 4) { // IPv4
                            std::ostringstream oss;
                            oss << static_cast<int>(ip_addr->data[0]) << "."
                                << static_cast<int>(ip_addr->data[1]) << "."
                                << static_cast<int>(ip_addr->data[2]) << "."
                                << static_cast<int>(ip_addr->data[3]);
                            san_str += oss.str();
                        } else if (ip_addr->length == 16) { // IPv6
                             std::ostringstream oss;
                             for(int j=0; j < ip_addr->length; ++j) {
                                oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(ip_addr->data[j]);
                                if (j % 2 == 1 && j < ip_addr->length -1) oss << ":";
                             }
                             san_str += oss.str();
                        }
                    }
                }
                if (!san_str.empty()) {
                    ret.subject_alternative_names.push_back(san_str);
                }
            }
            GENERAL_NAMES_free(sans);
        }

        X509_free(cert);
    }

    return ret;
}

SSL_CTX *
create_client_context(const SSL_METHOD *method) {
    return method ? SSL_CTX_new(method) : nullptr;
}

SSL_CTX *
create_server_context(const SSL_METHOD *method, std::filesystem::path cert_path,
                      std::filesystem::path key_path) {
    SSL_CTX *ctx = nullptr;
    if (!method)
        goto error;
    ctx = SSL_CTX_new(method);
    if (!ctx ||
        SSL_CTX_use_certificate_file(ctx, cert_path.string().c_str(), SSL_FILETYPE_PEM) <= 0 ||
        SSL_CTX_use_PrivateKey_file(ctx, key_path.string().c_str(), SSL_FILETYPE_PEM) <= 0 ||
        SSL_CTX_check_private_key(ctx) <= 0)
        goto error;

    return ctx;
error:
    if (ctx)
        SSL_CTX_free(ctx);
    return nullptr;
}

bool load_ca_certificates(SSL_CTX *ctx, const std::string &ca_file_path) {
    if (!ctx || ca_file_path.empty()) return false;
    if (SSL_CTX_load_verify_locations(ctx, ca_file_path.c_str(), nullptr) != 1) {
        // Consider logging ERR_get_error() here
        return false;
    }
    return true;
}

bool load_ca_directory(SSL_CTX *ctx, const std::string &ca_dir_path) {
    if (!ctx || ca_dir_path.empty()) return false;
    if (SSL_CTX_load_verify_locations(ctx, nullptr, ca_dir_path.c_str()) != 1) {
        // Consider logging ERR_get_error() here
        return false;
    }
    return true;
}

bool set_cipher_list(SSL_CTX *ctx, const std::string &ciphers) {
    if (!ctx || ciphers.empty()) return false;
    if (SSL_CTX_set_cipher_list(ctx, ciphers.c_str()) != 1) {
        // Consider logging ERR_get_error() here
        return false;
    }
    return true;
}

bool set_ciphersuites_tls13(SSL_CTX *ctx, const std::string &ciphersuites) {
    if (!ctx || ciphersuites.empty()) return false;
    if (SSL_CTX_set_ciphersuites(ctx, ciphersuites.c_str()) != 1) {
        // Consider logging ERR_get_error() here
        return false;
    }
    return true;
}

bool set_tls_protocol_versions(SSL_CTX *ctx, int min_version, int max_version) {
    if (!ctx) return false;
    bool success = true;
    if (min_version != 0) {
        if (SSL_CTX_set_min_proto_version(ctx, min_version) != 1) {
            // Consider logging ERR_get_error() here
            success = false;
        }
    }
    if (max_version != 0) {
        if (SSL_CTX_set_max_proto_version(ctx, max_version) != 1) {
            // Consider logging ERR_get_error() here
            success = false;
        }
    }
    return success;
}

bool configure_mtls_server_context(SSL_CTX *ctx, const std::string &client_ca_file_path, int verification_mode) {
    if (!ctx) return false;

    SSL_CTX_set_verify(ctx, verification_mode, nullptr); // Using OpenSSL's default verify callback

    if (!client_ca_file_path.empty()) {
        if (SSL_CTX_load_verify_locations(ctx, client_ca_file_path.c_str(), nullptr) != 1) {
            // Consider logging ERR_get_error() here
            return false;
        }
    }
    // If client_ca_file_path is empty, it relies on previously loaded CAs or system defaults if applicable.
    // For mTLS, it's common to have a specific CA for client certs.
    return true;
}

bool configure_client_certificate(SSL_CTX *ctx, const std::string &client_cert_path, const std::string &client_key_path) {
    if (!ctx || client_cert_path.empty() || client_key_path.empty()) return false;

    if (SSL_CTX_use_certificate_file(ctx, client_cert_path.c_str(), SSL_FILETYPE_PEM) <= 0) {
        // Consider logging ERR_get_error() here
        return false;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, client_key_path.c_str(), SSL_FILETYPE_PEM) <= 0) {
        // Consider logging ERR_get_error() here
        return false;
    }
    if (SSL_CTX_check_private_key(ctx) <= 0) {
        // Consider logging ERR_get_error() here
        return false;
    }
    return true;
}


// Helper to convert std::vector<std::string> to the format OpenSSL needs for ALPN protos
// (length-prefixed strings concatenated: <len1><proto1><len2><proto2>...)
static std::vector<unsigned char> serialize_alpn_protocols(const std::vector<std::string>& protocols) {
    std::vector<unsigned char> out;
    for (const auto& proto : protocols) {
        if (proto.length() > 255) continue; // Protocol too long, skip
        out.push_back(static_cast<unsigned char>(proto.length()));
        out.insert(out.end(), proto.begin(), proto.end());
    }
    return out;
}

bool set_alpn_protos_client(SSL_CTX *ctx, const std::vector<std::string>& protocols) {
    if (!ctx || protocols.empty()) return false;
    std::vector<unsigned char> serialized_protos = serialize_alpn_protocols(protocols);
    if (serialized_protos.empty()) return false; // No valid protocols to set

    if (SSL_CTX_set_alpn_protos(ctx, serialized_protos.data(), static_cast<unsigned int>(serialized_protos.size())) != 0) {
        // SSL_CTX_set_alpn_protos returns 0 on success, non-0 on failure.
        // Consider logging ERR_get_error() here
        return false;
    }
    return true;
}

bool set_alpn_selection_callback_server(SSL_CTX *ctx, SSL_CTX_alpn_select_cb_func callback, void *arg) {
    if (!ctx || !callback) return false;
    SSL_CTX_set_alpn_select_cb(ctx, callback, arg);
    return true; // SSL_CTX_set_alpn_select_cb doesn't have a direct error return like others, assumes success if non-null callback.
}

bool enable_server_session_caching(SSL_CTX *ctx, long cache_size) {
    if (!ctx) return false;
    SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_SERVER);
    if (cache_size != 0) { // 0 means unlimited for SSL_CTX_sess_set_cache_size, OpenSSL default is SSL_SESSION_CACHE_MAX_SIZE_DEFAULT
        SSL_CTX_sess_set_cache_size(ctx, cache_size);
    }
    // SSL_CTX_set_timeout can also be relevant here. Using default for now.
    return true; // These functions typically return the previous value, not direct success/failure. Assuming success.
}

bool disable_client_session_cache(SSL_CTX *ctx) {
    if (!ctx) return false;
    SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_OFF | SSL_SESS_CACHE_NO_INTERNAL_STORE);
    return true;
}

bool set_custom_verify_callback(SSL_CTX *ctx, int (*callback)(int, X509_STORE_CTX *), int verification_mode) {
    if (!ctx) return false;
    SSL_CTX_set_verify(ctx, verification_mode, callback);
    return true;
}

bool set_ocsp_stapling_client_callback(SSL_CTX *ctx, int (*callback)(SSL *s, void *arg), void *arg) {
    if (!ctx) return false;
    SSL_CTX_set_tlsext_status_cb(ctx, callback);
    SSL_CTX_set_tlsext_status_arg(ctx, arg);
    return true;
}

bool set_ocsp_stapling_responder_server(SSL_CTX *ctx, int (*callback)(SSL *s, void *arg), void *arg) {
    if (!ctx) return false;
    // For the server to provide an OCSP response, it also uses SSL_CTX_set_tlsext_status_cb.
    // The callback is responsible for calling SSL_set_tlsext_status_ocsp_resp.
    SSL_CTX_set_tlsext_status_cb(ctx, callback);
    SSL_CTX_set_tlsext_status_arg(ctx, arg);
    return true;
}

bool set_sni_hostname_selection_callback_server(SSL_CTX *ctx, int (*callback)(SSL *s, int *al, void *arg), void *arg) {
    if (!ctx || !callback) return false;
    SSL_CTX_set_tlsext_servername_callback(ctx, callback);
    SSL_CTX_set_tlsext_servername_arg(ctx, arg);
    return true;
}

bool set_keylog_callback(SSL_CTX *ctx, SSL_CTX_keylog_cb_func callback) {
    if (!ctx || !callback) return false;
    SSL_CTX_set_keylog_callback(ctx, callback);
    return true;
}

bool configure_dh_parameters_server(SSL_CTX* ctx, const std::string& dh_param_file_path) {
    if (!ctx || dh_param_file_path.empty()) return false;

#if OPENSSL_VERSION_NUMBER >= 0x30000000L // OpenSSL 3.0+
    // Use modern EVP_PKEY approach for OpenSSL 3.0+
    BIO *bio = BIO_new_file(dh_param_file_path.c_str(), "r");
    if (!bio) {
        // Consider logging ERR_get_error()
        return false;
    }

    EVP_PKEY *pkey = PEM_read_bio_Parameters(bio, nullptr);
    BIO_free(bio);

    if (!pkey) {
        // Consider logging ERR_get_error()
        return false;
    }

    if (SSL_CTX_set0_tmp_dh_pkey(ctx, pkey) != 1) {
        EVP_PKEY_free(pkey);
        // Consider logging ERR_get_error()
        return false;
    }
    // SSL_CTX_set0_tmp_dh_pkey takes ownership of pkey, so don't free it on success

#else
    // Legacy approach for older OpenSSL versions
    BIO *bio = BIO_new_file(dh_param_file_path.c_str(), "r");
    if (!bio) {
        // Consider logging ERR_get_error()
        return false;
    }

    DH *dh = PEM_read_bio_DHparams(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);

    if (!dh) {
        // Consider logging ERR_get_error()
        return false;
    }

    if (SSL_CTX_set_tmp_dh(ctx, dh) != 1) {
        DH_free(dh);
        // Consider logging ERR_get_error()
        return false;
    }
    // For older OpenSSL versions, we need to explicitly free DH after SSL_CTX_set_tmp_dh
    DH_free(dh);
#endif

    return true;
}

bool configure_ecdh_curves_server(SSL_CTX* ctx, const std::string& curve_names_list) {
    if (!ctx) return false;
    #if OPENSSL_VERSION_NUMBER >= 0x10100000L
    if (!curve_names_list.empty()) {
        if (SSL_CTX_set1_curves_list(ctx, curve_names_list.c_str()) != 1) {
            return false;
        }
    } 
    #else
    if (!curve_names_list.empty()) {
        return false; 
    }
    #endif
    return true;
}

void free_session(Session& session) {
    if (session._session_handle) {
        SSL_SESSION_free(session._session_handle);
        session._session_handle = nullptr;
    }
}

bool enable_post_handshake_auth_server(SSL_CTX* ctx) {
    if (!ctx) return false;
#if OPENSSL_VERSION_NUMBER >= 0x10101000L // SSL_CTX_set_post_handshake_auth available from 1.1.1
    SSL_CTX_set_post_handshake_auth(ctx, 1);
    return true;
#else
    return false; // Feature not available
#endif
}

} // namespace qb::io::ssl

namespace qb::io::tcp::ssl {

socket::socket() noexcept
    : tcp::socket()
    , _ssl_handle(nullptr, SSL_free)
    , _connected(false) {}

socket::socket(SSL *ctx, tcp::socket &sock) noexcept
    : tcp::socket(std::move(sock))
    , _ssl_handle(ctx, SSL_free)
    , _connected(false) {}

socket::~socket() noexcept {
    if (_ssl_handle) {
        const auto handle    = ssl_handle();
        const auto ctx       = SSL_get_SSL_CTX(handle);
        const auto is_client = !SSL_is_server(handle);
        // SSL_shutdown(handle);
        // SSL_free(handle);
        if (is_client)
            SSL_CTX_free(ctx);
        _ssl_handle.reset(nullptr);
        _connected = false;
    }
}

void
socket::init(SSL *handle) noexcept {
    _connected = false;
    if (!_ssl_handle) {
        const auto ctx = SSL_CTX_new(SSLv23_client_method());
        _ssl_handle.reset(SSL_new(ctx));
        if (!_ssl_handle) {
            SSL_CTX_free(ctx);
            return;
        }
    } else 
    _ssl_handle.reset(handle);
}

int
socket::handCheck() noexcept {
    if (_connected)
        return 1;
    auto ret = SSL_do_handshake(ssl_handle());
    if (ret != 1) {
        auto err = SSL_get_error(ssl_handle(), ret);
        switch (err) {
            case SSL_ERROR_WANT_WRITE:
            case SSL_ERROR_WANT_READ:
                return 0;
            default:
                disconnect();
                return -1;
        }
    }
    _connected = true;
    return 1;
}

int
socket::connect_in(int af, std::string const &host, uint16_t port) noexcept {
    auto ret = -1;
    qb::io::socket::resolve_i(
        [&, this](const auto &ep) {
            if (ep.af() == af) {
                ret = connect(ep, host);
                return true;
            }
            return false;
        },
        host.c_str(), port, af, SOCK_STREAM);

    return ret;
}

int
socket::connect(endpoint const &ep, std::string const &hostname) noexcept {
    auto ret = tcp::socket::connect(ep);
    if (ret != 0 && !socket_no_error(qb::io::socket::get_last_errno()))
        return ret;
    if (!_ssl_handle) {
        const auto ctx = SSL_CTX_new(SSLv23_client_method());
        _ssl_handle.reset(SSL_new(ctx));
        if (!_ssl_handle) {
            SSL_CTX_free(ctx);
            tcp::socket::disconnect();
            return SocketStatus::Error;
        }
    }
    const auto h_ssl = ssl_handle();
    SSL_set_quiet_shutdown(h_ssl, 1);
    SSL_set_tlsext_host_name(h_ssl, hostname.c_str());
    SSL_set_connect_state(h_ssl);
    SSL_set_fd(h_ssl, static_cast<int>(native_handle()));
    if (ret != 0)
        return ret;
    return handCheck() < 0 ? -1 : 0;
}

int
socket::connect(uri const &u) noexcept {
    switch (u.af()) {
        case AF_INET:
        case AF_INET6:
            // Ensure hostname from URI is used for SNI
            return connect_in(u.af(), std::string(u.host()), u.u_port());
        case AF_UNIX:
            const auto path = std::string(u.path()) + std::string(u.host());
            return connect_un(path);
    }
    return -1;
}

int
socket::connect_v4(std::string const &host, uint16_t port) noexcept {
    return connect_in(AF_INET, host, port);
}

int
socket::connect_v6(std::string const &host, uint16_t port) noexcept {
    return connect_in(AF_INET6, host, port);
}

int
socket::connect_un(std::string const &path) noexcept {
    return connect(endpoint().as_un(path.c_str()));
}

// NON BLOCKING
int
socket::n_connect_in(int af, std::string const &host, uint16_t port) noexcept {
    auto ret = -1;
    qb::io::socket::resolve_i(
        [&, this](const auto &ep) {
            if (ep.af() == af) {
                ret = n_connect(ep, host);
                return true;
            }
            return false;
        },
        host.c_str(), port, af, SOCK_STREAM);

    return ret;
}

int
socket::n_connect(endpoint const &ep, std::string const &hostname) noexcept {
    auto ret = tcp::socket::n_connect(ep);
    if (ret != 0 && !socket_no_error(qb::io::socket::get_last_errno()))
        return ret;
    if (!_ssl_handle) {
        const auto ctx = SSL_CTX_new(SSLv23_client_method());
        _ssl_handle.reset(SSL_new(ctx));
        if (!_ssl_handle) {
            SSL_CTX_free(ctx);
            tcp::socket::disconnect();
            return SocketStatus::Error;
        }
    }
    const auto h_ssl = ssl_handle();
    SSL_set_quiet_shutdown(h_ssl, 1);
    SSL_set_tlsext_host_name(h_ssl, hostname.c_str());
    unsigned char alpn[] = { 2, 'h', '2' };
    SSL_set_alpn_protos(h_ssl, alpn, sizeof(alpn));
    SSL_set_connect_state(h_ssl);

    return ret;
}

// used for async
int
socket::connected() noexcept {
    if (!_ssl_handle)
        return -1;
    const auto h_ssl = ssl_handle();
    SSL_set_fd(h_ssl, static_cast<int>(native_handle()));
    return handCheck() < 0 ? -1 : 0;
}

int
socket::n_connect(uri const &u) noexcept {
    switch (u.af()) {
        case AF_INET:
        case AF_INET6:
            // Ensure hostname from URI is used for SNI
            return n_connect_in(u.af(), std::string(u.host()), u.u_port());
        case AF_UNIX:
            const auto path = std::string(u.path()) + std::string(u.host());
            return n_connect_un(path);
    }
    return -1;
}

int
socket::n_connect_v4(std::string const &host, uint16_t port) noexcept {
    return n_connect_in(AF_INET, host, port);
}

int
socket::n_connect_v6(std::string const &host, uint16_t port) noexcept {
    return n_connect_in(AF_INET6, host, port);
}

int
socket::n_connect_un(std::string const &path) noexcept {
    return n_connect(endpoint().as_un(path.c_str()));
}

int
socket::disconnect() noexcept {
    _connected = false;
    //    SSL_shutdown(ssl_handle());
    return tcp::socket::disconnect();
}

int
socket::read(void *data, std::size_t size) noexcept {
    auto ret = handCheck();
    if (ret == 1) {
        ret = SSL_read(ssl_handle(), data, static_cast<int>(size));
        if (ret < 0) {
            auto err = SSL_get_error(ssl_handle(), ret);
            switch (err) {
                case SSL_ERROR_WANT_WRITE:
                case SSL_ERROR_WANT_READ:
                    return 0;
                default:
                    return -1;
            }
        }
    }
    return ret;
}

int
socket::write(const void *data, std::size_t size) noexcept {
    auto ret = handCheck();
    if (ret == 1) {
        ret = SSL_write(ssl_handle(), data, static_cast<int>(size));
        if (ret < 0) {
            auto err = SSL_get_error(ssl_handle(), ret);
            switch (err) {
                case SSL_ERROR_WANT_WRITE:
                case SSL_ERROR_WANT_READ:
                    return 0;
                default:
                    return -1;
            }
        }
    }
    return ret;
}

[[nodiscard]] SSL *
socket::ssl_handle() const noexcept {
    return _ssl_handle.get();
}

qb::io::ssl::Certificate
socket::get_peer_certificate_details() const noexcept {
    if (!_connected || !_ssl_handle) {
        return {};
    }
    return qb::io::ssl::get_certificate(_ssl_handle.get());
}

std::string
socket::get_negotiated_cipher_suite() const noexcept {
    if (!_connected || !_ssl_handle) {
        return "";
    }
    const char* cipher_name = SSL_get_cipher_name(_ssl_handle.get());
    return cipher_name ? std::string(cipher_name) : "";
}

std::string
socket::get_negotiated_tls_version() const noexcept {
    if (!_connected || !_ssl_handle) {
        return "";
    }
    const char* tls_version = SSL_get_version(_ssl_handle.get());
    return tls_version ? std::string(tls_version) : "";
}

std::string
socket::get_alpn_selected_protocol() const noexcept {
    if (!_connected || !_ssl_handle) {
        return "";
    }
    const unsigned char *data = nullptr;
    unsigned int len = 0;
    SSL_get0_alpn_selected(_ssl_handle.get(), &data, &len);
    if (data && len > 0) {
        return std::string(reinterpret_cast<const char*>(data), len);
    }
    return "";
}

std::string
socket::get_last_ssl_error_string() const noexcept {
    if (!_ssl_handle) {
        return "No SSL handle";
    }
    unsigned long err_code = ERR_peek_last_error();
    if (err_code == 0) {
        return "No SSL error in queue";
    }
    char err_buf[256]; 
    ERR_error_string_n(err_code, err_buf, sizeof(err_buf));
    return std::string(err_buf);
}

bool socket::disable_session_resumption() noexcept {
    if (!_ssl_handle) return false;
    SSL_set_options(_ssl_handle.get(), SSL_OP_NO_TICKET | SSL_OP_NO_SESSION_RESUMPTION_ON_RENEGOTIATION);
    SSL_set_session(_ssl_handle.get(), nullptr);
    return true;
}

bool socket::request_ocsp_stapling(bool enable) noexcept {
    if (!_ssl_handle) return false;
    if (enable) {
        if (SSL_set_tlsext_status_type(_ssl_handle.get(), TLSEXT_STATUSTYPE_ocsp) != 1) {
            return false;
        }
    }
    return true;
}

std::vector<qb::io::ssl::Certificate>
socket::get_peer_certificate_chain() const noexcept {
    std::vector<qb::io::ssl::Certificate> chain_info;
    if (!_connected || !_ssl_handle) {
        return chain_info;
    }

    STACK_OF(X509) *cert_stack = SSL_get_peer_cert_chain(_ssl_handle.get());
    if (!cert_stack) {
        return chain_info;
    }

    for (int i = 0; i < sk_X509_num(cert_stack); ++i) {
        X509 *cert = sk_X509_value(cert_stack, i);
        if (cert) {
            qb::io::ssl::Certificate cert_details;
            char *line;

            line = X509_NAME_oneline(X509_get_subject_name(cert), nullptr, 0);
            if (line) { cert_details.subject = line; OPENSSL_free(line); }
            line = X509_NAME_oneline(X509_get_issuer_name(cert), nullptr, 0);
            if (line) { cert_details.issuer = line; OPENSSL_free(line); }
            cert_details.version = X509_get_version(cert);
            
            const ASN1_INTEGER *serial = X509_get_serialNumber(cert);
            cert_details.serial_number = asn1_integer_to_hex_string(serial);

            const ASN1_TIME *not_before_asn1 = X509_get0_notBefore(cert);
            cert_details.not_before = asn1_time_to_time_t(not_before_asn1);

            const ASN1_TIME *not_after_asn1 = X509_get0_notAfter(cert);
            cert_details.not_after = asn1_time_to_time_t(not_after_asn1);

            int sig_nid = X509_get_signature_nid(cert);
            const char *sig_algo_name = OBJ_nid2ln(sig_nid);
            if (sig_algo_name) cert_details.signature_algorithm = sig_algo_name;

            GENERAL_NAMES *sans = static_cast<GENERAL_NAMES*>(X509_get_ext_d2i(cert, NID_subject_alt_name, nullptr, nullptr));
            if (sans) {
                for (int j = 0; j < sk_GENERAL_NAME_num(sans); ++j) {
                    GENERAL_NAME *san_entry = sk_GENERAL_NAME_value(sans, j);
                    std::string san_str_entry;
                    if (san_entry->type == GEN_DNS) {
                        ASN1_IA5STRING *dns_name = san_entry->d.dNSName;
                        if (dns_name && dns_name->data && dns_name->length > 0) {
                           san_str_entry = std::string("DNS:") + reinterpret_cast<char*>(dns_name->data);
                        }
                    } else if (san_entry->type == GEN_IPADD) {
                        ASN1_OCTET_STRING *ip_addr = san_entry->d.iPAddress;
                         if (ip_addr && ip_addr->data) {
                            san_str_entry = "IP:";
                            if (ip_addr->length == 4) { // IPv4
                                std::ostringstream oss_ip;
                                oss_ip << static_cast<int>(ip_addr->data[0]) << "."
                                    << static_cast<int>(ip_addr->data[1]) << "."
                                    << static_cast<int>(ip_addr->data[2]) << "."
                                    << static_cast<int>(ip_addr->data[3]);
                                san_str_entry += oss_ip.str();
                            } else if (ip_addr->length == 16) { // IPv6
                                 std::ostringstream oss_ip;
                                 for(int k=0; k < ip_addr->length; ++k) {
                                    oss_ip << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(ip_addr->data[k]);
                                    if (k % 2 == 1 && k < ip_addr->length -1) oss_ip << ":";
                                 }
                                 san_str_entry += oss_ip.str();
                            }
                        }
                    }
                    if (!san_str_entry.empty()) {
                        cert_details.subject_alternative_names.push_back(san_str_entry);
                    }
                }
                GENERAL_NAMES_free(sans);
            }
            chain_info.push_back(cert_details);
        }
    }
    return chain_info;
}

qb::io::ssl::Session
socket::get_session() const noexcept {
    qb::io::ssl::Session sess_obj{};
    if (!_connected || !_ssl_handle) {
        return sess_obj;
    }
    sess_obj._session_handle = SSL_get1_session(_ssl_handle.get()); // SSL_get1_session increments ref count
    return sess_obj;
}

bool socket::set_session(qb::io::ssl::Session& session) noexcept {
    if (!_ssl_handle || !session.is_valid()) {
        return false;
    }
    return SSL_set_session(_ssl_handle.get(), session._session_handle) == 1;
}

bool socket::request_client_post_handshake_auth() noexcept {
    if (!_connected || !_ssl_handle) return false;

#if OPENSSL_VERSION_NUMBER >= 0x10101000L // SSL_verify_client_post_handshake available from 1.1.1
    // Check if it's TLS 1.3
    if (SSL_version(_ssl_handle.get()) != TLS1_3_VERSION) {
        return false; // PHA is a TLS 1.3 feature
    }
    if (SSL_verify_client_post_handshake(_ssl_handle.get()) == 1) {
        return true;
    }
    return false;
#else
    return false; // Feature not available
#endif
}

namespace {
    // Local helper if not accessible from qb::io::ssl - ensure it's defined if needed
    static std::vector<unsigned char> serialize_alpn_protocols_for_socket(const std::vector<std::string>& protocols) {
        std::vector<unsigned char> out;
        for (const auto& proto : protocols) {
            if (proto.length() > 255) continue; // Protocol too long, skip
            out.push_back(static_cast<unsigned char>(proto.length()));
            out.insert(out.end(), proto.begin(), proto.end());
        }
        return out;
    }
}

bool socket::set_sni_hostname(const std::string& hostname) noexcept {
    if (!_ssl_handle || hostname.empty()) return false;
    // Convert hostname to C-string. Check for embedded nulls if necessary.
    if (SSL_set_tlsext_host_name(_ssl_handle.get(), hostname.c_str()) == 1) {
        return true;
    }
    return false;
}

bool socket::set_alpn_protocols(const std::vector<std::string>& protocols) noexcept {
    if (!_ssl_handle || protocols.empty()) return false;
    std::vector<unsigned char> serialized_protos = serialize_alpn_protocols_for_socket(protocols);
    if (serialized_protos.empty()) return false; 

    if (SSL_set_alpn_protos(_ssl_handle.get(), serialized_protos.data(), static_cast<unsigned int>(serialized_protos.size())) == 0) {
        // SSL_set_alpn_protos returns 0 on success for SSL object, non-0 on failure.
        return true;
    }
    return false;
}

bool socket::set_verify_callback(int (*callback)(int, X509_STORE_CTX *), int verification_mode) noexcept {
    if (!_ssl_handle) return false;
    SSL_set_verify(_ssl_handle.get(), verification_mode, callback);
    return true; // SSL_set_verify does not return a success/failure for the SSL object itself.
}

bool socket::set_verify_depth(int depth) noexcept {
    if (!_ssl_handle) return false;
    SSL_set_verify_depth(_ssl_handle.get(), depth);
    return true; // SSL_set_verify_depth does not return a success/failure.
}

} // namespace qb::io::tcp::ssl