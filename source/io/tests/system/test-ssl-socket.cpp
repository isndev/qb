/**
 * @file qb/source/io/tests/system/test-ssl-socket.cpp
 * @brief System tests for qb SSL/TLS socket and listener utilities.
 *
 * These tests cover SSL context configuration, listener wrappers and a real
 * loopback TLS handshake. They intentionally use the generated test
 * certificate and dynamic ports so the suite remains deterministic in CI.
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
 *
 * @ingroup Tests
 */

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <qb/io/tcp/ssl/listener.h>
#include <qb/io/tcp/ssl/socket.h>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <unistd.h>
#endif

using namespace std::chrono_literals;

namespace {

std::filesystem::path
ssl_resource_path(const char *file_name) {
    const std::array candidates{
        std::filesystem::current_path() / file_name,
        std::filesystem::current_path() / "ssl" / file_name,
        std::filesystem::path(__FILE__).parent_path() / "resources" / "ssl" / file_name,
        std::filesystem::path("resources") / "ssl" / file_name,
    };
    for (auto const &candidate : candidates) {
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
    }
    return candidates.front();
}

struct ssl_test_files {
    std::filesystem::path cert;
    std::filesystem::path key;
};

ssl_test_files
require_ssl_files() {
    return {ssl_resource_path("cert.pem"), ssl_resource_path("key.pem")};
}

bool
ssl_files_available(const ssl_test_files &files) {
    return std::filesystem::exists(files.cert) && std::filesystem::exists(files.key);
}

int
always_verify(int, X509_STORE_CTX *) {
    return 1;
}

int
ocsp_callback(SSL *, void *) {
    return SSL_TLSEXT_ERR_NOACK;
}

int
sni_callback(SSL *, int *, void *) {
    return SSL_TLSEXT_ERR_OK;
}

void
keylog_callback(const SSL *, const char *) {}

void
info_callback(const SSL *, int, int) {}

void
msg_callback(int, int, int, const void *, std::size_t, SSL *, void *) {}

int
select_first_alpn(SSL *, const unsigned char **out, unsigned char *outlen,
                  const unsigned char *in, unsigned int inlen, void *) {
    if (!in || inlen == 0) {
        return SSL_TLSEXT_ERR_NOACK;
    }
    *out = in + 1;
    *outlen = in[0];
    return SSL_TLSEXT_ERR_OK;
}

void
drive_server_handshake(qb::io::tcp::ssl::socket &socket) {
    for (int i = 0; i < 200 && !socket.handshake_complete(); ++i) {
        const int status = socket.handshake_status();
        ASSERT_GE(status, 0);
        if (status == 1) {
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    ASSERT_TRUE(socket.handshake_complete());
}

class thread_join_guard {
    std::thread          &_thread;
    std::function<void()> _before_join;

public:
    template <typename Fn>
    thread_join_guard(std::thread &thread, Fn &&before_join)
        : _thread(thread)
        , _before_join(std::forward<Fn>(before_join)) {}

    thread_join_guard(const thread_join_guard &)            = delete;
    thread_join_guard &operator=(const thread_join_guard &) = delete;

    ~thread_join_guard() {
        if (_thread.joinable()) {
            if (_before_join) {
                _before_join();
            }
            _thread.join();
        }
    }
};

::testing::AssertionResult
read_exactly(qb::io::tcp::ssl::socket &socket, void *data, std::size_t size,
             std::chrono::milliseconds timeout = 2s) {
    auto       *out      = static_cast<char *>(data);
    std::size_t received = 0;
    const auto  deadline = std::chrono::steady_clock::now() + timeout;
    int         last     = 0;

    while (received < size && std::chrono::steady_clock::now() < deadline) {
        const int ret = socket.read(out + received, size - received);
        last          = ret;
        if (ret > 0) {
            received += static_cast<std::size_t>(ret);
            continue;
        }
        if (ret < 0) {
            return ::testing::AssertionFailure()
                   << "SSL read failed after " << received << "/" << size << " bytes";
        }
        std::this_thread::sleep_for(1ms);
    }

    if (received == size) {
        return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure()
           << "timed out waiting for " << size << " SSL bytes; received " << received
           << ", last read result=" << last;
}

::testing::AssertionResult
write_exactly(qb::io::tcp::ssl::socket &socket, const void *data, std::size_t size,
              std::chrono::milliseconds timeout = 2s) {
    const auto *in       = static_cast<const char *>(data);
    std::size_t written  = 0;
    const auto  deadline = std::chrono::steady_clock::now() + timeout;
    int         last     = 0;

    while (written < size && std::chrono::steady_clock::now() < deadline) {
        const int ret = socket.write(in + written, size - written);
        last          = ret;
        if (ret > 0) {
            written += static_cast<std::size_t>(ret);
            continue;
        }
        if (ret < 0) {
            return ::testing::AssertionFailure()
                   << "SSL write failed after " << written << "/" << size << " bytes";
        }
        std::this_thread::sleep_for(1ms);
    }

    if (written == size) {
        return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure()
           << "timed out writing " << size << " SSL bytes; wrote " << written
           << ", last write result=" << last;
}

bool
record_thread_failure(::testing::AssertionResult result) {
    if (result) {
        return true;
    }
    ADD_FAILURE() << result.message();
    return false;
}

} // namespace

TEST(SSLContext, NullAndInvalidInputsFailCleanly) {
    EXPECT_FALSE(qb::io::ssl::attach_socket(nullptr, qb::io::inet::invalid_socket));
    EXPECT_TRUE(qb::io::ssl::get_certificate(nullptr).subject.empty());
    EXPECT_EQ(qb::io::ssl::create_client_context(nullptr), nullptr);
    EXPECT_EQ(qb::io::ssl::create_server_context(nullptr, {}, {}), nullptr);
    EXPECT_EQ(qb::io::ssl::create_server_context(TLS_server_method(),
                                                 "missing-cert.pem", "missing-key.pem"),
              nullptr);

    EXPECT_FALSE(qb::io::ssl::load_ca_certificates(nullptr, "ca.pem"));
    EXPECT_FALSE(qb::io::ssl::load_ca_directory(nullptr, "."));
    EXPECT_FALSE(qb::io::ssl::set_cipher_list(nullptr, "HIGH"));
    EXPECT_FALSE(qb::io::ssl::set_ciphersuites_tls13(nullptr, "TLS_AES_128_GCM_SHA256"));
    EXPECT_FALSE(qb::io::ssl::set_tls_protocol_versions(nullptr, TLS1_2_VERSION, 0));
    EXPECT_FALSE(qb::io::ssl::configure_mtls_server_context(nullptr, ""));
    EXPECT_FALSE(qb::io::ssl::configure_client_certificate(nullptr, "cert.pem", "key.pem"));
    EXPECT_FALSE(qb::io::ssl::set_alpn_protos_client(nullptr, {"h2"}));
    EXPECT_FALSE(qb::io::ssl::set_alpn_selection_callback_server(nullptr, select_first_alpn, nullptr));
    EXPECT_FALSE(qb::io::ssl::enable_server_session_caching(nullptr, 16));
    EXPECT_FALSE(qb::io::ssl::disable_client_session_cache(nullptr));
    EXPECT_FALSE(qb::io::ssl::set_custom_verify_callback(nullptr, always_verify, SSL_VERIFY_PEER));
    EXPECT_FALSE(qb::io::ssl::set_ocsp_stapling_client_callback(nullptr, ocsp_callback, nullptr));
    EXPECT_FALSE(qb::io::ssl::set_ocsp_stapling_responder_server(nullptr, ocsp_callback, nullptr));
    EXPECT_FALSE(qb::io::ssl::set_sni_hostname_selection_callback_server(nullptr, sni_callback, nullptr));
    EXPECT_FALSE(qb::io::ssl::set_keylog_callback(nullptr, keylog_callback));
    EXPECT_FALSE(qb::io::ssl::configure_dh_parameters_server(nullptr, "dh.pem"));
    EXPECT_FALSE(qb::io::ssl::configure_ecdh_curves_server(nullptr, "prime256v1"));
    EXPECT_FALSE(qb::io::ssl::enable_post_handshake_auth_server(nullptr));

    qb::io::ssl::Session session;
    qb::io::ssl::free_session(session);
    EXPECT_FALSE(session.is_valid());
}

TEST(SSLContext, ConfiguresClientAndServerContexts) {
    const auto files = require_ssl_files();
    if (!ssl_files_available(files)) {
        GTEST_SKIP() << "SSL certificate resources are not available";
    }

    auto *client = qb::io::ssl::create_client_context(TLS_client_method());
    ASSERT_NE(client, nullptr);
    auto client_guard = std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)>(client, SSL_CTX_free);

    auto *server = qb::io::ssl::create_server_context(TLS_server_method(),
                                                      files.cert, files.key);
    ASSERT_NE(server, nullptr);
    auto server_guard = std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)>(server, SSL_CTX_free);

    EXPECT_TRUE(qb::io::ssl::load_ca_certificates(client, files.cert.string()));
    EXPECT_FALSE(qb::io::ssl::load_ca_certificates(client, ""));
    EXPECT_FALSE(qb::io::ssl::load_ca_directory(client, ""));
    EXPECT_TRUE(qb::io::ssl::set_cipher_list(client, "HIGH:!aNULL"));
    EXPECT_FALSE(qb::io::ssl::set_cipher_list(client, ""));
    EXPECT_TRUE(qb::io::ssl::set_ciphersuites_tls13(client, "TLS_AES_128_GCM_SHA256"));
    EXPECT_FALSE(qb::io::ssl::set_ciphersuites_tls13(client, ""));
    EXPECT_TRUE(qb::io::ssl::set_tls_protocol_versions(client, TLS1_2_VERSION, 0));
    EXPECT_TRUE(qb::io::ssl::configure_client_certificate(client,
                                                         files.cert.string(),
                                                         files.key.string()));
    EXPECT_FALSE(qb::io::ssl::configure_client_certificate(client, "", files.key.string()));
    EXPECT_TRUE(qb::io::ssl::set_alpn_protos_client(client, {"h2", "http/1.1"}));
    EXPECT_FALSE(qb::io::ssl::set_alpn_protos_client(client, {}));
    EXPECT_FALSE(qb::io::ssl::set_alpn_protos_client(client, {std::string(256, 'x')}));
    EXPECT_TRUE(qb::io::ssl::disable_client_session_cache(client));
    EXPECT_TRUE(qb::io::ssl::set_custom_verify_callback(client, always_verify, SSL_VERIFY_PEER));
    EXPECT_TRUE(qb::io::ssl::set_ocsp_stapling_client_callback(client, ocsp_callback, nullptr));
    EXPECT_TRUE(qb::io::ssl::set_keylog_callback(client, keylog_callback));

    EXPECT_TRUE(qb::io::ssl::configure_mtls_server_context(server, files.cert.string()));
    EXPECT_TRUE(qb::io::ssl::configure_mtls_server_context(server, ""));
    EXPECT_FALSE(qb::io::ssl::configure_mtls_server_context(server, "missing-ca.pem"));
    EXPECT_TRUE(qb::io::ssl::set_alpn_selection_callback_server(server, select_first_alpn, nullptr));
    EXPECT_FALSE(qb::io::ssl::set_alpn_selection_callback_server(server, nullptr, nullptr));
    EXPECT_TRUE(qb::io::ssl::enable_server_session_caching(server, 32));
    EXPECT_TRUE(qb::io::ssl::set_ocsp_stapling_responder_server(server, ocsp_callback, nullptr));
    EXPECT_TRUE(qb::io::ssl::set_sni_hostname_selection_callback_server(server, sni_callback, nullptr));
    EXPECT_TRUE(qb::io::ssl::configure_ecdh_curves_server(server, "prime256v1"));
    EXPECT_FALSE(qb::io::ssl::configure_ecdh_curves_server(server, "not-a-curve"));
    EXPECT_FALSE(qb::io::ssl::configure_dh_parameters_server(server, "missing-dh.pem"));
    EXPECT_TRUE(qb::io::ssl::enable_post_handshake_auth_server(server));
}

TEST(SSLContext, RejectsInvalidConfigurationOnRealContexts) {
    const auto files = require_ssl_files();
    if (!ssl_files_available(files)) {
        GTEST_SKIP() << "SSL certificate resources are not available";
    }

    auto *client = qb::io::ssl::create_client_context(TLS_client_method());
    ASSERT_NE(client, nullptr);
    auto client_guard = std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)>(client, SSL_CTX_free);

    EXPECT_FALSE(qb::io::ssl::load_ca_certificates(client, "missing-ca.pem"));
    EXPECT_FALSE(qb::io::ssl::set_cipher_list(client, "NO-SUCH-CIPHER"));
    EXPECT_FALSE(qb::io::ssl::set_ciphersuites_tls13(client, "NO_SUCH_TLS13_SUITE"));
    EXPECT_FALSE(qb::io::ssl::configure_client_certificate(client,
                                                          "missing-cert.pem",
                                                          files.key.string()));
    EXPECT_FALSE(qb::io::ssl::configure_client_certificate(client,
                                                          files.cert.string(),
                                                          "missing-key.pem"));

    auto *server = qb::io::ssl::create_server_context(TLS_server_method(),
                                                      files.cert, files.key);
    ASSERT_NE(server, nullptr);
    auto server_guard = std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)>(server, SSL_CTX_free);

    EXPECT_FALSE(qb::io::ssl::configure_mtls_server_context(server, "missing-client-ca.pem"));
    EXPECT_FALSE(qb::io::ssl::configure_ecdh_curves_server(server, "not-a-curve"));
    EXPECT_FALSE(qb::io::ssl::configure_dh_parameters_server(server, "missing-dh-params.pem"));
}

TEST(SSLListener, WrappersExposeContextState) {
    qb::io::tcp::ssl::listener uninitialized;
    EXPECT_EQ(uninitialized.ssl_handle(), nullptr);
    EXPECT_FALSE(uninitialized.load_ca_certificates_for_client_auth("ca.pem"));
    EXPECT_FALSE(uninitialized.load_ca_directory_for_client_auth("."));
    EXPECT_FALSE(uninitialized.set_cipher_list("HIGH"));
    EXPECT_FALSE(uninitialized.set_ciphersuites_tls13("TLS_AES_128_GCM_SHA256"));
    EXPECT_FALSE(uninitialized.set_tls_protocol_versions(TLS1_2_VERSION, 0));
    EXPECT_FALSE(uninitialized.configure_mtls(""));
    EXPECT_FALSE(uninitialized.set_alpn_selection_callback(select_first_alpn, nullptr));
    EXPECT_FALSE(uninitialized.enable_session_caching());
    EXPECT_FALSE(uninitialized.set_custom_client_verify_callback(always_verify, SSL_VERIFY_PEER));
    EXPECT_FALSE(uninitialized.set_ocsp_stapling_responder_callback(ocsp_callback, nullptr));
    EXPECT_FALSE(uninitialized.set_sni_selection_callback(sni_callback, nullptr));
    EXPECT_FALSE(uninitialized.set_keylog_callback(keylog_callback));
    EXPECT_FALSE(uninitialized.configure_dh_parameters("dh.pem"));
    EXPECT_FALSE(uninitialized.configure_ecdh_curves("prime256v1"));
    EXPECT_FALSE(uninitialized.enable_post_handshake_auth());
    EXPECT_FALSE(uninitialized.set_supported_alpn_protocols({"h2"}));
    EXPECT_EQ(uninitialized.get_min_protocol_version(), 0);
    EXPECT_EQ(uninitialized.get_max_protocol_version(), 0);
    EXPECT_EQ(uninitialized.get_verify_mode(), -1);
    EXPECT_EQ(uninitialized.get_verify_depth(), -1);
    EXPECT_EQ(uninitialized.get_session_cache_mode(), -1);
    EXPECT_EQ(uninitialized.get_session_cache_size(), -1);
    EXPECT_EQ(uninitialized.set_options(SSL_OP_NO_COMPRESSION), 0);
    EXPECT_EQ(uninitialized.clear_options(SSL_OP_NO_COMPRESSION), 0);
    EXPECT_EQ(uninitialized.set_session_timeout(10), 0);
    EXPECT_FALSE(uninitialized.set_info_callback(info_callback));
    EXPECT_FALSE(uninitialized.set_msg_callback(msg_callback, nullptr));

    const auto files = require_ssl_files();
    if (!ssl_files_available(files)) {
        GTEST_SKIP() << "SSL certificate resources are not available";
    }
    qb::io::tcp::ssl::listener listener;
    listener.init(qb::io::ssl::create_server_context(TLS_server_method(),
                                                     files.cert, files.key));
    ASSERT_NE(listener.ssl_handle(), nullptr);

    EXPECT_TRUE(listener.load_ca_certificates_for_client_auth(files.cert.string()));
    EXPECT_FALSE(listener.load_ca_directory_for_client_auth(""));
    EXPECT_TRUE(listener.set_cipher_list("HIGH:!aNULL"));
    EXPECT_TRUE(listener.set_ciphersuites_tls13("TLS_AES_128_GCM_SHA256"));
    EXPECT_TRUE(listener.set_tls_protocol_versions(TLS1_2_VERSION, TLS1_3_VERSION));
    EXPECT_EQ(listener.get_min_protocol_version(), TLS1_2_VERSION);
    EXPECT_EQ(listener.get_max_protocol_version(), TLS1_3_VERSION);
    EXPECT_TRUE(listener.configure_mtls("", SSL_VERIFY_PEER));
    EXPECT_NE(listener.get_verify_mode(), -1);
    EXPECT_TRUE(listener.set_alpn_selection_callback(select_first_alpn, nullptr));
    EXPECT_TRUE(listener.enable_session_caching(64));
    EXPECT_GT(listener.get_session_cache_size(), 0);
    EXPECT_TRUE(listener.set_custom_client_verify_callback(always_verify, SSL_VERIFY_PEER));
    EXPECT_TRUE(listener.set_ocsp_stapling_responder_callback(ocsp_callback, nullptr));
    EXPECT_TRUE(listener.set_sni_selection_callback(sni_callback, nullptr));
    EXPECT_TRUE(listener.set_keylog_callback(keylog_callback));
    EXPECT_TRUE(listener.configure_ecdh_curves("prime256v1"));
    EXPECT_TRUE(listener.enable_post_handshake_auth());
    EXPECT_TRUE(listener.set_supported_alpn_protocols({"h2", "http/1.1"}));
    EXPECT_FALSE(listener.set_supported_alpn_protocols({}));
    EXPECT_FALSE(listener.set_supported_alpn_protocols({std::string(256, 'x')}));
    EXPECT_NE(listener.set_options(SSL_OP_NO_COMPRESSION), 0);
    EXPECT_NE(listener.clear_options(SSL_OP_NO_COMPRESSION), 0);
    EXPECT_GE(listener.set_session_timeout(5), 0);
    EXPECT_TRUE(listener.set_info_callback(info_callback));
    EXPECT_TRUE(listener.set_msg_callback(msg_callback, nullptr));
}

TEST(SSLListener, AcceptWithoutContextFailsCleanlyAfterTcpAccept) {
    {
        qb::io::tcp::ssl::listener listener;
        ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), 0);
        const auto port = listener.local_endpoint().port();
        ASSERT_NE(port, 0);

        std::thread client_thread([port] {
            qb::io::tcp::socket client;
            ASSERT_EQ(client.connect_v4("127.0.0.1", port), 0);
            client.disconnect();
        });

        auto accepted = listener.accept();
        EXPECT_FALSE(accepted.is_open());
        EXPECT_EQ(accepted.ssl_handle(), nullptr);
        client_thread.join();
    }

    {
        qb::io::tcp::ssl::listener listener;
        ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), 0);
        const auto port = listener.local_endpoint().port();
        ASSERT_NE(port, 0);

        std::thread client_thread([port] {
            qb::io::tcp::socket client;
            ASSERT_EQ(client.connect_v4("127.0.0.1", port), 0);
            client.disconnect();
        });

        qb::io::tcp::ssl::socket accepted;
        EXPECT_EQ(listener.accept(accepted), -1);
        EXPECT_FALSE(accepted.is_open());
        EXPECT_EQ(accepted.ssl_handle(), nullptr);
        client_thread.join();
    }
}

TEST(SSLSocket, DefaultStateAndPreHandshakeConfiguration) {
    qb::io::tcp::ssl::socket socket;

    EXPECT_TRUE(socket.verify_peer());
    EXPECT_EQ(socket.ssl_handle(), nullptr);
    EXPECT_FALSE(socket.handshake_complete());
    EXPECT_EQ(socket.handshake_status(), -1);
    EXPECT_EQ(socket.read(nullptr, 0), -1);
    EXPECT_EQ(socket.write("", 0), -1);
    EXPECT_EQ(socket.get_last_ssl_error_string(), "No SSL handle");
    EXPECT_TRUE(socket.get_peer_certificate_details().subject.empty());
    EXPECT_TRUE(socket.get_negotiated_cipher_suite().empty());
    EXPECT_TRUE(socket.get_negotiated_tls_version().empty());
    EXPECT_TRUE(socket.get_alpn_selected_protocol().empty());
    EXPECT_TRUE(socket.get_peer_certificate_chain().empty());
    EXPECT_FALSE(socket.get_session().is_valid());
    EXPECT_FALSE(socket.disable_session_resumption());
    EXPECT_FALSE(socket.request_ocsp_stapling());
    EXPECT_FALSE(socket.set_verify_callback(always_verify, SSL_VERIFY_PEER));
    EXPECT_FALSE(socket.set_verify_depth(2));
    qb::io::ssl::Session invalid_session;
    EXPECT_FALSE(socket.set_session(invalid_session));
    EXPECT_FALSE(socket.request_client_post_handshake_auth());
    EXPECT_FALSE(socket.set_sni_hostname(""));
    EXPECT_TRUE(socket.set_sni_hostname("localhost"));
    EXPECT_FALSE(socket.set_alpn_protocols({}));
    EXPECT_TRUE(socket.set_alpn_protocols({"h2", "http/1.1"}));

    auto *ctx = qb::io::ssl::create_client_context(TLS_client_method());
    ASSERT_NE(ctx, nullptr);
    socket.init(SSL_new(ctx));
    EXPECT_NE(socket.ssl_handle(), nullptr);
    EXPECT_TRUE(socket.disable_session_resumption());
    EXPECT_TRUE(socket.request_ocsp_stapling());
    EXPECT_TRUE(socket.set_verify_callback(always_verify, SSL_VERIFY_PEER));
    EXPECT_TRUE(socket.set_verify_depth(4));
    EXPECT_TRUE(socket.set_sni_hostname("localhost"));
    EXPECT_TRUE(socket.set_alpn_protocols({"h2"}));
    EXPECT_FALSE(socket.set_alpn_protocols({std::string(256, 'x')}));
    socket.set_insecure();
    EXPECT_FALSE(socket.verify_peer());
}

TEST(SSLSocket, ReinitializesHandleAndFailsPreConnectionOperationsCleanly) {
    qb::io::tcp::ssl::socket socket;

    auto *first_ctx = qb::io::ssl::create_client_context(TLS_client_method());
    ASSERT_NE(first_ctx, nullptr);
    auto *first_ssl = SSL_new(first_ctx);
    ASSERT_NE(first_ssl, nullptr);
    socket.init(first_ssl);
    ASSERT_NE(socket.ssl_handle(), nullptr);

    ERR_clear_error();
    EXPECT_EQ(socket.get_last_ssl_error_string(), "No SSL error in queue");
    EXPECT_TRUE(socket.request_ocsp_stapling(false));
    EXPECT_TRUE(socket.disable_session_resumption());
    EXPECT_TRUE(socket.set_verify_callback(always_verify, SSL_VERIFY_PEER));
    EXPECT_TRUE(socket.set_verify_depth(3));
    EXPECT_EQ(socket.connected(), qb::io::SocketStatus::Error);
    EXPECT_EQ(socket.handshake_status(), -1);

    auto *second_ctx = qb::io::ssl::create_client_context(TLS_client_method());
    ASSERT_NE(second_ctx, nullptr);
    auto *second_ssl = SSL_new(second_ctx);
    ASSERT_NE(second_ssl, nullptr);
    socket.init(second_ssl);
    EXPECT_NE(socket.ssl_handle(), nullptr);
    EXPECT_FALSE(socket.handshake_complete());

    qb::io::ssl::Session invalid_session;
    EXPECT_FALSE(socket.set_session(invalid_session));
    socket.set_insecure();
    EXPECT_FALSE(socket.verify_peer());
}

TEST(SSLSocket, LoopbackHandshakeExposesNegotiatedState) {
    const auto files = require_ssl_files();
    if (!ssl_files_available(files)) {
        GTEST_SKIP() << "SSL certificate resources are not available";
    }

    qb::io::tcp::ssl::listener listener;
    listener.init(qb::io::ssl::create_server_context(TLS_server_method(),
                                                     files.cert, files.key));
    ASSERT_NE(listener.ssl_handle(), nullptr);
    ASSERT_TRUE(listener.set_supported_alpn_protocols({"h2", "http/1.1"}));
    ASSERT_EQ(listener.listen_v4(0), 0);
    const auto port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);

    std::atomic<bool> server_ready{false};
    std::atomic<bool> server_ok{false};
    std::thread server_thread([&] {
        qb::io::tcp::ssl::socket server_socket;
        server_ready = true;
        ASSERT_EQ(listener.accept(server_socket), 0);
        drive_server_handshake(server_socket);
        EXPECT_FALSE(server_socket.get_negotiated_cipher_suite().empty());
        EXPECT_FALSE(server_socket.get_negotiated_tls_version().empty());
        EXPECT_EQ(server_socket.get_alpn_selected_protocol(), "h2");
        EXPECT_TRUE(server_socket.get_peer_certificate_details().subject.empty());

        char buffer[64] = {};
        if (!record_thread_failure(read_exactly(server_socket, buffer, 4))) {
            return;
        }
        EXPECT_EQ(std::string_view(buffer, 4), "ping");
        if (!record_thread_failure(write_exactly(server_socket, "pong", 4))) {
            return;
        }
        server_ok = true;
        std::this_thread::sleep_for(50ms);
    });
    thread_join_guard server_join(server_thread, [&] { listener.disconnect(); });

    while (!server_ready.load()) {
        std::this_thread::sleep_for(1ms);
    }

    qb::io::tcp::ssl::socket client;
    client.set_insecure();
    ASSERT_TRUE(client.set_sni_hostname("localhost"));
    ASSERT_TRUE(client.set_alpn_protocols({"h2", "http/1.1"}));
    ASSERT_EQ(client.connect_v4("127.0.0.1", port), 0);
    ASSERT_TRUE(client.handshake_complete());
    EXPECT_FALSE(client.get_negotiated_cipher_suite().empty());
    EXPECT_FALSE(client.get_negotiated_tls_version().empty());
    EXPECT_EQ(client.get_alpn_selected_protocol(), "h2");

    const auto certificate = client.get_peer_certificate_details();
    EXPECT_FALSE(certificate.subject.empty());
    EXPECT_FALSE(certificate.issuer.empty());
    EXPECT_FALSE(certificate.serial_number.empty());
    EXPECT_GT(certificate.not_after, certificate.not_before);
    EXPECT_FALSE(certificate.signature_algorithm.empty());

    const auto chain = client.get_peer_certificate_chain();
    EXPECT_FALSE(chain.empty());

    auto session = client.get_session();
    EXPECT_TRUE(session.is_valid());
    {
        qb::io::tcp::ssl::socket resumption_socket;
        auto *ctx = qb::io::ssl::create_client_context(TLS_client_method());
        ASSERT_NE(ctx, nullptr);
        resumption_socket.init(SSL_new(ctx));
        EXPECT_TRUE(resumption_socket.set_session(session));
    }
    qb::io::ssl::free_session(session);
    EXPECT_FALSE(session.is_valid());

    ASSERT_TRUE(write_exactly(client, "ping", 4));
    char reply[64] = {};
    ASSERT_TRUE(read_exactly(client, reply, 4));
    EXPECT_EQ(std::string_view(reply, 4), "pong");
    EXPECT_FALSE(client.request_client_post_handshake_auth());

    EXPECT_TRUE(server_ok.load());
}

TEST(SSLSocket, BlockingUriAndEndpointTimeoutConnectVariantsReachLoopbackServer) {
    const auto files = require_ssl_files();
    if (!ssl_files_available(files)) {
        GTEST_SKIP() << "SSL certificate resources are not available";
    }

    constexpr int expected_connections = 3;

    qb::io::tcp::ssl::listener listener;
    listener.init(qb::io::ssl::create_server_context(TLS_server_method(),
                                                     files.cert, files.key));
    ASSERT_NE(listener.ssl_handle(), nullptr);
    ASSERT_EQ(listener.listen_v4(0), 0);
    const auto port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);

    std::atomic<bool> server_ready{false};
    std::atomic<int>  server_count{0};
    std::thread server_thread([&] {
        server_ready = true;
        for (int i = 0; i < expected_connections; ++i) {
            qb::io::tcp::ssl::socket server_socket;
            ASSERT_EQ(listener.accept(server_socket), 0);
            drive_server_handshake(server_socket);

            char marker = 0;
            if (!record_thread_failure(read_exactly(server_socket, &marker, sizeof(marker)))) {
                return;
            }
            const char reply = static_cast<char>(marker + 1);
            if (!record_thread_failure(write_exactly(server_socket, &reply, sizeof(reply)))) {
                return;
            }
            ++server_count;
        }
    });
    thread_join_guard server_join(server_thread, [&] { listener.disconnect(); });

    while (!server_ready.load()) {
        std::this_thread::sleep_for(1ms);
    }

    auto exchange_marker = [](qb::io::tcp::ssl::socket &client, char marker) {
        ASSERT_TRUE(client.handshake_complete());
        ASSERT_TRUE(write_exactly(client, &marker, sizeof(marker)));
        char reply = 0;
        ASSERT_TRUE(read_exactly(client, &reply, sizeof(reply)));
        EXPECT_EQ(reply, static_cast<char>(marker + 1));
        client.disconnect();
    };

    qb::io::tcp::ssl::socket uri_client;
    uri_client.set_insecure();
    ASSERT_EQ(uri_client.connect(qb::io::uri("tcp://127.0.0.1:" + std::to_string(port))),
              0);
    exchange_marker(uri_client, 'a');

    qb::io::tcp::ssl::socket timed_uri_client;
    timed_uri_client.set_insecure();
    ASSERT_EQ(timed_uri_client.connect(qb::io::uri("tcp://127.0.0.1:" + std::to_string(port)),
                                       1s),
              0);
    exchange_marker(timed_uri_client, 'b');

    qb::io::tcp::ssl::socket endpoint_timeout_client;
    endpoint_timeout_client.set_insecure();
    ASSERT_EQ(endpoint_timeout_client.connect(qb::io::endpoint("127.0.0.1", port),
                                              "localhost", 1s),
              0);
    exchange_marker(endpoint_timeout_client, 'c');

    EXPECT_EQ(server_count.load(), expected_connections);
}

TEST(SSLSocket, IPv6AndUnixConnectVariantsCompleteHandshake) {
    const auto files = require_ssl_files();
    if (!ssl_files_available(files)) {
        GTEST_SKIP() << "SSL certificate resources are not available";
    }

    {
        qb::io::tcp::ssl::listener listener;
        listener.init(qb::io::ssl::create_server_context(TLS_server_method(),
                                                         files.cert, files.key));
        ASSERT_NE(listener.ssl_handle(), nullptr);
        ASSERT_EQ(listener.listen_v6(0, "::1"), 0);
        const auto port = listener.local_endpoint().port();
        ASSERT_NE(port, 0);

        std::thread server_thread([&] {
            qb::io::tcp::ssl::socket server_socket;
            ASSERT_EQ(listener.accept(server_socket), 0);
            drive_server_handshake(server_socket);
            char marker = 0;
            if (!record_thread_failure(read_exactly(server_socket, &marker, sizeof(marker)))) {
                return;
            }
            EXPECT_EQ(marker, 'v');
            if (!record_thread_failure(write_exactly(server_socket, "6", 1))) {
                return;
            }
        });
        thread_join_guard server_join(server_thread, [&] { listener.disconnect(); });

        qb::io::tcp::ssl::socket client;
        client.set_insecure();
        ASSERT_EQ(client.connect_v6("::1", port), 0);
        ASSERT_TRUE(client.handshake_complete());
        ASSERT_TRUE(write_exactly(client, "v", 1));
        char reply = 0;
        ASSERT_TRUE(read_exactly(client, &reply, sizeof(reply)));
        EXPECT_EQ(reply, '6');
        client.disconnect();
    }

#ifndef _WIN32
    {
        const auto path =
            std::string("/tmp/qb-ssl-socket-uri-") + std::to_string(::getpid()) + ".sock";
        const auto uri = qb::io::uri("unix://" + path);
        std::remove(path.c_str());

        qb::io::tcp::ssl::listener listener;
        listener.init(qb::io::ssl::create_server_context(TLS_server_method(),
                                                         files.cert, files.key));
        ASSERT_NE(listener.ssl_handle(), nullptr);
        ASSERT_EQ(listener.listen(uri), 0);

        std::thread server_thread([&] {
            qb::io::tcp::ssl::socket server_socket;
            ASSERT_EQ(listener.accept(server_socket), 0);
            drive_server_handshake(server_socket);
            char marker = 0;
            if (!record_thread_failure(read_exactly(server_socket, &marker, sizeof(marker)))) {
                return;
            }
            EXPECT_EQ(marker, 'u');
            if (!record_thread_failure(write_exactly(server_socket, "s", 1))) {
                return;
            }
        });
        thread_join_guard server_join(server_thread, [&] { listener.disconnect(); });

        qb::io::tcp::ssl::socket client;
        client.set_insecure();
        ASSERT_EQ(client.connect(uri, 1s), 0);
        ASSERT_TRUE(client.handshake_complete());
        ASSERT_TRUE(write_exactly(client, "u", 1));
        char reply = 0;
        ASSERT_TRUE(read_exactly(client, &reply, sizeof(reply)));
        EXPECT_EQ(reply, 's');
        client.disconnect();
        listener.disconnect();
        std::remove(path.c_str());
    }
#endif
}

TEST(SSLSocket, NonBlockingConnectVariantsPrepareSslState) {
    qb::io::tcp::listener listener;
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    const auto port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);

    qb::io::tcp::listener ipv6_listener;
    ASSERT_EQ(ipv6_listener.listen_v6(0, "::1"), qb::io::SocketStatus::Done);
    const auto ipv6_port = ipv6_listener.local_endpoint().port();
    ASSERT_NE(ipv6_port, 0);

    auto expect_progress = [](int ret, int err) {
        EXPECT_TRUE(ret == 0 || err == EINPROGRESS || qb::io::socket::not_send_error(err))
            << "unexpected n_connect result=" << ret << " errno=" << err;
    };

    qb::io::tcp::ssl::socket endpoint_client;
    endpoint_client.set_insecure();
    ASSERT_TRUE(endpoint_client.set_sni_hostname("localhost"));
    ASSERT_TRUE(endpoint_client.set_alpn_protocols({"h2"}));
    const int endpoint_ret =
        endpoint_client.n_connect(qb::io::endpoint("127.0.0.1", port), "localhost");
    expect_progress(endpoint_ret, qb::io::socket::get_last_errno());
    EXPECT_NE(endpoint_client.ssl_handle(), nullptr);
    EXPECT_FALSE(endpoint_client.handshake_complete());
    endpoint_client.close();

    qb::io::tcp::ssl::socket uri_client;
    uri_client.set_insecure();
    const int uri_ret =
        uri_client.n_connect(qb::io::uri("tcp://127.0.0.1:" + std::to_string(port)));
    expect_progress(uri_ret, qb::io::socket::get_last_errno());
    EXPECT_NE(uri_client.ssl_handle(), nullptr);
    uri_client.close();

    qb::io::tcp::ssl::socket v4_client;
    v4_client.set_insecure();
    const int v4_ret = v4_client.n_connect_v4("127.0.0.1", port);
    expect_progress(v4_ret, qb::io::socket::get_last_errno());
    EXPECT_NE(v4_client.ssl_handle(), nullptr);
    v4_client.close();

    qb::io::tcp::ssl::socket v6_client;
    v6_client.set_insecure();
    const int v6_ret = v6_client.n_connect_v6("::1", ipv6_port);
    expect_progress(v6_ret, qb::io::socket::get_last_errno());
    EXPECT_NE(v6_client.ssl_handle(), nullptr);
    v6_client.close();

#ifndef _WIN32
    const auto path =
        std::string("/tmp/qb-ssl-socket-nconnect-") + std::to_string(::getpid()) + ".sock";
    const auto uri = qb::io::uri("unix://" + path);
    std::remove(path.c_str());

    qb::io::tcp::listener unix_listener;
    ASSERT_EQ(unix_listener.listen_un(path), qb::io::SocketStatus::Done);

    qb::io::tcp::ssl::socket unix_uri_client;
    unix_uri_client.set_insecure();
    const int unix_uri_ret = unix_uri_client.n_connect(uri);
    expect_progress(unix_uri_ret, qb::io::socket::get_last_errno());
    EXPECT_NE(unix_uri_client.ssl_handle(), nullptr);
    unix_uri_client.close();

    qb::io::tcp::ssl::socket unix_direct_client;
    unix_direct_client.set_insecure();
    const int unix_direct_ret = unix_direct_client.n_connect_un(path);
    expect_progress(unix_direct_ret, qb::io::socket::get_last_errno());
    EXPECT_NE(unix_direct_client.ssl_handle(), nullptr);
    unix_direct_client.close();

    unix_listener.disconnect();
    std::remove(path.c_str());
#endif
}
