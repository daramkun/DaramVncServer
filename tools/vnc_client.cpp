// Minimal RFB client for integration tests and benchmarks. Decodes with the reference decoders, so it checks the
// server's output independently of the server's own encoder code.
//
//   daramvnc_client [host] [port] [--user U --pass P] [--enc tight|zrle|raw] [--pf native|565]
//                   [--seconds N] [--continuous] [--verify] [--no-cursor] [--quality 0-9]
#include <winsock2.h>
#include <mmsystem.h>
#include <ws2tcpip.h>

#include <openssl/ssl.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <array>
#include <deque>
#include <map>
#include <string>

#include "RefDecoders.h"
#include "RefH264Decoder.h"
#include "capture/GdiCapturer.h"
#include "security/Auth.h"

#pragma comment(lib, "winmm.lib")

using namespace std::chrono;
using ref::Image;

namespace {

struct Io {
    SOCKET s = INVALID_SOCKET;
    SSL* ssl = nullptr;
    size_t rx = 0, tx = 0;
    double throttleBps = 0;  // simulate a slow link by pacing reads against a virtual clock
    double throttleUntilSec = 0;  // >0: the slow link recovers after this many seconds
    std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point virtualClock = std::chrono::steady_clock::now();
    void readExact(void* buf, size_t n) {
        readRaw(buf, n);
        if (throttleBps > 0 && throttleUntilSec > 0 &&
            std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count() > throttleUntilSec)
            throttleBps = 0;
        if (throttleBps > 0) {
            const auto now = std::chrono::steady_clock::now();
            if (virtualClock < now) virtualClock = now;
            virtualClock += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(static_cast<double>(n) / throttleBps));
            const auto ahead = virtualClock - now;
            if (ahead > std::chrono::milliseconds(20)) Sleep(static_cast<DWORD>(std::chrono::duration_cast<std::chrono::milliseconds>(ahead).count()) - 10);
        }
    }
    void readRaw(void* buf, size_t n) {
        auto* p = static_cast<char*>(buf);
        size_t left = n;
        while (left) {
            int r = ssl ? SSL_read(ssl, p, static_cast<int>(left)) : recv(s, p, static_cast<int>(left), 0);
            if (r <= 0) throw std::runtime_error("connection closed");
            p += r; left -= static_cast<size_t>(r);
        }
        rx += n;
    }
    void writeAll(const void* buf, size_t n) {
        auto* p = static_cast<const char*>(buf);
        size_t left = n;
        while (left) {
            int r = ssl ? SSL_write(ssl, p, static_cast<int>(left)) : send(s, p, static_cast<int>(left), 0);
            if (r <= 0) throw std::runtime_error("write failed");
            p += r; left -= static_cast<size_t>(r);
        }
        tx += n;
    }
    uint8_t u8() { uint8_t v; readExact(&v, 1); return v; }
    uint16_t u16() { uint8_t b[2]; readExact(b, 2); return static_cast<uint16_t>(b[0] << 8 | b[1]); }
    uint32_t u32() { uint8_t b[4]; readExact(b, 4); return static_cast<uint32_t>(b[0]) << 24 | b[1] << 16 | b[2] << 8 | b[3]; }
    // True when data is available within `ms` (accounts for TLS-buffered bytes).
    bool readable(int ms) {
        if (ssl && SSL_pending(ssl) > 0) return true;
        fd_set f; FD_ZERO(&f); FD_SET(s, &f);
        timeval tv{ms / 1000, (ms % 1000) * 1000};
        return select(0, &f, nullptr, nullptr, &tv) > 0;
    }
};

void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(x >> 8); v.push_back(x & 0xFF); }
void put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 3; i >= 0; --i) v.push_back(static_cast<uint8_t>(x >> (i * 8))); }

}  // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1", user, pass, vncPass, enc = "tight", pfName = "native";
    int port = 5900, seconds = 5, quality = 8;
    double throttleKBps = 0, throttleUntil = 0, ackDelayMs = 0;
    bool continuous = false, verify = false, wantCursor = true, qemuKey = false, wantH264 = false;
    int rx = 0, ry = 0, rw = -1, rh = -1, resizeW = 0, resizeH = 0;
    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--user" && i + 1 < argc) user = argv[++i];
        else if (a == "--pass" && i + 1 < argc) pass = argv[++i];
        else if (a == "--vncpass" && i + 1 < argc) vncPass = argv[++i];
        else if (a == "--enc" && i + 1 < argc) enc = argv[++i];
        else if (a == "--pf" && i + 1 < argc) pfName = argv[++i];
        else if (a == "--seconds" && i + 1 < argc) seconds = std::atoi(argv[++i]);
        else if (a == "--quality" && i + 1 < argc) quality = std::atoi(argv[++i]);
        else if (a == "--throttle" && i + 1 < argc) throttleKBps = std::atof(argv[++i]);
        else if (a == "--throttle-until" && i + 1 < argc) throttleUntil = std::atof(argv[++i]);
        else if (a == "--ack-delay" && i + 1 < argc) ackDelayMs = std::atof(argv[++i]);
        else if (a == "--continuous") continuous = true;
        else if (a == "--verify") verify = true;
        else if (a == "--h264") wantH264 = true;
        else if (a == "--qemu-key") qemuKey = true;
        else if (a == "--region" && i + 1 < argc) std::sscanf(argv[++i], "%d,%d,%d,%d", &rx, &ry, &rw, &rh);
        else if (a == "--resize" && i + 1 < argc) std::sscanf(argv[++i], "%dx%d", &resizeW, &resizeH);
        else if (a == "--no-cursor") wantCursor = false;
        else if (positional == 0) { host = a; ++positional; }
        else if (positional == 1) { port = std::atoi(a.c_str()); ++positional; }
    }

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    timeBeginPeriod(1);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);  // GDI reference must see real pixels
    try {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        Io io;
        io.throttleBps = throttleKBps * 1024;
        io.throttleUntilSec = throttleUntil;
        io.s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_port = htons(static_cast<u_short>(port));
        inet_pton(AF_INET, host.c_str(), &sa.sin_addr);
        if (connect(io.s, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0) throw std::runtime_error("connect failed");
        BOOL nd = TRUE;
        setsockopt(io.s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nd), sizeof nd);

        // --- handshake ---
        char ver[12];
        io.readExact(ver, 12);
        io.writeAll("RFB 003.008\n", 12);
        const int nTypes = io.u8();
        if (nTypes == 0) throw std::runtime_error("server refused connection");
        std::vector<uint8_t> types(static_cast<size_t>(nTypes));
        io.readExact(types.data(), types.size());
        const bool useVncPass = !vncPass.empty();
        const bool useTls = !user.empty() || useVncPass;
        const uint8_t want = useTls ? 19 : 1;
        if (std::find(types.begin(), types.end(), want) == types.end()) throw std::runtime_error("server does not offer the wanted security type");
        io.writeAll(&want, 1);
        if (useTls) {
            io.u8(); io.u8();  // server version 0.2
            const uint8_t cv[2] = {0, 2};
            io.writeAll(cv, 2);
            if (io.u8() != 0) throw std::runtime_error("VeNCrypt version rejected");
            const int ns = io.u8();
            const uint32_t wantSub = useVncPass ? 261 : 262;  // X509Vnc : X509Plain
            bool offered = false;
            for (int i = 0; i < ns; ++i) offered |= io.u32() == wantSub;
            if (!offered) throw std::runtime_error("wanted VeNCrypt subtype not offered");
            std::vector<uint8_t> sel; put32(sel, wantSub);
            io.writeAll(sel.data(), 4);
            if (io.u8() != 1) throw std::runtime_error("TLS not accepted");
            SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
            SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);  // test tool: self-signed certificate
            io.ssl = SSL_new(ctx);
            SSL_set_fd(io.ssl, static_cast<int>(io.s));
            if (SSL_connect(io.ssl) != 1) throw std::runtime_error("TLS handshake failed");
            std::printf("TLS: %s, cipher %s\n", SSL_get_version(io.ssl), SSL_get_cipher_name(io.ssl));
            if (useVncPass) {
                uint8_t challenge[16], response[16];
                io.readExact(challenge, 16);
                if (!auth::vncAuthResponse(vncPass, challenge, response)) throw std::runtime_error("DES failed");
                io.writeAll(response, 16);
            } else {
                std::vector<uint8_t> cred;
                put32(cred, static_cast<uint32_t>(user.size())); put32(cred, static_cast<uint32_t>(pass.size()));
                cred.insert(cred.end(), user.begin(), user.end()); cred.insert(cred.end(), pass.begin(), pass.end());
                io.writeAll(cred.data(), cred.size());
            }
        }
        if (io.u32() != 0) {
            uint32_t n = io.u32(); std::string why(n, '\0'); io.readExact(why.data(), n);
            throw std::runtime_error("authentication failed: " + why);
        }
        const uint8_t shared = 1;
        io.writeAll(&shared, 1);
        int W = io.u16(), H = io.u16();
        uint8_t pfb[16];
        io.readExact(pfb, 16);
        std::string name(io.u32(), '\0');
        io.readExact(name.data(), name.size());
        std::printf("connected to '%s' %dx%d\n", name.c_str(), W, H);

        PixelFormat pf;
        if (pfName == "cmap") { pf.bpp = 8; pf.depth = 8; pf.trueColor = 0; pf.rMax = pf.gMax = pf.bMax = 0; pf.rShift = pf.gShift = pf.bShift = 0; }
        if (pfName == "565") { pf.bpp = 16; pf.depth = 16; pf.rMax = 31; pf.gMax = 63; pf.bMax = 31; pf.rShift = 11; pf.gShift = 5; pf.bShift = 0; }
        std::vector<uint8_t> m{0, 0, 0, 0, pf.bpp, pf.depth, pf.bigEndian, pf.trueColor};
        put16(m, pf.rMax); put16(m, pf.gMax); put16(m, pf.bMax);
        m.insert(m.end(), {pf.rShift, pf.gShift, pf.bShift, 0, 0, 0});
        io.writeAll(m.data(), m.size());

        std::vector<int32_t> encs;
        if (enc == "tight") encs = {7, 16, 1, 0};
        else if (enc == "zrle") encs = {16, 1, 0};
        else encs = {0};
        if (wantCursor) encs.push_back(-239);
        if (qemuKey) encs.push_back(-258);
        if (wantH264) encs.insert(encs.begin(), 50);
        encs.insert(encs.end(), {-223, -308, -312, -313, static_cast<int32_t>(0xC0A1E5CE), -32 + quality, -256 + 2});
        m = {2, 0};
        put16(m, static_cast<uint16_t>(encs.size()));
        for (int32_t e : encs) put32(m, static_cast<uint32_t>(e));
        io.writeAll(m.data(), m.size());

        if (rw < 0) { rw = W; rh = H; }
        auto request = [&](bool inc) {
            std::vector<uint8_t> r{3, static_cast<uint8_t>(inc)};
            put16(r, static_cast<uint16_t>(rx)); put16(r, static_cast<uint16_t>(ry)); put16(r, static_cast<uint16_t>(rw)); put16(r, static_cast<uint16_t>(rh));
            io.writeAll(r.data(), r.size());
        };

        Image fb(W, H);
        ref::TightDecoder tight;
        ref::ZrleDecoder zrle;
        std::map<int, int> rectsByEnc;
        size_t updates = 0, firstUpdateBytes = 0;
        double worstMs = 0, sumMs = 0;
        std::deque<std::pair<steady_clock::time_point, std::vector<uint8_t>>> delayedAcks;
        std::map<std::array<int, 4>, std::unique_ptr<ref::H264Decoder>> h264Ctx;
        int h264Frames = 0, h264Shown = 0;
        bool sawQemuAnnounce = false, qemuKeysSent = false, sawResize = false, resizeSent = false, sawContinuousAck = false, sawFence = false, sawCursor = false, sawExtSize = false, continuousOn = false;
        auto t0 = steady_clock::now(), lastReq = t0, lastUpdate = t0;
        bool waiting = false;

        request(false);
        lastReq = steady_clock::now();
        waiting = true;
        while (duration<double>(steady_clock::now() - t0).count() < seconds || (verify && duration<double>(steady_clock::now() - lastUpdate).count() < 0.9)) {
            // Simulated network round trip: fence answers leave the client ackDelayMs after they were due.
            while (!delayedAcks.empty() && delayedAcks.front().first <= steady_clock::now()) {
                io.writeAll(delayedAcks.front().second.data(), delayedAcks.front().second.size());
                delayedAcks.pop_front();
            }
            if (!io.readable(delayedAcks.empty() ? 100 : 2)) {
                if (!continuousOn && !waiting) { request(true); lastReq = steady_clock::now(); waiting = true; }
                continue;
            }
            const uint8_t type = io.u8();
            if (type == 0) {
                io.u8();
                const int n = io.u16();
                for (int i = 0; i < n; ++i) {
                    const int x = io.u16(), y = io.u16(), w = io.u16(), h = io.u16();
                    const int32_t e = static_cast<int32_t>(io.u32());
                    ++rectsByEnc[e];
                    if (e == 0) {
                        std::vector<uint8_t> raw(static_cast<size_t>(w) * h * (pf.bpp / 8));
                        io.readExact(raw.data(), raw.size());
                        ref::Reader r(raw);
                        for (int j = 0; j < h; ++j) for (int k = 0; k < w; ++k) {
                            uint32_t v = 0;
                            for (int b = 0; b < pf.bpp / 8; ++b) v |= static_cast<uint32_t>(r.u8()) << (b * 8);
                            uint8_t* o = fb.at(x + k, y + j);
                            if (!pf.trueColor) { const uint8_t* e = &zrle.palette[(v & 255) * 3]; o[0] = e[0]; o[1] = e[1]; o[2] = e[2]; continue; }
                            o[2] = static_cast<uint8_t>(((v >> pf.rShift) & pf.rMax) * 255 / pf.rMax);
                            o[1] = static_cast<uint8_t>(((v >> pf.gShift) & pf.gMax) * 255 / pf.gMax);
                            o[0] = static_cast<uint8_t>(((v >> pf.bShift) & pf.bMax) * 255 / pf.bMax);
                        }
                    } else if (e == 1) {
                        const int sx = io.u16(), sy = io.u16();
                        Image tmp(w, h);
                        for (int j = 0; j < h; ++j) std::memcpy(tmp.at(0, j), fb.at(sx, sy + j), static_cast<size_t>(w) * 4);
                        for (int j = 0; j < h; ++j) std::memcpy(fb.at(x, y + j), tmp.at(0, j), static_cast<size_t>(w) * 4);
                    } else if (e == 7 || e == 16) {
                        // Pull the whole rectangle payload off the wire, then decode from memory.
                        std::vector<uint8_t> payload;
                        auto grab = [&](size_t k) { size_t o = payload.size(); payload.resize(o + k); io.readExact(&payload[o], k); };
                        if (e == 16) {
                            grab(4);
                            uint32_t len = static_cast<uint32_t>(payload[0]) << 24 | payload[1] << 16 | payload[2] << 8 | payload[3];
                            grab(len);
                            ref::Reader r(payload);
                            zrle.decodeRect(r, x, y, w, h, pf, fb);
                        } else {
                            grab(1);
                            const uint8_t c = payload[0];
                            if ((c & 0xF0) == 0x80) grab(3);
                            else if ((c & 0xF0) == 0x90) { size_t before = payload.size(); grab(1); size_t len = payload[before] & 0x7F; if (payload[before] & 0x80) { grab(1); len |= (payload[before + 1] & 0x7F) << 7; if (payload[before + 1] & 0x80) { grab(1); len |= static_cast<size_t>(payload[before + 2]) << 14; } } grab(len); }
                            else {
                                int filter = 0;
                                if (c & 0x40) { grab(1); filter = payload[1]; }
                                size_t nPal = 0;
                                if (filter == 1) { grab(1); nPal = payload.back() + 1u; grab(nPal * 3); }
                                size_t expected = filter == 1 ? (nPal == 2 ? static_cast<size_t>((w + 7) / 8) * h : static_cast<size_t>(w) * h) : static_cast<size_t>(w) * h * 3;
                                if (expected >= 12) {
                                    size_t before = payload.size(); grab(1); size_t len = payload[before] & 0x7F;
                                    if (payload[before] & 0x80) { grab(1); len |= (payload[before + 1] & 0x7F) << 7; if (payload[before + 1] & 0x80) { grab(1); len |= static_cast<size_t>(payload[before + 2]) << 14; } }
                                    grab(len);
                                } else grab(expected);
                            }
                            ref::Reader r(payload);
                            tight.decodeRect(r, x, y, w, h, fb);
                        }
                    } else if (e == -239) {
                        const size_t bytes = static_cast<size_t>(w) * h * (pf.bpp / 8) + static_cast<size_t>((w + 7) / 8) * h;
                        std::vector<uint8_t> skip(bytes);
                        io.readExact(skip.data(), skip.size());
                        sawCursor = true;
                    } else if (e == -308) {
                        const int ns = io.u8(); io.u8(); io.u16();
                        for (int k = 0; k < ns; ++k) { io.u32(); io.u16(); io.u16(); io.u16(); io.u16(); io.u32(); }
                        sawExtSize = true;
                        std::printf("ExtendedDesktopSize: reason %d status %d size %dx%d\n", x, y, w, h);
                        if (w != W || h != H) { W = w; H = h; fb = Image(W, H); rx = ry = 0; rw = W; rh = H; sawResize = true; }
                    } else if (e == 50) {  // Open H.264: one decoder context per rectangle
                        const uint32_t len = io.u32(), flags = io.u32();
                        std::vector<uint8_t> data(len);
                        if (len) io.readExact(data.data(), len);
                        if (flags & 2) h264Ctx.clear();
                        const std::array<int, 4> key{x, y, w, h};
                        if (flags & 1) h264Ctx.erase(key);
                        auto it = h264Ctx.find(key);
                        if (it == h264Ctx.end()) {
                            auto d = std::make_unique<ref::H264Decoder>();
                            if (!d->open((w + 15) & ~15, (h + 15) & ~15)) throw std::runtime_error("H.264 decoder unavailable");
                            it = h264Ctx.emplace(key, std::move(d)).first;
                        }
                        ++h264Frames;
                        std::vector<uint8_t> bgra;
                        if (len && it->second->decode(data, w, h, bgra)) {
                            for (int j = 0; j < h; ++j) std::memcpy(fb.at(x, y + j), &bgra[static_cast<size_t>(j) * w * 4], static_cast<size_t>(w) * 4);
                            ++h264Shown;
                        }
                    } else if (e == -258) {
                        sawQemuAnnounce = true;
                    } else if (e == -223) {
                        if (w != W || h != H) { W = w; H = h; fb = Image(W, H); rx = ry = 0; rw = W; rh = H; sawResize = true; }
                    } else {
                        throw std::runtime_error("unexpected encoding " + std::to_string(e));
                    }
                }
                ++updates;
                const auto now = steady_clock::now();
                const double ms = duration<double, std::milli>(now - lastReq).count();
                if (waiting) { worstMs = std::max(worstMs, ms); sumMs += ms; }
                if (updates == 1) firstUpdateBytes = io.rx;
                lastUpdate = now;
                if (sawQemuAnnounce && !qemuKeysSent) {  // exercise the extended key event message (ignored by a view-only server)
                    for (int down = 1; down >= 0; --down) {
                        std::vector<uint8_t> r{255, 0};
                        put16(r, static_cast<uint16_t>(down)); put32(r, 0x61); put32(r, 0x1E);
                        io.writeAll(r.data(), r.size());
                    }
                    qemuKeysSent = true;
                }
                if (resizeW > 0 && !resizeSent && updates >= 3) {
                    std::vector<uint8_t> r{251, 0};
                    put16(r, static_cast<uint16_t>(resizeW)); put16(r, static_cast<uint16_t>(resizeH));
                    r.insert(r.end(), {1, 0});
                    put32(r, 1); put16(r, 0); put16(r, 0); put16(r, static_cast<uint16_t>(resizeW)); put16(r, static_cast<uint16_t>(resizeH)); put32(r, 0);
                    io.writeAll(r.data(), r.size());
                    resizeSent = true;
                    std::printf("sent SetDesktopSize %dx%d\n", resizeW, resizeH);
                }
                if (!continuousOn) { request(true); lastReq = now; waiting = true; }
            } else if (type == 248) {  // Fence
                uint8_t h3[3]; io.readExact(h3, 3);
                const uint32_t flags = io.u32();
                const uint8_t len = io.u8();
                std::vector<uint8_t> payload(len);
                if (len) io.readExact(payload.data(), len);
                sawFence = true;
                if (flags & 0x80000000u) {
                    std::vector<uint8_t> r{248, 0, 0, 0};
                    put32(r, flags & 0x7fffffffu);
                    r.push_back(len);
                    r.insert(r.end(), payload.begin(), payload.end());
                    if (ackDelayMs > 0) delayedAcks.emplace_back(steady_clock::now() + milliseconds(static_cast<int>(ackDelayMs)), std::move(r));
                    else io.writeAll(r.data(), r.size());
                }
            } else if (type == 150) {  // EndOfContinuousUpdates
                sawContinuousAck = true;
                if (continuous && !continuousOn) {
                    std::vector<uint8_t> r{150, 1};
                    put16(r, static_cast<uint16_t>(rx)); put16(r, static_cast<uint16_t>(ry)); put16(r, static_cast<uint16_t>(rw)); put16(r, static_cast<uint16_t>(rh));
                    io.writeAll(r.data(), r.size());
                    continuousOn = true;
                }
            } else if (type == 3) {  // ServerCutText
                uint8_t h3[3]; io.readExact(h3, 3);
                int32_t len = static_cast<int32_t>(io.u32());
                size_t n = len < 0 ? static_cast<size_t>(-static_cast<int64_t>(len)) : static_cast<size_t>(len);
                std::vector<uint8_t> skip(n);
                if (n) io.readExact(skip.data(), n);
            } else if (type == 1) {  // SetColourMapEntries
                io.u8();
                const int first = io.u16(), n = io.u16();
                for (int k = 0; k < n; ++k) {
                    const uint8_t r = static_cast<uint8_t>(io.u16() >> 8), g = static_cast<uint8_t>(io.u16() >> 8), b = static_cast<uint8_t>(io.u16() >> 8);
                    if (first + k < 256) { zrle.palette[(first + k) * 3] = b; zrle.palette[(first + k) * 3 + 1] = g; zrle.palette[(first + k) * 3 + 2] = r; }
                }
            } else if (type == 2) {
                // bell
            } else {
                throw std::runtime_error("unexpected server message " + std::to_string(type));
            }
        }

        const double secs = duration<double>(steady_clock::now() - t0).count();
        std::printf("%zu updates in %.1fs (%.1f/s), %.2f MB received (%.1f Mbit/s), first update %.2f MB\n", updates, secs,
                    updates / secs, io.rx / 1048576.0, io.rx * 8 / 1e6 / secs, firstUpdateBytes / 1048576.0);
        if (updates) std::printf("request->update latency: avg %.1f ms, worst %.1f ms\n", sumMs / updates, worstMs);
        std::printf("rects:");
        for (auto& kv : rectsByEnc) std::printf(" enc%d=%d", kv.first, kv.second);
        std::printf("\nextensions seen: fence=%d continuous-ack=%d cursor=%d ext-desktop-size=%d jpeg-rects=%d\n", sawFence,
                    sawContinuousAck, sawCursor, sawExtSize, tight.jpegRects);
        if (wantH264) std::printf("h264: %d frames received, %d decoded and shown (the MF decoder holds one frame back)\n", h264Frames, h264Shown);
        if (qemuKey) std::printf("qemu-ext-key announced=%d, key events sent=%d, connection still alive\n", sawQemuAnnounce, qemuKeysSent);

        if (verify) {
            GdiCapturer gdi;
            FrameUpdate u;
            gdi.grabFull(u);
            if (gdi.width() != W || gdi.height() != H) throw std::runtime_error("GDI screenshot size differs from the remote framebuffer");
            size_t diff = 0, total = static_cast<size_t>(rw) * rh, big = 0;
            int firstX = -1, firstY = -1;
            int minX = W, minY = H, maxX = -1, maxY = -1;
            for (int y = ry; y < ry + rh; ++y) for (int x = rx; x < rx + rw; ++x) {
                const uint8_t* a = fb.at(x, y);
                const uint8_t* b = gdi.pixels() + (static_cast<size_t>(y) * W + x) * 4;
                uint8_t quant[3];
                if (pf.colorMapped()) {  // the server could only send the nearest palette colour
                    PixelFormat::rgb332Entry(PixelFormat::rgb332Index(b[2], b[1], b[0]), quant[2], quant[1], quant[0]);
                    b = quant;
                }
                if (a[0] != b[0] || a[1] != b[1] || a[2] != b[2]) {
                    ++diff;
                    if (std::max({std::abs(a[0] - b[0]), std::abs(a[1] - b[1]), std::abs(a[2] - b[2])}) > 8) ++big;
                    if (firstX < 0) { firstX = x; firstY = y; std::printf("first diff at (%d,%d): client BGR %d,%d,%d  gdi BGR %d,%d,%d\n", x, y, a[0], a[1], a[2], b[0], b[1], b[2]); }
                    minX = std::min(minX, x); minY = std::min(minY, y); maxX = std::max(maxX, x); maxY = std::max(maxY, y);
                }
            }
            std::printf("verify vs GDI screenshot: %zu of %zu pixels differ (%.4f%%)", diff, total, 100.0 * diff / total);
            std::printf(" [%zu differ by more than 8]", big);
            if (diff) std::printf(", bounding box (%d,%d)-(%d,%d)", minX, minY, maxX, maxY);
            std::printf("\n");
            return diff * 1000 > total ? 3 : 0;  // allow up to 0.1% for live content (clock, caret, ...)
        }
        return 0;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "error: %s\n", ex.what());
        return 1;
    }
}
