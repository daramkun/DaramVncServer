// Updater thread: shared capture hub -> classify/encode -> send, with fence-based pacing and adaptive quality.
#include <algorithm>

#include "capture/CursorCapture.h"
#include "rfb/Connection.h"
#include "util/Desktop.h"
#include "util/Log.h"

using namespace std::chrono;
using rfbenc::put16;
using rfbenc::put32;

namespace {

constexpr int kMaxOutstandingFences = 2;
constexpr auto kRefineDelay = milliseconds(300);
constexpr size_t kMaxLossyRects = 12;

Rect unionRect(const Rect& a, const Rect& b) {
    const int x = std::min(a.x, b.x), y = std::min(a.y, b.y);
    return {x, y, std::max(a.x + a.w, b.x + b.w) - x, std::max(a.y + a.h, b.y + b.h) - y};
}

Rect clipTo(const Rect& r, int w, int h) {
    const int x1 = std::max(r.x, 0), y1 = std::max(r.y, 0), x2 = std::min(r.x + r.w, w), y2 = std::min(r.y + r.h, h);
    return x2 > x1 && y2 > y1 ? Rect{x1, y1, x2 - x1, y2 - y1} : Rect{};
}

// Highest JPEG quality worth using at an estimated throughput (bytes/s); 0 = unknown -> no cap.
int jpegCapForBandwidth(double bw) {
    if (bw <= 0) return 100;
    if (bw < 300e3) return 25;
    if (bw < 1e6) return 40;
    if (bw < 3e6) return 60;
    if (bw < 8e6) return 80;
    return 100;
}

int zlibForBandwidth(double bw) {
    if (bw <= 0 || bw > 30e6) return 2;  // fast network: spend little CPU
    if (bw > 5e6) return 3;
    return 6;
}

}  // namespace

bool Connection::sendDesktopSizeMsg(const Caps& caps, const CaptureHub::Layout& layout, int reason, int errorCode) {
    std::vector<uint8_t> msg{rfb::kMsgFramebufferUpdate, 0, 0, 1};
    if (caps.extDesktopSize) {
        rfbenc::putRectHeader(msg, reason, errorCode, layout.w, layout.h, rfb::kEncExtDesktopSize);
        msg.insert(msg.end(), {static_cast<uint8_t>(layout.screens.size()), 0, 0, 0});
        for (const ScreenInfo& sc : layout.screens) {
            put32(msg, sc.id);
            put16(msg, static_cast<uint16_t>(sc.x)); put16(msg, static_cast<uint16_t>(sc.y));
            put16(msg, static_cast<uint16_t>(sc.w)); put16(msg, static_cast<uint16_t>(sc.h));
            put32(msg, 0);  // flags
        }
    } else if (caps.desktopSize && reason != 1) {
        rfbenc::putRectHeader(msg, 0, 0, layout.w, layout.h, rfb::kEncDesktopSize);
    } else {
        return true;
    }
    return send(msg);
}

void Connection::appendCursor(std::vector<uint8_t>& msg, int& count, const PixelFormat& pf) {
    const CursorShape& c = lastCursor_;
    rfbenc::putRectHeader(msg, c.hotX, c.hotY, c.w, c.h, rfb::kEncCursor);
    ++count;
    for (int y = 0; y < c.h; ++y) pf.convertRow(&c.bgra[static_cast<size_t>(y) * c.w * 4], c.w, msg);
    const int rowBytes = (c.w + 7) / 8;
    for (int y = 0; y < c.h; ++y)
        for (int bx = 0; bx < rowBytes; ++bx) {
            uint8_t b = 0;
            for (int bit = 0; bit < 8; ++bit) {
                const int x = bx * 8 + bit;
                if (x < c.w && c.mask[static_cast<size_t>(y) * c.w + x]) b |= 0x80 >> bit;
            }
            msg.push_back(b);
        }
}

// SetColourMapEntries for the fixed RGB332 palette used by 8bpp colour-mapped clients.
bool Connection::sendColourMap() {
    std::vector<uint8_t> m{1, 0};
    put16(m, 0);    // first colour
    put16(m, 256);  // number of colours
    for (int i = 0; i < 256; ++i) {
        uint8_t r, g, b;
        PixelFormat::rgb332Entry(i, r, g, b);
        put16(m, static_cast<uint16_t>(r * 257));
        put16(m, static_cast<uint16_t>(g * 257));
        put16(m, static_cast<uint16_t>(b * 257));
    }
    return send(m);
}

void Connection::appendFence(std::vector<uint8_t>& msg, size_t bytes) {
    uint32_t id;
    {
        std::lock_guard<std::mutex> lk(mu_);
        id = fenceNext_++;
        if (id == 0) id = fenceNext_++;
        fences_[id] = {steady_clock::now(), bytes};
    }
    msg.insert(msg.end(), {rfb::kMsgFence, 0, 0, 0});
    put32(msg, 0x80000001u);  // request | BlockBefore
    msg.push_back(4);
    put32(msg, id);
}

rfbenc::RectEncoder* Connection::pickEncoder(const Caps& caps, const PixelFormat& pf) {
    if (caps.tight && pf.tightPixel24()) {
        if (!tight_) tight_ = std::make_unique<rfbenc::TightEncoder>(pf, -1, 3);
        return tight_.get();
    }
    if (caps.zrle) {
        if (!zrle_) zrle_ = std::make_unique<rfbenc::ZrleEncoder>(pf, 3);
        else zrle_->setPixelFormat(pf);
        return zrle_.get();
    }
    if (!raw_) raw_ = std::make_unique<rfbenc::RawEncoder>(pf);
    else raw_->setPixelFormat(pf);
    return raw_.get();
}

void Connection::adaptQuality(const Caps& caps) {
    double bw;
    {
        std::lock_guard<std::mutex> lk(mu_);
        bw = bandwidth_;
    }
    const int clientQ = caps.quality >= 0 ? rfbenc::jpegQualityForLevel(caps.quality) : -1;
    adaptZlib_ = zlibForBandwidth(bw);
    if (clientQ < 0 || clientQ >= 100) adaptJpeg_ = clientQ;
    else adaptJpeg_ = std::min(clientQ, jpegCapForBandwidth(bw));
    if (tight_) {
        tight_->setJpegQuality(adaptJpeg_);
        tight_->setZlibLevel(adaptZlib_);
    }
}

int Connection::encodeRects(rfbenc::RectEncoder* enc, const rfbenc::FrameView& fb, const std::vector<Rect>& rects,
                            std::vector<uint8_t>& msg) {
    int count = 0;
    for (const Rect& r : rects) {
        count += enc->encode(fb, r, msg);
        if (tight_ && enc == tight_.get() && tight_->lastEncodeHadLossy()) {
            lossy_.push_back(r);
            if (lossy_.size() > kMaxLossyRects) {
                Rect u = lossy_[0];
                for (const Rect& l : lossy_) u = unionRect(u, l);
                lossy_.assign(1, u);
            }
        }
    }
    return count;
}

void Connection::recordStats(std::chrono::steady_clock::duration encode, std::chrono::steady_clock::duration sendTime, size_t bytes) {
    stats_.updates++;
    stats_.encodeMs += duration<double, std::milli>(encode).count();
    stats_.sendMs += duration<double, std::milli>(sendTime).count();
    stats_.bytes += bytes;
    const double secs = duration<double>(steady_clock::now() - stats_.since).count();
    if (secs >= 10.0) {
        if (stats_.updates >= 20) {
            double bw, rtt;
            {
                std::lock_guard<std::mutex> lk(mu_);
                bw = bandwidth_;
                rtt = minRttMs_;
            }
            LOGI("%s: %.1f updates/s, %.0f KB/update, encode %.2f ms, send %.2f ms per update, link ~%.1f MB/s (base latency %.1f ms), jpeg<=%d zlib %d",
                 peer_.c_str(), stats_.updates / secs, stats_.bytes / 1024.0 / stats_.updates, stats_.encodeMs / stats_.updates,
                 stats_.sendMs / stats_.updates, bw / 1048576.0, rtt < 1e8 ? rtt : 0.0, adaptJpeg_, adaptZlib_);
        }
        stats_ = Stats{};
        stats_.since = steady_clock::now();
    }
}

Connection::UpdateResult Connection::sendUpdate(bool nonIncremental, const Caps& caps, const PixelFormat& pf, unsigned pfGen) {
    Rect region;
    bool forceFull, cursorDirty, announceKey;
    {
        std::lock_guard<std::mutex> lk(mu_);
        announceKey = announceQemuKey_;
        region = continuous_ ? continuousRect_ : interest_;
        forceFull = forceFull_;
        forceFull_ = false;
        cursorDirty = cursorDirty_;
    }
    // A colour-mapped client needs the palette before it can interpret any pixel (re-sent after each format change).
    if (pf.colorMapped() && colormapGeneration_ != pfGen) {
        if (!sendColourMap()) return UpdateResult::Error;
        colormapGeneration_ = pfGen;
    }
    CaptureHub::Layout layout = hub_->layout();
    // A snapshot request owes the client the whole requested region again; a pixel-format change owes everything.
    if (forceFull) hub_->markDamage(*sub_, Rect{0, 0, layout.w, layout.h});
    else if (nonIncremental) hub_->markDamage(*sub_, region);

    if (!hub_->wait(*sub_, (forceFull || nonIncremental) ? 0 : 30, region)) return UpdateResult::Error;
    CaptureHub::Delta d;
    if (!hub_->take(*sub_, region, d) && !announceKey) return UpdateResult::Idle;
    layout = hub_->layout();

    if (d.resized) {
        input_->setSize(layout.w, layout.h);
        lossy_.clear();
        if (!caps.extDesktopSize && !caps.desktopSize) {
            LOGI("%s: screen resized to %dx%d but client cannot follow", peer_.c_str(), layout.w, layout.h);
            return UpdateResult::Error;
        }
        if (!sendDesktopSizeMsg(caps, layout, d.resizeReason, 0)) return UpdateResult::Error;
    } else if (nonIncremental && caps.extDesktopSize) {
        // Required answer to a non-incremental request: tells the client the layout and that resizing is refused.
        if (!sendDesktopSizeMsg(caps, layout, 0, 0)) return UpdateResult::Error;
    }

    if (d.hasCursor) {
        lastCursor_ = std::move(d.cursor);
        haveCursor_ = true;
        cursorDirty = true;
    }
    if (cursorDirty && !haveCursor_) haveCursor_ = captureCurrentCursor(lastCursor_);

    std::vector<uint8_t> msg{rfb::kMsgFramebufferUpdate, 0, 0, 0};
    int count = 0;
    if (caps.cursor && haveCursor_ && cursorDirty) {
        appendCursor(msg, count, pf);
        std::lock_guard<std::mutex> lk(mu_);
        cursorDirty_ = false;
    }

    if (announceKey) {  // empty pseudo-rectangle: "extended key events are welcome"
        rfbenc::putRectHeader(msg, 0, 0, 0, 0, rfb::kEncQemuKey);
        ++count;
        std::lock_guard<std::mutex> lk(mu_);
        announceQemuKey_ = false;
    }

    const auto encodeStart = steady_clock::now();
    {
        // Pixels are read under a shared lock so the capture thread cannot change them mid-encode.
        CaptureHub::View view = hub_->view();
        const rfbenc::FrameView fb{view.bgra, view.w, view.h};
        std::vector<Rect> dirty;
        for (const MoveRect& m : d.moves) {
            const Rect dst = clipTo(m.dst, view.w, view.h);
            if (dst.w != m.dst.w || dst.h != m.dst.h) {
                if (dst.w > 0) dirty.push_back(dst);
                continue;
            }
            if (caps.copyRect) {
                rfbenc::putRectHeader(msg, m.dst.x, m.dst.y, m.dst.w, m.dst.h, rfb::kEncCopyRect);
                put16(msg, static_cast<uint16_t>(m.srcX));
                put16(msg, static_cast<uint16_t>(m.srcY));
                ++count;
            } else {
                dirty.push_back(m.dst);  // the framebuffer already holds the moved pixels
            }
        }
        for (const Rect& r : d.dirty) {
            const Rect c = clipTo(r, view.w, view.h);
            if (c.w > 0) dirty.push_back(c);
        }
        if (!dirty.empty()) {
            lastChange_ = encodeStart;
            adaptQuality(caps);
            if (cfg_.h264 && caps.h264) handleVideo(dirty, fb, caps, msg, count);  // may take over a video-like region
            if (!dirty.empty()) count += encodeRects(pickEncoder(caps, pf), fb, dirty, msg);
        }
    }
    const auto encodeEnd = steady_clock::now();

    if (count == 0) return UpdateResult::Idle;
    msg[2] = static_cast<uint8_t>(count >> 8);
    msg[3] = static_cast<uint8_t>(count & 0xFF);
    if (caps.fence) appendFence(msg, msg.size());
    const size_t wireBytes = msg.size();
    if (!send(msg)) return UpdateResult::Error;
    recordStats(encodeEnd - encodeStart, steady_clock::now() - encodeEnd, wireBytes);

    // The request is answered either way; in continuous mode the client sends no more of them, and a stale
    // "pending non-incremental" flag would turn every later update into a full snapshot.
    std::lock_guard<std::mutex> lk(mu_);
    requestPending_ = false;
    return UpdateResult::Sent;
}

// Re-sends regions that went out as JPEG, losslessly, once the screen has been quiet for a moment.
bool Connection::sendRefine(const Caps& caps, const PixelFormat& pf) {
    if (lossy_.empty() || !tight_ || pickEncoder(caps, pf) != tight_.get()) {
        lossy_.clear();
        return true;
    }
    std::vector<Rect> rects = std::move(lossy_);
    lossy_.clear();
    std::vector<uint8_t> msg{rfb::kMsgFramebufferUpdate, 0, 0, 0};
    int count = 0;
    {
        CaptureHub::View view = hub_->view();
        const rfbenc::FrameView fb{view.bgra, view.w, view.h};
        std::vector<Rect> clipped;
        for (const Rect& r : rects) {
            const Rect c = clipTo(r, view.w, view.h);
            if (c.w > 0) clipped.push_back(c);
        }
        tight_->setLossless(true);
        count = encodeRects(tight_.get(), fb, clipped, msg);
        tight_->setLossless(false);
    }
    lossy_.clear();  // lossless pass reports nothing lossy
    if (count == 0) return true;
    msg[2] = static_cast<uint8_t>(count >> 8);
    msg[3] = static_cast<uint8_t>(count & 0xFF);
    if (caps.fence) appendFence(msg, msg.size());
    if (!send(msg)) return false;
    std::lock_guard<std::mutex> lk(mu_);
    requestPending_ = false;
    return true;
}

void Connection::updaterMain() {
    while (true) {
        Caps caps;
        PixelFormat pf;
        unsigned pfGen;
        bool nonIncremental;
        {
            std::unique_lock<std::mutex> lk(mu_);
            for (;;) {
                // Forget fences the client never answered so a silent client cannot stall us forever.
                const auto now = steady_clock::now();
                for (auto it = fences_.begin(); it != fences_.end();)
                    it = (now - it->second.sent > seconds(2)) ? fences_.erase(it) : std::next(it);
                const bool want = requestPending_ || continuous_;
                // Keep the amount of unanswered data near what the link moves in ~80 ms so queues cannot add latency.
                size_t inflight = 0;
                for (const auto& f : fences_) inflight += f.second.bytes;
                const size_t budget = bandwidth_ > 0 ? std::max<size_t>(96 * 1024, static_cast<size_t>(bandwidth_ * 0.08)) : SIZE_MAX;
                const bool congested = caps_.fence && !fences_.empty() &&
                                       (fences_.size() >= static_cast<size_t>(kMaxOutstandingFences) || inflight >= budget);
                if (stop_) return;
                if (want && !congested) break;
                cv_.wait_for(lk, milliseconds(250));
            }
            caps = caps_;
            pf = pf_;
            pfGen = pfGeneration_;
            nonIncremental = requestPending_ && !requestIncremental_;
        }

        const UpdateResult r = sendUpdate(nonIncremental, caps, pf, pfGen);
        if (r == UpdateResult::Error) {
            dead_ = true;
            tp_->shutdown();
            return;
        }
        if (r == UpdateResult::Idle) maintainVideo();
        if (r == UpdateResult::Idle && !lossy_.empty() && steady_clock::now() - lastChange_ > kRefineDelay) {
            if (!sendRefine(caps, pf)) {
                dead_ = true;
                tp_->shutdown();
                return;
            }
        }
    }
}
