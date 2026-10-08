#pragma once
#include <winsock2.h>

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>

typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_st SSL;

// Byte stream used by the RFB layer. Reads and writes may be issued from different threads concurrently.
class Transport {
public:
    virtual ~Transport() = default;
    virtual bool readExact(void* buf, size_t len) = 0;  // blocks; false on EOF/error/shutdown
    virtual bool writeAll(const void* buf, size_t len) = 0;
    virtual void shutdown() = 0;                         // unblocks pending reads, idempotent
    virtual void setReadTimeout(unsigned ms) { (void)ms; }  // 0 = none; used against stalled pre-auth peers
};

// Plain TCP with TCP_NODELAY and a modest send buffer so queued data cannot add much latency.
class PlainTransport : public Transport {
public:
    explicit PlainTransport(SOCKET s);
    ~PlainTransport() override;
    bool readExact(void* buf, size_t len) override;
    bool writeAll(const void* buf, size_t len) override;
    void shutdown() override;
    void setReadTimeout(unsigned ms) override;
    SOCKET socket() const { return sock_; }
    // Hands the socket over (e.g. to a TLS transport); this object no longer closes it.
    SOCKET release();

private:
    SOCKET sock_;
};

// OpenSSL over a non-blocking socket. SSL objects are not thread-safe, so every SSL_* call is serialized by a
// mutex and all waiting for socket readiness happens outside of it.
class TlsTransport : public Transport {
public:
    // Takes ownership of `sock`. Returns nullptr (and closes the socket) when the handshake fails.
    static std::unique_ptr<TlsTransport> accept(SOCKET sock, SSL_CTX* ctx, std::string* error);
    ~TlsTransport() override;
    bool readExact(void* buf, size_t len) override;
    bool writeAll(const void* buf, size_t len) override;
    void shutdown() override;

private:
    TlsTransport(SOCKET s, SSL* ssl) : sock_(s), ssl_(ssl) {}
    bool waitSocket(bool forWrite);

    SOCKET sock_;
    SSL* ssl_;
    std::mutex sslMutex_;
    std::atomic<bool> closed_{false};
    std::atomic<unsigned long long> waits_{0}, idleWakeups_{0};  // select() calls / calls that timed out with nothing to do
    unsigned long long createdTick_ = 0;
};
