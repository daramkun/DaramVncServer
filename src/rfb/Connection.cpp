#include "rfb/Connection.h"

#include <ws2tcpip.h>

#include <algorithm>
#include <cstring>

#include "capture/GdiCapturer.h"
#include "encode/Encoder.h"
#include "input/Clipboard.h"
#include "security/Auth.h"
#include "util/Desktop.h"
#include "util/Log.h"
#include "util/ThreadCpu.h"

using rfbenc::put16;
using rfbenc::put32;

namespace {

uint16_t get16(const uint8_t* p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }
uint32_t get32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 | p[3];
}

// Per-address failed-login tracking: delays grow with each failure and repeated failures lock the address out.
struct FailureTracker {
    std::mutex m;
    struct Entry { int fails = 0; std::chrono::steady_clock::time_point last; };
    std::map<std::string, Entry> byPeer;

    // Returns false when the peer is currently locked out.
    bool allowed(const std::string& peer) {
        std::lock_guard<std::mutex> lk(m);
        auto it = byPeer.find(peer);
        if (it == byPeer.end()) return true;
        if (std::chrono::steady_clock::now() - it->second.last > std::chrono::minutes(5)) { byPeer.erase(it); return true; }
        return it->second.fails < 6;
    }
    int recordFailure(const std::string& peer) {
        std::lock_guard<std::mutex> lk(m);
        Entry& e = byPeer[peer];
        ++e.fails;
        e.last = std::chrono::steady_clock::now();
        return e.fails;
    }
    void recordSuccess(const std::string& peer) {
        std::lock_guard<std::mutex> lk(m);
        byPeer.erase(peer);
    }
};
FailureTracker g_failures;

std::string hostName() {
    char host[256] = "daramvnc";
    DWORD n = sizeof host;
    GetComputerNameExA(ComputerNameDnsHostname, host, &n);
    return host;
}

}  // namespace

Connection::Connection(SOCKET s, std::string peer, const ServerConfig& cfg, SSL_CTX* tlsCtx, std::shared_ptr<CaptureHub> hub)
    : peer_(std::move(peer)), cfg_(cfg), tlsCtx_(tlsCtx), tp_(std::make_unique<PlainTransport>(s)), hub_(std::move(hub)) {}

Connection::~Connection() {
    stop();
    if (clipListener_) ClipboardMonitor::instance().removeListener(clipListener_);
}

void Connection::signalStop() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (tp_) tp_->shutdown();
}

void Connection::stop() {
    signalStop();
    if (updater_.joinable() && updater_.get_id() != std::this_thread::get_id()) updater_.join();
}

bool Connection::send(const std::vector<uint8_t>& data) {
    std::lock_guard<std::mutex> lk(sendMu_);
    if (!tp_->writeAll(data.data(), data.size())) {
        dead_ = true;
        tp_->shutdown();
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------
// Handshake and security

bool Connection::sendSecurityFailure(const std::string& reason) {
    std::vector<uint8_t> m;
    put32(m, 1);
    put32(m, static_cast<uint32_t>(reason.size()));
    m.insert(m.end(), reason.begin(), reason.end());
    tp_->writeAll(m.data(), m.size());
    return false;
}

bool Connection::negotiate() {
    if (!tp_->writeAll("RFB 003.008\n", 12)) return false;
    char ver[12];
    if (!tp_->readExact(ver, 12)) return false;
    if (std::strncmp(ver, "RFB 003.", 8) != 0 || ver[11] != '\n') return false;
    const int minor = std::atoi(std::string(ver + 8, 3).c_str());
    if (minor < 7) {  // 3.3 clients cannot choose a security type; not supported
        const uint8_t none[] = {0, 0, 0, 0, 0, 0, 0, 0};
        tp_->writeAll(none, 4);
        return false;
    }

    std::vector<uint8_t> types;
    if (!cfg_.hasCredentials()) {
        types.push_back(1);  // None: only reachable when bound to loopback (enforced by Server)
    } else {
        // VeNCrypt (TLS) carries both the user/password login and the VNC-password variant.
        if (tlsCtx_ && (!cfg_.passwordHash.empty() || !cfg_.vncPasswordProtected.empty())) types.push_back(19);
        if (!cfg_.vncPasswordProtected.empty() && cfg_.allowLegacyVncAuth) types.push_back(2);
    }
    if (types.empty()) return false;
    std::vector<uint8_t> offer{static_cast<uint8_t>(types.size())};
    offer.insert(offer.end(), types.begin(), types.end());
    if (!tp_->writeAll(offer.data(), offer.size())) return false;

    uint8_t choice;
    if (!tp_->readExact(&choice, 1)) return false;
    if (std::find(types.begin(), types.end(), choice) == types.end()) return false;
    if (!g_failures.allowed(peer_)) return sendSecurityFailure("Too many failed attempts");

    switch (choice) {
        case 1: return securityNone();
        case 2: return securityVncAuth();
        case 19: return securityVeNCrypt();
    }
    return false;
}

bool Connection::securityNone() {
    const uint8_t ok[4] = {0, 0, 0, 0};
    return tp_->writeAll(ok, 4);
}

bool Connection::securityVncAuth() {
    uint8_t challenge[16], response[16], expected[16];
    auth::randomBytes(challenge, sizeof challenge);
    if (!tp_->writeAll(challenge, 16) || !tp_->readExact(response, 16)) return false;
    std::string secret;
    bool ok = auth::unprotectSecret(cfg_.vncPasswordProtected, secret) && auth::vncAuthResponse(secret, challenge, expected) &&
              auth::constantTimeEqual(response, expected, 16);
    SecureZeroMemory(secret.data(), secret.size());
    if (!ok) {
        int fails = g_failures.recordFailure(peer_);
        Sleep(std::min(5000, 250 * fails * fails));
        LOGI("%s: VNC authentication failed (%d)", peer_.c_str(), fails);
        return sendSecurityFailure("Authentication failed");
    }
    g_failures.recordSuccess(peer_);
    return securityNone();
}

bool Connection::verifyPlain(const std::string& user, const std::string& pass) {
    // Always run the hash so the timing does not reveal whether the user name matched.
    bool userOk = !cfg_.user.empty() && user.size() == cfg_.user.size() &&
                  auth::constantTimeEqual(user.data(), cfg_.user.data(), user.size());
    bool passOk = auth::verifyPassword(pass, cfg_.passwordHash);
    return userOk && passOk;
}

bool Connection::securityVeNCrypt() {
    const uint8_t version[2] = {0, 2};
    if (!tp_->writeAll(version, 2)) return false;
    uint8_t cv[2];
    if (!tp_->readExact(cv, 2)) return false;
    if (cv[0] != 0 || cv[1] < 2) {  // only 0.2
        const uint8_t nack = 1;
        tp_->writeAll(&nack, 1);
        return false;
    }
    const uint8_t ack = 0;
    if (!tp_->writeAll(&ack, 1)) return false;

    constexpr uint32_t kX509Vnc = 261, kX509Plain = 262;
    std::vector<uint32_t> subs;
    if (!cfg_.passwordHash.empty()) subs.push_back(kX509Plain);
    if (!cfg_.vncPasswordProtected.empty()) subs.push_back(kX509Vnc);
    std::vector<uint8_t> list{static_cast<uint8_t>(subs.size())};
    for (uint32_t s : subs) put32(list, s);
    if (!tp_->writeAll(list.data(), list.size())) return false;

    uint8_t sel[4];
    if (!tp_->readExact(sel, 4)) return false;
    const uint32_t sub = get32(sel);
    if (std::find(subs.begin(), subs.end(), sub) == subs.end()) return false;
    const uint8_t go = 1;
    if (!tp_->writeAll(&go, 1)) return false;

    // Upgrade to TLS: hand the socket to the TLS transport.
    auto* plain = dynamic_cast<PlainTransport*>(tp_.get());
    if (!plain) return false;
    std::string err;
    auto tls = TlsTransport::accept(plain->release(), tlsCtx_, &err);
    if (!tls) {
        LOGI("%s: TLS handshake failed: %s", peer_.c_str(), err.c_str());
        return false;
    }
    tp_ = std::move(tls);

    bool ok = false;
    if (sub == kX509Plain) {
        uint8_t lens[8];
        if (!tp_->readExact(lens, 8)) return false;
        const uint32_t ul = get32(lens), pl = get32(lens + 4);
        if (ul > 256 || pl > 1024) return false;
        std::string user(ul, '\0'), pass(pl, '\0');
        if ((ul && !tp_->readExact(user.data(), ul)) || (pl && !tp_->readExact(pass.data(), pl))) return false;
        ok = verifyPlain(user, pass);
        SecureZeroMemory(pass.data(), pass.size());
        if (!ok) LOGI("%s: login failed for user '%s'", peer_.c_str(), user.c_str());
    } else {
        uint8_t challenge[16], response[16], expected[16];
        auth::randomBytes(challenge, sizeof challenge);
        if (!tp_->writeAll(challenge, 16) || !tp_->readExact(response, 16)) return false;
        std::string secret;
        ok = auth::unprotectSecret(cfg_.vncPasswordProtected, secret) && auth::vncAuthResponse(secret, challenge, expected) &&
             auth::constantTimeEqual(response, expected, 16);
        SecureZeroMemory(secret.data(), secret.size());
    }
    if (!ok) {
        int fails = g_failures.recordFailure(peer_);
        Sleep(std::min(5000, 250 * fails * fails));
        return sendSecurityFailure("Authentication failed");
    }
    g_failures.recordSuccess(peer_);
    return securityNone();
}

bool Connection::serverInit() {
    uint8_t shared;
    if (!tp_->readExact(&shared, 1)) return false;
    std::vector<uint8_t> init;
    const CaptureHub::Layout layout = hub_->layout();
    put16(init, static_cast<uint16_t>(layout.w));
    put16(init, static_cast<uint16_t>(layout.h));
    init.insert(init.end(), {32, 24, 0, 1});
    put16(init, 255); put16(init, 255); put16(init, 255);
    init.insert(init.end(), {16, 8, 0, 0, 0, 0});
    const std::string name = hostName();
    put32(init, static_cast<uint32_t>(name.size()));
    init.insert(init.end(), name.begin(), name.end());
    return send(init);
}

// ---------------------------------------------------------------------------------------------------------
// Client -> server messages

bool Connection::handleSetEncodings() {
    uint8_t hdr[3];
    if (!tp_->readExact(hdr, 3)) return false;
    const uint16_t n = get16(hdr + 1);
    std::vector<uint8_t> raw(static_cast<size_t>(n) * 4);
    if (n && !tp_->readExact(raw.data(), raw.size())) return false;

    Caps c;
    for (uint16_t i = 0; i < n; ++i) {
        const int32_t e = static_cast<int32_t>(get32(&raw[static_cast<size_t>(i) * 4]));
        if (e == rfb::kEncTight) c.tight = true;
        else if (e == rfb::kEncZrle) c.zrle = true;
        else if (e == rfb::kEncCopyRect) c.copyRect = true;
        else if (e == rfb::kEncCursor) c.cursor = true;
        else if (e == rfb::kEncDesktopSize) c.desktopSize = true;
        else if (e == rfb::kEncExtDesktopSize) c.extDesktopSize = true;
        else if (e == rfb::kEncFence) c.fence = true;
        else if (e == rfb::kEncContinuousUpdates) c.continuous = true;
        else if (e == rfb::kEncExtClipboard) c.extClipboard = true;
        else if (e == rfb::kEncQemuKey) c.qemuKey = true;
        else if (e == rfb::kEncH264) c.h264 = true;
        else if (e >= rfb::kEncQualityFirst && e <= rfb::kEncQualityLast) c.quality = e - rfb::kEncQualityFirst;
    }

    bool announceFence = false, announceCont = false, announceClip = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        caps_ = c;
        cursorDirty_ = true;
        announceFence = c.fence && !announcedFence_;
        announceCont = c.continuous && !announcedContinuous_;
        announcedFence_ |= c.fence;
        announcedContinuous_ |= c.continuous;
        announceClip = c.extClipboard;
        if (c.qemuKey && !qemuKeyAnnounced_) { announceQemuKey_ = true; qemuKeyAnnounced_ = true; }
    }
    cv_.notify_all();

    if (announceFence) {  // tell the client the Fence extension is supported
        std::vector<uint8_t> m{rfb::kMsgFence, 0, 0, 0};
        put32(m, 0x80000001u);  // request | BlockBefore
        m.push_back(4);
        put32(m, 0);  // id 0 = handshake, ignored when echoed
        if (!send(m)) return false;
    }
    if (announceCont) {
        if (!send({rfb::kMsgEndOfContinuousUpdates})) return false;
    }
    {
        std::lock_guard<std::mutex> lk(clipMu_);
        clipExtended_ = c.extClipboard;
    }
    if (announceClip) sendClipboardCaps();
    return true;
}

void Connection::noteFenceResponse(uint32_t id) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = fences_.find(id);
    if (it == fences_.end()) return;
    const auto now = std::chrono::steady_clock::now();
    const FenceInfo f = it->second;
    fences_.erase(it);

    // A fence only answers after every earlier byte was consumed, so response timing reveals the link:
    //  - pipelined (sent while an earlier update was still unanswered): the gap between consecutive answers is
    //    the pure transfer time of this update;
    //  - sent into an idle pipe: transfer time = round trip minus the base latency;
    //  - small updates sent into an idle pipe measure the base latency itself.
    const bool pipelined = lastFenceResponse_ != std::chrono::steady_clock::time_point{} && f.sent < lastFenceResponse_;
    const double rtt = std::chrono::duration<double, std::milli>(now - f.sent).count();
    if (f.bytes <= 4096) {
        if (!pipelined) minRttMs_ = std::min(minRttMs_, rtt);
    } else {
        double transferMs;
        if (pipelined) transferMs = std::chrono::duration<double, std::milli>(now - lastFenceResponse_).count();
        else transferMs = rtt - (minRttMs_ < 1e8 ? minRttMs_ : 0);
        const double bw = static_cast<double>(f.bytes) / (std::max(transferMs, 1.0) / 1000.0);
        bandwidth_ = bandwidth_ == 0 ? bw : bandwidth_ * 0.6 + bw * 0.4;
    }
    lastFenceResponse_ = now;
    cv_.notify_all();
}

bool Connection::handleSetDesktopSize() {
    uint8_t hdr[7];
    if (!tp_->readExact(hdr, 7)) return false;
    const int w = get16(hdr + 1), h = get16(hdr + 3);
    const int screens = hdr[5];
    std::vector<uint8_t> skip(static_cast<size_t>(screens) * 16);
    if (!skip.empty() && !tp_->readExact(skip.data(), skip.size())) return false;
    Caps c;
    {
        std::lock_guard<std::mutex> lk(mu_);
        c = caps_;
    }
    if (!c.extDesktopSize) return true;
    if (cfg_.viewOnly) return sendDesktopSizeMsg(c, hub_->layout(), 1, 1);
    const int code = hub_->requestResize(*sub_, w, h);
    const CaptureHub::Layout layout = hub_->layout();
    // On success the updater announces the new size (reason "client requested") once capture follows the mode
    // change; answer here only when nothing will change or the request failed.
    if (code != 0 || (layout.w == w && layout.h == h)) return sendDesktopSizeMsg(c, layout, 1, code);
    return true;
}

bool Connection::messageLoop() {
    for (;;) {
        uint8_t type;
        if (!tp_->readExact(&type, 1)) return false;
        uint8_t b[32];
        switch (type) {
            case 0: {  // SetPixelFormat
                if (!tp_->readExact(b, 19)) return false;
                const uint8_t* p = b + 3;
                PixelFormat f;
                f.bpp = p[0]; f.depth = p[1]; f.bigEndian = p[2]; f.trueColor = p[3];
                f.rMax = get16(p + 4); f.gMax = get16(p + 6); f.bMax = get16(p + 8);
                f.rShift = p[10]; f.gShift = p[11]; f.bShift = p[12];
                if (!f.valid()) {
                    LOGI("%s: unsupported or invalid pixel format", peer_.c_str());
                    return false;
                }
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    pf_ = f;
                    pfGeneration_++;
                    forceFull_ = true;
                    cursorDirty_ = true;
                }
                cv_.notify_all();
                break;
            }
            case 2:
                if (!handleSetEncodings()) return false;
                break;
            case 3: {  // FramebufferUpdateRequest (the region is ignored; we always send everything that changed)
                if (!tp_->readExact(b, 9)) return false;
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    const bool wasPending = requestPending_;
                    requestPending_ = true;
                    interest_ = {get16(b + 1), get16(b + 3), get16(b + 5), get16(b + 7)};
                    if (b[0] == 0) {  // a non-incremental request wins over any pending incremental one
                        requestIncremental_ = false;
                        forceFull_ = true;
                    } else if (!wasPending) {
                        requestIncremental_ = true;
                    }
                }
                cv_.notify_all();
                break;
            }
            case 4:  // KeyEvent
                if (!tp_->readExact(b, 7)) return false;
                if (!cfg_.viewOnly) input_->key(b[0] != 0, get32(b + 3));
                break;
            case 5:  // PointerEvent
                if (!tp_->readExact(b, 5)) return false;
                if (!cfg_.viewOnly) input_->pointer(b[0], get16(b + 1), get16(b + 3));
                cv_.notify_all();  // prompt a capture soon after input
                break;
            case 6:
                if (!handleClientCutText()) return false;
                break;
            case rfb::kMsgEndOfContinuousUpdates: {  // client->server number 150 is EnableContinuousUpdates
                if (!tp_->readExact(b, 9)) return false;
                const bool enable = b[0] != 0;
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    continuous_ = enable;
                    continuousRect_ = {get16(b + 1), get16(b + 3), get16(b + 5), get16(b + 7)};
                }
                cv_.notify_all();
                if (!enable && !send({rfb::kMsgEndOfContinuousUpdates})) return false;
                break;
            }
            case rfb::kMsgFence: {
                uint8_t h[8];
                if (!tp_->readExact(h, 8)) return false;
                const uint32_t flags = get32(h + 3);
                const uint8_t len = h[7];
                uint8_t payload[64] = {0};
                if (len > 64) return false;
                if (len && !tp_->readExact(payload, len)) return false;
                if (flags & 0x80000000u) {  // request: echo with the request bit cleared
                    std::vector<uint8_t> m{rfb::kMsgFence, 0, 0, 0};
                    put32(m, flags & 0x7u);  // we understand BlockBefore/BlockAfter/SyncNext only
                    m.push_back(len);
                    m.insert(m.end(), payload, payload + len);
                    if (!send(m)) return false;
                } else if (len == 4) {
                    noteFenceResponse(get32(payload));
                }
                break;
            }
            case 251:
                if (!handleSetDesktopSize()) return false;
                break;
            case 255: {  // QEMU client message: only the extended key event (subtype 0) is advertised
                if (!tp_->readExact(b, 1) || b[0] != 0) return false;
                if (!tp_->readExact(b, 10)) return false;
                if (!cfg_.viewOnly) input_->keyExt(get16(b) != 0, get32(b + 2), get32(b + 6));
                break;
            }
            default:
                LOGI("%s: unknown client message %u", peer_.c_str(), type);
                return false;
        }
    }
}

void Connection::run() {
    threadcpu::Scope cpuScope("reader");
    desktop::attachToInputDesktop();
    tp_->setReadTimeout(20000);  // don't let an unauthenticated peer hold a thread forever
    if (!negotiate()) {
        LOGI("%s: handshake/authentication failed", peer_.c_str());
        return;
    }
    tp_->setReadTimeout(0);
    sub_ = hub_->subscribe();
    if (!sub_) {
        LOGI("%s: screen capture unavailable", peer_.c_str());
        return;
    }
    const CaptureHub::Layout layout = hub_->layout();
    input_ = std::make_unique<InputInjector>(layout.w, layout.h);
    if (!serverInit()) {
        hub_->unsubscribe(sub_);
        return;
    }
    LOGI("%s: connected (%dx%d)", peer_.c_str(), layout.w, layout.h);

    // Clipboard changes on the server side are pushed to this client; the weak_ptr keeps callbacks safe.
    std::weak_ptr<Connection> weak = shared_from_this();
    clipListener_ = ClipboardMonitor::instance().addListener([weak] {
        if (auto self = weak.lock()) self->onLocalClipboardChanged();
    });

    updater_ = std::thread([this] { updaterMain(); });
    messageLoop();
    stop();
    hub_->unsubscribe(sub_);
    input_->releaseAll();
    LOGI("%s: disconnected", peer_.c_str());
}
