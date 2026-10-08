#include "net/Transport.h"

#include <ws2tcpip.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <algorithm>

PlainTransport::PlainTransport(SOCKET s) : sock_(s) {
    BOOL nodelay = TRUE;
    setsockopt(sock_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof nodelay);
    int sndbuf = 256 * 1024;
    setsockopt(sock_, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sndbuf), sizeof sndbuf);
    DWORD sendTimeout = 30000;  // a client that stops reading must not pin our sender thread forever
    setsockopt(sock_, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&sendTimeout), sizeof sendTimeout);
}

PlainTransport::~PlainTransport() {
    if (sock_ != INVALID_SOCKET) closesocket(sock_);
}

void PlainTransport::setReadTimeout(unsigned ms) {
    DWORD t = ms;
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof t);
}

SOCKET PlainTransport::release() {
    SOCKET s = sock_;
    sock_ = INVALID_SOCKET;
    return s;
}

bool PlainTransport::readExact(void* buf, size_t len) {
    auto* p = static_cast<char*>(buf);
    while (len > 0) {
        int n = recv(sock_, p, static_cast<int>(std::min<size_t>(len, 1 << 20)), 0);
        if (n <= 0) return false;
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

bool PlainTransport::writeAll(const void* buf, size_t len) {
    auto* p = static_cast<const char*>(buf);
    while (len > 0) {
        int n = send(sock_, p, static_cast<int>(std::min<size_t>(len, 1 << 20)), 0);
        if (n <= 0) return false;
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

void PlainTransport::shutdown() {
    if (sock_ != INVALID_SOCKET) ::shutdown(sock_, SD_BOTH);
}

// ---------------------------------------------------------------------------------------------------------

std::unique_ptr<TlsTransport> TlsTransport::accept(SOCKET sock, SSL_CTX* ctx, std::string* error) {
    SSL* ssl = SSL_new(ctx);
    if (!ssl) {
        closesocket(sock);
        if (error) *error = "SSL_new failed";
        return nullptr;
    }
    SSL_set_fd(ssl, static_cast<int>(sock));
    // Handshake in blocking mode (bounded by a receive timeout), then switch to non-blocking for the session.
    DWORD timeoutMs = 15000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof timeoutMs);
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof timeoutMs);
    if (SSL_accept(ssl) != 1) {
        if (error) {
            char buf[256];
            ERR_error_string_n(ERR_get_error(), buf, sizeof buf);
            *error = buf;
        }
        SSL_free(ssl);
        closesocket(sock);
        return nullptr;
    }
    timeoutMs = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof timeoutMs);
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof timeoutMs);
    u_long nb = 1;
    ioctlsocket(sock, FIONBIO, &nb);
    return std::unique_ptr<TlsTransport>(new TlsTransport(sock, ssl));
}

TlsTransport::~TlsTransport() {
    closed_ = true;
    if (ssl_) {
        SSL_free(ssl_);  // not a clean TLS shutdown on purpose: peer may already be gone
    }
    if (sock_ != INVALID_SOCKET) closesocket(sock_);
}

bool TlsTransport::waitSocket(bool forWrite) {
    while (!closed_) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sock_, &fds);
        timeval tv{0, 50000};  // wake regularly to notice shutdown()
        int r = select(0, forWrite ? nullptr : &fds, forWrite ? &fds : nullptr, nullptr, &tv);
        if (r > 0) return true;
        if (r < 0) return false;
    }
    return false;
}

bool TlsTransport::readExact(void* buf, size_t len) {
    auto* p = static_cast<char*>(buf);
    while (len > 0) {
        if (closed_) return false;
        int n, err;
        {
            std::lock_guard<std::mutex> lk(sslMutex_);
            n = SSL_read(ssl_, p, static_cast<int>(std::min<size_t>(len, 1 << 20)));
            err = n > 0 ? SSL_ERROR_NONE : SSL_get_error(ssl_, n);
        }
        if (n > 0) {
            p += n;
            len -= static_cast<size_t>(n);
        } else if (err == SSL_ERROR_WANT_READ) {
            if (!waitSocket(false)) return false;
        } else if (err == SSL_ERROR_WANT_WRITE) {
            if (!waitSocket(true)) return false;
        } else {
            return false;
        }
    }
    return true;
}

bool TlsTransport::writeAll(const void* buf, size_t len) {
    auto* p = static_cast<const char*>(buf);
    ULONGLONG lastProgress = GetTickCount64();
    while (len > 0) {
        if (closed_) return false;
        if (GetTickCount64() - lastProgress > 30000) return false;  // peer stopped reading
        int n, err;
        {
            std::lock_guard<std::mutex> lk(sslMutex_);
            n = SSL_write(ssl_, p, static_cast<int>(std::min<size_t>(len, 1 << 16)));
            err = n > 0 ? SSL_ERROR_NONE : SSL_get_error(ssl_, n);
        }
        if (n > 0) {
            p += n;
            len -= static_cast<size_t>(n);
            lastProgress = GetTickCount64();
        } else if (err == SSL_ERROR_WANT_WRITE) {
            if (!waitSocket(true)) return false;
        } else if (err == SSL_ERROR_WANT_READ) {
            if (!waitSocket(false)) return false;
        } else {
            return false;
        }
    }
    return true;
}

void TlsTransport::shutdown() {
    closed_ = true;
    ::shutdown(sock_, SD_BOTH);
}
