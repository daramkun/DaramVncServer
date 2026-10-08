#pragma once
#include <winsock2.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "config/Config.h"
#include "capture/CaptureHub.h"
#include "rfb/Connection.h"

// Listener: accepts clients and runs a Connection per client on its own thread.
class Server {
public:
    explicit Server(ServerConfig cfg);
    ~Server();

    // Binds and starts accepting. Refuses to listen on a non-loopback address without credentials.
    bool start(std::string* error);
    void stop();
    int port() const { return port_; }

private:
    void acceptLoop();
    bool initTls(std::string* error);

    ServerConfig cfg_;
    SSL_CTX* tls_ = nullptr;
    std::shared_ptr<CaptureHub> hub_ = std::make_shared<CaptureHub>();
    SOCKET listen_ = INVALID_SOCKET;
    int port_ = 0;
    std::atomic<bool> running_{false};
    std::thread acceptThread_;

    std::mutex mu_;
    std::condition_variable cv_;
    std::vector<std::weak_ptr<Connection>> conns_;
    int active_ = 0;
};
