// H.264 ("Open H.264", RFB encoding 50) for regions that behave like video: large, photo-like and changing at
// frame rate. Everything else keeps using Tight/ZRLE; when a video region goes quiet it is re-sent losslessly.
#include <algorithm>

#include "encode/TileClass.h"
#include "rfb/Connection.h"
#include "util/Log.h"

using namespace std::chrono;
using rfbenc::put32;

namespace {

constexpr int kMinVideoPixels = 20000;  // ~140x140
constexpr int kStartStreak = 5;         // consecutive fast updates before a video context is opened
constexpr auto kMaxCandidateGap = milliseconds(200);
constexpr auto kQuietBeforeClose = milliseconds(500);
constexpr auto kMinFrameInterval = milliseconds(28);  // cap at ~35 fps

bool isEmpty(const Rect& r) { return r.w <= 0 || r.h <= 0; }

Rect intersectRect(const Rect& a, const Rect& b) {
    const int x1 = std::max(a.x, b.x), y1 = std::max(a.y, b.y);
    const int x2 = std::min(a.x + a.w, b.x + b.w), y2 = std::min(a.y + a.h, b.y + b.h);
    return x2 > x1 && y2 > y1 ? Rect{x1, y1, x2 - x1, y2 - y1} : Rect{};
}

Rect unionRect(const Rect& a, const Rect& b) {
    if (isEmpty(a)) return b;
    if (isEmpty(b)) return a;
    const int x = std::min(a.x, b.x), y = std::min(a.y, b.y);
    return {x, y, std::max(a.x + a.w, b.x + b.w) - x, std::max(a.y + a.h, b.y + b.h) - y};
}

// a minus b (b is an intersection with a): up to four rectangles.
void subtractRect(const Rect& a, const Rect& b, std::vector<Rect>& out) {
    if (b.y > a.y) out.push_back({a.x, a.y, a.w, b.y - a.y});
    if (b.y + b.h < a.y + a.h) out.push_back({a.x, b.y + b.h, a.w, a.y + a.h - (b.y + b.h)});
    if (b.x > a.x) out.push_back({a.x, b.y, b.x - a.x, b.h});
    if (b.x + b.w < a.x + a.w) out.push_back({b.x + b.w, b.y, a.x + a.w - (b.x + b.w), b.h});
}

// Fraction of sampled tiles (up to 64x64) that are photo-like (more than 256 colours).
double photoRatio(const rfbenc::FrameView& fb, const Rect& r) {
    int photo = 0, total = 0;
    const int stepX = std::max(64, r.w / 5), stepY = std::max(64, r.h / 5);
    for (int y = r.y; y + 16 <= r.y + r.h; y += stepY)
        for (int x = r.x; x + 16 <= r.x + r.w; x += stepX) {
            const int w = std::min(64, r.x + r.w - x), h = std::min(64, r.y + r.h - y);
            if (rfbenc::classifyTile(fb, x, y, w, h).kind == 2) ++photo;
            ++total;
        }
    return total ? static_cast<double>(photo) / total : 0.0;
}

}  // namespace

int Connection::videoBitrateKbps() {
    double bw;
    {
        std::lock_guard<std::mutex> lk(mu_);
        bw = bandwidth_;
    }
    if (bw <= 0) return 8000;  // link not measured yet
    return static_cast<int>(std::clamp(bw * 8 * 0.5 / 1000.0, 800.0, 16000.0));
}

void Connection::maintainVideo() {
    if (!video_.active) return;
    if (steady_clock::now() - video_.lastDamage > kQuietBeforeClose) {
        // The video stopped: replace the lossy picture with an exact one (done by the refine pass).
        lossy_.push_back(video_.r);
        video_.active = false;
        video_.enc.reset();
        videoCandStreak_ = 0;
    }
}

void Connection::handleVideo(std::vector<Rect>& dirty, const rfbenc::FrameView& fb, const Caps& caps, std::vector<uint8_t>& msg,
                             int& count) {
    (void)caps;
    if (h264Failed_) return;
    const auto now = steady_clock::now();

    if (!video_.active) {
        Rect cand;
        for (const Rect& r : dirty)
            if (r.w >= 64 && r.h >= 64 && r.w * r.h >= kMinVideoPixels && photoRatio(fb, r) >= 0.7) cand = unionRect(cand, r);
        if (isEmpty(cand)) return;
        // Quantise to 16 px so the context rectangle stays identical from frame to frame.
        const int x = cand.x & ~15, y = cand.y & ~15;
        const int x2 = std::min((cand.x + cand.w + 15) & ~15, fb.width), y2 = std::min((cand.y + cand.h + 15) & ~15, fb.height);
        cand = {x, y, x2 - x, y2 - y};
        const Rect overlap = intersectRect(cand, videoCand_);
        const bool same = videoCandStreak_ > 0 && now - videoCandLast_ < kMaxCandidateGap && !isEmpty(overlap) &&
                          overlap.w * overlap.h * 10 >= std::min(cand.w * cand.h, videoCand_.w * videoCand_.h) * 6;
        videoCand_ = same ? unionRect(videoCand_, cand) : cand;
        videoCandStreak_ = same ? videoCandStreak_ + 1 : 1;
        videoCandLast_ = now;
        if (videoCandStreak_ < kStartStreak) return;

        Rect vr = videoCand_;
        vr.w = std::min(vr.w, fb.width - vr.x);
        vr.h = std::min(vr.h, fb.height - vr.y);
        video_.enc = rfbenc::H264Encoder::create(vr.w, vr.h, 30, videoBitrateKbps());
        if (!video_.enc) {
            LOGI("%s: H.264 encoder unavailable, staying with Tight", peer_.c_str());
            h264Failed_ = true;
            return;
        }
        video_.active = true;
        video_.first = true;
        video_.r = vr;
        video_.lastFrame = {};
        video_.lastDamage = now;
        LOGI("%s: H.264 started for %dx%d region at (%d,%d)", peer_.c_str(), vr.w, vr.h, vr.x, vr.y);
    }

    // Pull the parts of the damage that fall inside the video rectangle out of the Tight path.
    const Rect vr = video_.r;
    std::vector<Rect> rest;
    bool hit = false;
    for (const Rect& r : dirty) {
        const Rect in = intersectRect(r, vr);
        if (isEmpty(in)) { rest.push_back(r); continue; }
        hit = true;
        subtractRect(r, in, rest);
    }
    if (!hit) return;
    video_.lastDamage = now;
    if (now - video_.lastFrame < kMinFrameInterval) {  // too soon: the next frame carries newer pixels anyway
        dirty = std::move(rest);
        return;
    }
    video_.enc->setBitrate(videoBitrateKbps());
    std::vector<uint8_t> data;
    if (!video_.enc->encode(fb, vr, video_.first, data)) {
        LOGI("%s: H.264 encode failed, falling back to Tight", peer_.c_str());
        h264Failed_ = true;
        video_.active = false;
        video_.enc.reset();
        return;  // `dirty` is untouched, so Tight sends the region
    }
    rfbenc::putRectHeader(msg, vr.x, vr.y, vr.w, vr.h, rfb::kEncH264);
    put32(msg, static_cast<uint32_t>(data.size()));
    put32(msg, video_.first ? 1u : 0u);  // ResetContext on the first frame of a context
    msg.insert(msg.end(), data.begin(), data.end());
    ++count;
    video_.first = false;
    video_.lastFrame = now;
    dirty = std::move(rest);
}
