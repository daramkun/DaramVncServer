#include "rfb/Server.h"

#include <cstring>

#include <ws2tcpip.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include "security/Auth.h"
#include "util/Log.h"

namespace {
constexpr int kMaxClients = 8;
}

namespace {

// Resolves the bind setting to a socket address. "localhost" means IPv4 loopback; an IPv6 literal ("::1", "::" ...)
// selects an IPv6 socket, and "::" (the unspecified address) is opened dual-stack so IPv4 clients can connect too.
bool resolveBind(const std::string& setting, int port, sockaddr_storage& out, int& len, bool& dualStack, std::string* error) {
    const std::string host = setting == "localhost" ? "127.0.0.1" : setting;
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST | AI_PASSIVE;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) {
        *error = "invalid bind address " + setting;
        return false;
    }
    std::memset(&out, 0, sizeof out);
    std::memcpy(&out, res->ai_addr, res->ai_addrlen);
    len = static_cast<int>(res->ai_addrlen);
    dualStack = false;
    if (res->ai_family == AF_INET) {
        reinterpret_cast<sockaddr_in*>(&out)->sin_port = htons(static_cast<u_short>(port));
    } else {
        auto* a6 = reinterpret_cast<sockaddr_in6*>(&out);
        a6->sin6_port = htons(static_cast<u_short>(port));
        dualStack = IN6_IS_ADDR_UNSPECIFIED(&a6->sin6_addr) != 0;
    }
    freeaddrinfo(res);
    return true;
}

// Text form of a peer address; IPv4 clients of a dual-stack socket (::ffff:a.b.c.d) are shown as plain IPv4 so the
// same machine has one name in logs and in the login-failure tracking.
std::string peerName(const sockaddr_storage& ss) {
    char ip[64] = "?";
    if (ss.ss_family == AF_INET) {
        inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in&>(ss).sin_addr, ip, sizeof ip);
    } else if (ss.ss_family == AF_INET6) {
        const auto& a6 = reinterpret_cast<const sockaddr_in6&>(ss);
        if (IN6_IS_ADDR_V4MAPPED(&a6.sin6_addr)) inet_ntop(AF_INET, &a6.sin6_addr.s6_addr[12], ip, sizeof ip);
        else inet_ntop(AF_INET6, &a6.sin6_addr, ip, sizeof ip);
    }
    return ip;
}

}  // namespace

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
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { *error = "WSAStartup failed"; return false; }

    // Check the address first so a typo is reported as a typo, not as a missing password.
    sockaddr_storage addr;
    int addrLen = 0;
    bool dualStack = false;
    if (!resolveBind(cfg_.bind, cfg_.port, addr, addrLen, dualStack, error)) return false;
    if (!cfg_.hasCredentials() && !cfg_.isLoopbackOnly()) {
        *error = "refusing to listen on " + cfg_.bind +
                 " without authentication; run 'daramvnc passwd <user>' first (or pass --bind 127.0.0.1 to test on this machine only)";
        return false;
    }
    if (!initTls(error)) return false;
    if (cfg_.allowLegacyVncAuth) {
        if (cfg_.vncPasswordProtected.empty()) {
            LOGI("RealVNC compatibility requested but no VNC password is set; run 'daramvnc vncpasswd --realvnc'");
        } else {
            LOGI("RealVNC-compatible VNC authentication enabled (security type 2); screen and input traffic are not encrypted on this authentication path");
        }
    }

    listen_ = socket(addr.ss_family, SOCK_STREAM, IPPROTO_TCP);
    if (listen_ == INVALID_SOCKET) {
        *error = "cannot create a listening socket (error " + std::to_string(WSAGetLastError()) + ")";
        return false;
    }
    if (addr.ss_family == AF_INET6) {
        DWORD v6only = dualStack ? 0 : 1;  // "::" also takes IPv4 clients; a specific IPv6 address stays IPv6-only
        setsockopt(listen_, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&v6only), sizeof v6only);
    }
    // Exclusive: don't let another process share the port.
    BOOL excl = TRUE;
    setsockopt(listen_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof excl);
    if (bind(listen_, reinterpret_cast<sockaddr*>(&addr), addrLen) != 0 || listen(listen_, 16) != 0) {
        *error = "bind/listen failed on port " + std::to_string(cfg_.port) + " (error " + std::to_string(WSAGetLastError()) + ")";
        closesocket(listen_);
        listen_ = INVALID_SOCKET;
        return false;
    }
    sockaddr_storage bound{};
    int len = sizeof bound;
    getsockname(listen_, reinterpret_cast<sockaddr*>(&bound), &len);
    port_ = ntohs(bound.ss_family == AF_INET6 ? reinterpret_cast<sockaddr_in6&>(bound).sin6_port
                                              : reinterpret_cast<sockaddr_in&>(bound).sin_port);

    running_ = true;
    acceptThread_ = std::thread([this] { acceptLoop(); });
    LOGI("listening on %s%s%s:%d%s (%s)", addr.ss_family == AF_INET6 ? "[" : "", cfg_.bind.c_str(), addr.ss_family == AF_INET6 ? "]" : "", port_,
         dualStack ? " (IPv4 and IPv6)" : "", cfg_.hasCredentials() ? "TLS + authentication" : "no authentication, loopback only");
    return true;
}

void Server::acceptLoop() {
    while (running_) {
        sockaddr_storage peerAddr{};
        int plen = sizeof peerAddr;
        SOCKET c = accept(listen_, reinterpret_cast<sockaddr*>(&peerAddr), &plen);
        if (c == INVALID_SOCKET) {
            if (!running_) break;
            continue;
        }
        const std::string ip = peerName(peerAddr);
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (active_ >= kMaxClients) {
                LOGI("rejecting %s: too many clients", ip.c_str());
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
