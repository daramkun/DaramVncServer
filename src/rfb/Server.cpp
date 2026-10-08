#include "rfb/Server.h"

#include <ws2tcpip.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include "security/Auth.h"
#include "util/Log.h"

namespace {
constexpr int kMaxClients = 8;
}

Server::Server(ServerConfig cfg) : cfg_(std::move(cfg)) {}

Server::~Server() { stop(); }

bool Server::initTls(std::string* error) {
    if (cfg_.passwordHash.empty() && cfg_.vncPasswordProtected.empty()) return true;  // no auth: no TLS needed
    if (!auth::ensureCertificate(cfg_.certFile, cfg_.keyFile, error)) return false;
    tls_ = SSL_CTX_new(TLS_server_method());
    if (!tls_) { *error = "SSL_CTX_new failed"; return false; }
    SSL_CTX_set_min_proto_version(tls_, TLS1_2_VERSION);
    SSL_CTX_set_options(tls_, SSL_OP_NO_COMPRESSION | SSL_OP_CIPHER_SERVER_PREFERENCE);
    SSL_CTX_set_mode(tls_, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    if (SSL_CTX_use_certificate_chain_file(tls_, cfg_.certFile.c_str()) != 1 ||
        SSL_CTX_use_PrivateKey_file(tls_, cfg_.keyFile.c_str(), SSL_FILETYPE_PEM) != 1 || SSL_CTX_check_private_key(tls_) != 1) {
        char buf[256];
        ERR_error_string_n(ERR_get_error(), buf, sizeof buf);
        *error = std::string("cannot load TLS certificate/key: ") + buf;
        return false;
    }
    return true;
}

bool Server::start(std::string* error) {
    std::string dummy;
    if (!error) error = &dummy;
    if (!cfg_.hasCredentials() && !cfg_.isLoopbackOnly()) {
        *error = "refusing to listen on " + cfg_.bind +
                 " without authentication; run 'daramvnc passwd <user>' first or bind to 127.0.0.1";
        return false;
    }
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { *error = "WSAStartup failed"; return false; }
    if (!initTls(error)) return false;

    listen_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(cfg_.port));
    if (cfg_.bind == "localhost") addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    else if (inet_pton(AF_INET, cfg_.bind.c_str(), &addr.sin_addr) != 1) { *error = "invalid bind address " + cfg_.bind; return false; }
    // Exclusive: don't let another process share the port.
    BOOL excl = TRUE;
    setsockopt(listen_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof excl);
    if (bind(listen_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || listen(listen_, 16) != 0) {
        *error = "bind/listen failed on port " + std::to_string(cfg_.port) + " (error " + std::to_string(WSAGetLastError()) + ")";
        closesocket(listen_);
        listen_ = INVALID_SOCKET;
        return false;
    }
    sockaddr_in bound{};
    int len = sizeof bound;
    getsockname(listen_, reinterpret_cast<sockaddr*>(&bound), &len);
    port_ = ntohs(bound.sin_port);

    running_ = true;
    acceptThread_ = std::thread([this] { acceptLoop(); });
    LOGI("listening on %s:%d (%s)", cfg_.bind.c_str(), port_, cfg_.hasCredentials() ? "TLS + authentication" : "no authentication, loopback only");
    return true;
}

void Server::acceptLoop() {
    while (running_) {
        sockaddr_in peerAddr{};
        int plen = sizeof peerAddr;
        SOCKET c = accept(listen_, reinterpret_cast<sockaddr*>(&peerAddr), &plen);
        if (c == INVALID_SOCKET) {
            if (!running_) break;
            continue;
        }
        char ip[64] = "?";
        inet_ntop(AF_INET, &peerAddr.sin_addr, ip, sizeof ip);
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (active_ >= kMaxClients) {
                LOGI("rejecting %s: too many clients", ip);
                closesocket(c);
                continue;
            }
            ++active_;
        }
        auto conn = std::make_shared<Connection>(c, ip, cfg_, tls_, hub_);
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (size_t i = conns_.size(); i-- > 0;)  // forget connections that have ended
                if (conns_[i].expired()) conns_.erase(conns_.begin() + static_cast<std::ptrdiff_t>(i));
            conns_.push_back(conn);
        }
        std::thread([this, conn] {
            conn->run();
            std::lock_guard<std::mutex> lk(mu_);
            --active_;
            cv_.notify_all();
        }).detach();
    }
}

void Server::stop() {
    if (!running_.exchange(false)) return;
    if (listen_ != INVALID_SOCKET) {
        closesocket(listen_);
        listen_ = INVALID_SOCKET;
    }
    if (acceptThread_.joinable()) acceptThread_.join();
    std::unique_lock<std::mutex> lk(mu_);
    for (auto& w : conns_)
        if (auto c = w.lock()) c->requestStop();
    cv_.wait_for(lk, std::chrono::seconds(5), [this] { return active_ == 0; });
    if (tls_) {
        SSL_CTX_free(tls_);
        tls_ = nullptr;
    }
}
