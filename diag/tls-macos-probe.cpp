// SCRATCH (Huly QB-1270 part 3), never to merge: what the kernel and OpenSSL do at the points where qb's close_notify
// witness went red on macOS with OpenSSL 4.1. OpenSSL and POSIX only -- no qb -- so that one build per OpenSSL on one
// machine separates what the system does from what the library does. One line per measurement, "PROBE <name>: ...".
//
//   tls-macos-probe <cert.pem> <key.pem>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <utility>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/opensslv.h>
#include <openssl/ssl.h>

using namespace std::chrono_literals;
using steady = std::chrono::steady_clock;

namespace {

const char *g_cert = nullptr;
const char *g_key  = nullptr;

void
nonblocking(int fd) {
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
}

// a, connected to b, both non-blocking, on 127.0.0.1.
bool
tcp_pair(int &a, int &b) {
    const int   l = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len        = sizeof(addr);
    if (::bind(l, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 || ::listen(l, 4) != 0
        || ::getsockname(l, reinterpret_cast<sockaddr *>(&addr), &len) != 0)
        return false;
    a = ::socket(AF_INET, SOCK_STREAM, 0);
    if (::connect(a, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0)
        return false;
    b = ::accept(l, nullptr, nullptr);
    ::close(l);
#ifdef SO_NOSIGPIPE
    int one = 1;
    ::setsockopt(a, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    ::setsockopt(b, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    nonblocking(a);
    nonblocking(b);
    return b >= 0;
}

// What the peer reads on `fd` within `budget`: octets, then FIN / a socket error / nothing.
std::string
peer_reads(int fd, std::chrono::milliseconds budget) {
    const auto  deadline = steady::now() + budget;
    std::size_t octets   = 0;
    char        buffer[65536];
    for (;;) {
        const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n > 0) {
            octets += static_cast<std::size_t>(n);
            continue;
        }
        if (n == 0)
            return std::to_string(octets) + " octets, FIN";
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            return std::to_string(octets) + " octets, socket error " + std::to_string(errno) + " (" + std::strerror(errno) + ")";
        if (steady::now() >= deadline)
            return std::to_string(octets) + " octets, NO FIN within " + std::to_string(budget.count()) + " ms";
        std::this_thread::sleep_for(1ms);
    }
}

std::string
call(const char *what, int ret, int error) {
    return std::string(what) + " = " + std::to_string(ret)
           + (ret < 0 ? " errno " + std::to_string(error) + " (" + std::strerror(error) + ")" : "");
}

void
line(std::string const &name, std::string const &text) {
    std::printf("PROBE %s: %s\n", name.c_str(), text.c_str());
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------------------------------------------------
// 1. The kernel alone: shutdown() on a connection whose peer already sent its FIN.

void
kernel_shutdown(const char *name, bool peer_fin, bool read_fin, int first, int second) {
    int a = -1, b = -1;
    if (!tcp_pair(a, b)) {
        line(name, "tcp_pair failed");
        return;
    }
    std::string text;
    if (peer_fin) {
        ::shutdown(b, SHUT_WR);
        std::this_thread::sleep_for(50ms);
        if (read_fin) {
            char       c        = 0;
            ssize_t    n        = -1;
            const auto deadline = steady::now() + 2s;
            while ((n = ::recv(a, &c, 1, 0)) < 0 && steady::now() < deadline)
                std::this_thread::sleep_for(1ms);
            text += "a read the peer's FIN: recv = " + std::to_string(n) + "; ";
        } else {
            text += "the peer's FIN arrived, unread; ";
        }
    }
    errno               = 0;
    int         ret     = ::shutdown(a, first);
    int         error   = errno;
    const char *names[] = {"SHUT_RD", "SHUT_WR", "SHUT_RDWR"};
    text += call((std::string("shutdown(") + names[first] + ")").c_str(), ret, error);
    if (second >= 0) {
        errno = 0;
        ret   = ::shutdown(a, second);
        error = errno;
        text += "; " + call((std::string("then shutdown(") + names[second] + ")").c_str(), ret, error);
    }
    text += "; the peer reads: " + peer_reads(b, 2000ms);
    ::close(a);
    text += "; after close(a): " + peer_reads(b, 1000ms);
    ::close(b);
    line(name, text);
}

// ---------------------------------------------------------------------------------------------------------------------
// TLS: a client SSL (the side qb plays) on a, a server SSL (the raw peer) on b.

SSL_CTX *
context(bool server, int max_version) {
    SSL_CTX *ctx = SSL_CTX_new(server ? TLS_server_method() : TLS_client_method());
    if (max_version)
        SSL_CTX_set_max_proto_version(ctx, max_version);
    if (server) {
        if (SSL_CTX_use_certificate_file(ctx, g_cert, SSL_FILETYPE_PEM) != 1
            || SSL_CTX_use_PrivateKey_file(ctx, g_key, SSL_FILETYPE_PEM) != 1) {
            std::printf("PROBE setup: the certificate or key did not load\n");
            ERR_print_errors_fp(stdout);
        }
    }
    return ctx;
}

struct Tls {
    int      a = -1, b = -1;
    SSL_CTX *cctx = nullptr, *sctx = nullptr;
    SSL     *c = nullptr, *s = nullptr;

    ~Tls() {
        SSL_free(c);
        SSL_free(s);
        SSL_CTX_free(cctx);
        SSL_CTX_free(sctx);
        if (a >= 0)
            ::close(a);
        if (b >= 0)
            ::close(b);
    }

    // Connected, both handshakes complete, and one octet from the server read by the client (TLS 1.3: its tickets).
    bool
    open(int max_version) {
        cctx = context(false, max_version);
        sctx = context(true, max_version);
        if (!tcp_pair(a, b))
            return false;
        c = SSL_new(cctx);
        s = SSL_new(sctx);
        SSL_set_fd(c, a);
        SSL_set_fd(s, b);
        SSL_set_connect_state(c);
        SSL_set_accept_state(s);
        const auto deadline = steady::now() + 5s;
        int        rc = 0, rs = 0;
        while ((rc != 1 || rs != 1) && steady::now() < deadline) {
            if (rc != 1)
                rc = SSL_do_handshake(c);
            if (rs != 1)
                rs = SSL_do_handshake(s);
            std::this_thread::sleep_for(1ms);
        }
        if (rc != 1 || rs != 1)
            return false;
        if (SSL_write(s, "x", 1) != 1)
            return false;
        char x = 0;
        while (SSL_read(c, &x, 1) != 1) {
            if (steady::now() >= deadline)
                return false;
            std::this_thread::sleep_for(1ms);
        }
        ERR_clear_error();
        return true;
    }
};

const char *
ssl_error_name(int e) {
    switch (e) {
        case SSL_ERROR_NONE:
            return "NONE";
        case SSL_ERROR_SSL:
            return "SSL";
        case SSL_ERROR_WANT_READ:
            return "WANT_READ";
        case SSL_ERROR_WANT_WRITE:
            return "WANT_WRITE";
        case SSL_ERROR_SYSCALL:
            return "SYSCALL";
        case SSL_ERROR_ZERO_RETURN:
            return "ZERO_RETURN";
        default:
            return "other";
    }
}

std::string
state(SSL *ssl) {
    return "in_init=" + std::to_string(SSL_in_init(ssl)) + " init_finished=" + std::to_string(SSL_is_init_finished(ssl))
           + " shutdown=" + std::to_string(SSL_get_shutdown(ssl)) + " state=\"" + SSL_state_string_long(ssl) + "\"";
}

// The answer of one OpenSSL call: its return, SSL_get_error() (asked first, while the queue is intact), the errno the
// call left, every entry it queued (drained), and the object's state after it.
std::string
answer(const char *what, SSL *ssl, int ret, int error) {
    const int     e     = SSL_get_error(ssl, ret);
    std::string   text  = std::string(what) + " = " + std::to_string(ret) + ", SSL_get_error " + ssl_error_name(e) + " (" + std::to_string(e)
                          + "), errno " + std::to_string(error) + " (" + std::strerror(error) + "), queue [";
    unsigned long code  = 0;
    bool          first = true;
    while ((code = ERR_get_error()) != 0) {
        char reason[256] = {};
        ERR_error_string_n(code, reason, sizeof(reason));
        text += (first ? "" : " | ") + std::string(reason);
        first = false;
    }
    return text + "], " + state(ssl);
}

std::size_t g_attempts = 0;

void
count_writes(SSL *ssl) {
    g_attempts = 0;
    BIO_set_callback_ex(SSL_get_wbio(ssl), [](BIO *, int oper, const char *, std::size_t, int, long, int ret, std::size_t *) -> long {
        if (oper == BIO_CB_WRITE)
            ++g_attempts;
        return ret;
    });
}

void
reset(int fd) {
    linger hard{};
    hard.l_onoff  = 1;
    hard.l_linger = 0;
    ::setsockopt(fd, SOL_SOCKET, SO_LINGER, &hard, sizeof(hard));
    ::close(fd);
}

int
read_until_not_want(SSL *ssl, int &error) {
    char       octet    = 0;
    const auto deadline = steady::now() + 5s;
    for (;;) {
        ERR_clear_error();
        errno         = 0;
        const int ret = SSL_read(ssl, &octet, 1);
        error         = errno;
        if (ret > 0)
            return ret;
        const int e = SSL_get_error(ssl, ret);
        if ((e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) || steady::now() >= deadline)
            return ret;
        std::this_thread::sleep_for(1ms);
    }
}

std::string
version_name(int v) {
    return v == TLS1_2_VERSION ? "1.2" : "1.3";
}

// 2. The peer resets the connection; the client meets it by a read.
void
reset_read(int version) {
    const std::string name = "reset-read-tls" + version_name(version);
    Tls               t;
    if (!t.open(version)) {
        line(name, "setup failed");
        return;
    }
    count_writes(t.c);
    reset(t.b);
    t.b = -1;
    std::this_thread::sleep_for(50ms);
    int error = 0;
    int ret   = read_until_not_want(t.c, error);
    line(name, answer("SSL_read", t.c, ret, error));
    line(name, std::string("SSL_SESSION_is_resumable = ") + std::to_string(SSL_SESSION_is_resumable(SSL_get_session(t.c))));
    ERR_clear_error();
    errno = 0;
    ret   = read_until_not_want(t.c, error);
    line(name, answer("again: SSL_read", t.c, ret, error));
    ERR_clear_error();
    errno = 0;
    ret   = SSL_shutdown(t.c);
    error = errno;
    line(name, answer("SSL_shutdown", t.c, ret, error) + ", write attempts " + std::to_string(g_attempts));
}

// 3. ...met by a write.
void
reset_write(int version) {
    const std::string name = "reset-write-tls" + version_name(version);
    Tls               t;
    if (!t.open(version)) {
        line(name, "setup failed");
        return;
    }
    reset(t.b);
    t.b = -1;
    std::this_thread::sleep_for(50ms);
    const auto deadline = steady::now() + 5s;
    int        ret = 1, error = 0, writes = 0;
    while (ret > 0 && steady::now() < deadline) {
        ERR_clear_error();
        errno = 0;
        ret   = SSL_write(t.c, "LAST-BYTES-GOAWAY", 17);
        error = errno;
        ++writes;
        if (ret > 0)
            std::this_thread::sleep_for(5ms);
    }
    line(name, std::to_string(writes) + " writes; " + answer("SSL_write", t.c, ret, error));
    ERR_clear_error();
    errno = 0;
    ret   = SSL_write(t.c, "LAST-BYTES-GOAWAY", 17);
    error = errno;
    line(name, answer("again: SSL_write", t.c, ret, error));
    count_writes(t.c);
    ERR_clear_error();
    errno = 0;
    ret   = SSL_shutdown(t.c);
    error = errno;
    line(name, answer("SSL_shutdown", t.c, ret, error) + ", write attempts " + std::to_string(g_attempts));
}

// 4./5. One read or one write a filter BIO fails with a system error, no retry flag: what OpenSSL then lets through.
namespace inject {
int fail_write = 0;
int fail_read  = 0;

int
write(BIO *bio, const char *in, int size) {
    BIO_clear_retry_flags(bio);
    if (const int error = std::exchange(fail_write, 0)) {
        errno = error;
        return -1;
    }
    const int ret = BIO_write(BIO_next(bio), in, size);
    BIO_copy_next_retry(bio);
    return ret;
}
int
read(BIO *bio, char *out, int size) {
    BIO_clear_retry_flags(bio);
    if (const int error = std::exchange(fail_read, 0)) {
        errno = error;
        return -1;
    }
    const int ret = BIO_read(BIO_next(bio), out, size);
    BIO_copy_next_retry(bio);
    return ret;
}
long
ctrl(BIO *bio, int cmd, long num, void *ptr) {
    return BIO_ctrl(BIO_next(bio), cmd, num, ptr);
}
int
create(BIO *bio) {
    BIO_set_init(bio, 1);
    return 1;
}
BIO_METHOD *
method() {
    static BIO_METHOD *const m = [] {
        BIO_METHOD *const meth = BIO_meth_new(BIO_get_new_index() | BIO_TYPE_FILTER, "probe-fail-once");
        BIO_meth_set_write(meth, &inject::write);
        BIO_meth_set_read(meth, &inject::read);
        BIO_meth_set_ctrl(meth, &inject::ctrl);
        BIO_meth_set_create(meth, &inject::create);
        return meth;
    }();
    return m;
}
void
install(SSL *ssl, bool writes) {
    BIO *const sock   = writes ? SSL_get_wbio(ssl) : SSL_get_rbio(ssl);
    BIO *const filter = BIO_new(method());
    BIO_up_ref(sock);
    BIO_push(filter, sock);
    if (writes)
        SSL_set0_wbio(ssl, filter);
    else
        SSL_set0_rbio(ssl, filter);
}
} // namespace inject

void
injected_read(int error_to_inject, const char *label) {
    const std::string name = std::string("inject-read-") + label;
    Tls               t;
    if (!t.open(0)) {
        line(name, "setup failed");
        return;
    }
    inject::install(t.c, false);
    SSL_write(t.s, "y", 1);
    std::this_thread::sleep_for(20ms);
    inject::fail_read = error_to_inject;
    char octet        = 0;
    ERR_clear_error();
    errno     = 0;
    int ret   = SSL_read(t.c, &octet, 1);
    int error = errno;
    line(name, answer("SSL_read (the injected error)", t.c, ret, error));
    ret = read_until_not_want(t.c, error);
    line(name, answer("again: SSL_read", t.c, ret, error));
    ERR_clear_error();
    errno = 0;
    ret   = SSL_shutdown(t.c);
    error = errno;
    line(name, answer("SSL_shutdown", t.c, ret, error));
}

void
injected_write(int error_to_inject, const char *label) {
    const std::string name = std::string("inject-write-") + label;
    Tls               t;
    if (!t.open(0)) {
        line(name, "setup failed");
        return;
    }
    inject::install(t.c, true);
    inject::fail_write = error_to_inject;
    ERR_clear_error();
    errno     = 0;
    int ret   = SSL_write(t.c, "LAST-BYTES-GOAWAY", 17);
    int error = errno;
    line(name, answer("SSL_write (the injected error)", t.c, ret, error));
    ERR_clear_error();
    errno = 0;
    ret   = SSL_write(t.c, "LAST-BYTES-GOAWAY", 17);
    error = errno;
    line(name, answer("again: SSL_write", t.c, ret, error));
    ERR_clear_error();
    errno = 0;
    ret   = SSL_shutdown(t.c);
    error = errno;
    line(name, answer("SSL_shutdown", t.c, ret, error));
}

// 6. A send buffer filled to the octet, then the alert.
std::size_t
fill(int fd, int &last_error) {
    static const std::string chunk(std::size_t{64} << 10, 'f');
    std::size_t              sent = 0;
    for (;;) {
        const ssize_t n = ::send(fd, chunk.data(), chunk.size(), 0);
        if (n <= 0)
            break;
        sent += static_cast<std::size_t>(n);
    }
    for (;;) {
        const ssize_t n = ::send(fd, chunk.data(), 1, 0);
        if (n <= 0) {
            last_error = errno;
            break;
        }
        sent += static_cast<std::size_t>(n);
    }
    return sent;
}

int
option(int fd, int level, int name) {
    int       value = -1;
    socklen_t len   = sizeof(value);
    ::getsockopt(fd, level, name, &value, &len);
    return value;
}

std::string
buffers(Tls const &t) {
    std::string text = "a SO_SNDBUF " + std::to_string(option(t.a, SOL_SOCKET, SO_SNDBUF)) + ", b SO_RCVBUF "
                       + std::to_string(option(t.b, SOL_SOCKET, SO_RCVBUF));
#ifdef SO_NWRITE
    text += ", a SO_NWRITE " + std::to_string(option(t.a, SOL_SOCKET, SO_NWRITE));
#endif
#ifdef SO_NREAD
    text += ", b SO_NREAD " + std::to_string(option(t.b, SOL_SOCKET, SO_NREAD));
#endif
    return text;
}

// fixed: SO_SNDBUF on a and SO_RCVBUF on b set by hand first (on macOS that turns their autotuning off).
void
full_buffer(const char *label, bool fixed, std::chrono::milliseconds wait) {
    const std::string name = std::string("full-buffer-") + label;
    Tls               t;
    if (!t.open(0)) {
        line(name, "setup failed");
        return;
    }
    if (fixed) {
        int size = 65536;
        ::setsockopt(t.a, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
        ::setsockopt(t.b, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
    }
    line(name, "before the fill: " + buffers(t));
    int               last  = 0;
    const std::size_t taken = fill(t.a, last);
    const auto        after = steady::now();
    line(name, "fill took " + std::to_string(taken) + " octets, the last refusal errno " + std::to_string(last) + " (" + std::strerror(last)
                   + "); " + buffers(t));
    if (wait.count() > 0) {
        // How much room comes back by itself while nobody reads: a one-octet send every millisecond.
        std::size_t room  = 0;
        long        first = -1;
        while (steady::now() - after < wait) {
            if (::send(t.a, "f", 1, 0) == 1) {
                if (first < 0)
                    first = static_cast<long>(std::chrono::duration_cast<std::chrono::microseconds>(steady::now() - after).count());
                ++room;
            } else {
                std::this_thread::sleep_for(1ms);
            }
        }
        line(name, "within " + std::to_string(wait.count()) + " ms of the fill the socket took " + std::to_string(room)
                       + " more octets, the first after " + std::to_string(first) + " us; " + buffers(t));
        return;
    }
    const auto written = BIO_number_written(SSL_get_wbio(t.c));
    count_writes(t.c);
    ERR_clear_error();
    errno            = 0;
    const int  ret   = SSL_shutdown(t.c);
    const int  error = errno;
    const auto us    = std::chrono::duration_cast<std::chrono::microseconds>(steady::now() - after).count();
    line(name, answer("SSL_shutdown", t.c, ret, error) + ", " + std::to_string(us) + " us after the fill, write attempts "
                   + std::to_string(g_attempts) + ", octets the socket took " + std::to_string(BIO_number_written(SSL_get_wbio(t.c)) - written)
                   + "; " + buffers(t));
}

// 7. The TLS case of the red: the peer ends the session (its close_notify, its FIN), the client answers, then shuts down.
void
peer_ended(int how, const char *label) {
    const std::string name = std::string("peer-ended-") + label;
    Tls               t;
    if (!t.open(0)) {
        line(name, "setup failed");
        return;
    }
    SSL_shutdown(t.s);
    ::shutdown(t.b, SHUT_WR);
    int error = 0;
    int ret   = read_until_not_want(t.c, error);
    line(name, answer("SSL_read", t.c, ret, error));
    ERR_clear_error();
    errno = 0;
    ret   = SSL_shutdown(t.c);
    error = errno;
    line(name, answer("SSL_shutdown (the answer)", t.c, ret, error));
    errno            = 0;
    ret              = ::shutdown(t.a, how);
    error            = errno;
    std::string text = call(how == SHUT_RDWR ? "shutdown(SHUT_RDWR)" : "shutdown(SHUT_WR)", ret, error);
    char        buf[64];
    int         got = 0;
    for (const auto deadline = steady::now() + 2s; steady::now() < deadline;) {
        ERR_clear_error();
        got = SSL_read(t.s, buf, sizeof(buf));
        if (got > 0 || SSL_get_error(t.s, got) != SSL_ERROR_WANT_READ)
            break;
        std::this_thread::sleep_for(1ms);
    }
    text += "; the peer's SSL_read = " + std::to_string(got) + " (" + ssl_error_name(SSL_get_error(t.s, got))
            + "); then raw: " + peer_reads(t.b, 2000ms);
    line(name, text);
}

} // namespace

int
main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <cert.pem> <key.pem>\n", argv[0]);
        return 2;
    }
    g_cert = argv[1];
    g_key  = argv[2];
    ::signal(SIGPIPE, SIG_IGN);
    line("library", std::string(OpenSSL_version(OPENSSL_VERSION)) + " (built against " + OPENSSL_VERSION_TEXT + ")");

    kernel_shutdown("kernel-rdwr-no-peer-fin", false, false, SHUT_RDWR, -1);
    kernel_shutdown("kernel-rdwr-after-peer-fin-read", true, true, SHUT_RDWR, -1);
    kernel_shutdown("kernel-rdwr-after-peer-fin-unread", true, false, SHUT_RDWR, -1);
    kernel_shutdown("kernel-wr-after-peer-fin-read", true, true, SHUT_WR, -1);
    kernel_shutdown("kernel-rd-after-peer-fin-read", true, true, SHUT_RD, -1);
    kernel_shutdown("kernel-wr-then-rd-after-peer-fin-read", true, true, SHUT_WR, SHUT_RD);
    kernel_shutdown("kernel-wr-then-rdwr-no-peer-fin", false, false, SHUT_WR, SHUT_RDWR);
    kernel_shutdown("kernel-rdwr-twice-no-peer-fin", false, false, SHUT_RDWR, SHUT_RDWR);

    peer_ended(SHUT_RDWR, "rdwr");
    peer_ended(SHUT_WR, "wr");

    reset_read(0);
    reset_read(TLS1_2_VERSION);
    reset_write(0);
    reset_write(TLS1_2_VERSION);

    injected_read(EINTR, "eintr");
    injected_read(ECONNRESET, "econnreset");
    injected_write(ENOBUFS, "enobufs");
    injected_write(EINTR, "eintr");
    injected_write(ECONNRESET, "econnreset");

    for (int i = 0; i < 3; ++i) {
        full_buffer("autotuned-alert", false, 0ms);
        full_buffer("fixed-alert", true, 0ms);
    }
    full_buffer("autotuned-room-200ms", false, 200ms);
    full_buffer("fixed-room-200ms", true, 200ms);
    return 0;
}
