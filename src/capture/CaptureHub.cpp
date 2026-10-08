#include "capture/CaptureHub.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

#include "capture/GdiCapturer.h"
#include "capture/MultiCapturer.h"
#include "util/Log.h"

namespace {

constexpr size_t kMaxPendingRects = 16;

bool empty(const Rect& r) { return r.w <= 0 || r.h <= 0; }

Rect intersect(const Rect& a, const Rect& b) {
    const int x1 = std::max(a.x, b.x), y1 = std::max(a.y, b.y);
    const int x2 = std::min(a.x + a.w, b.x + b.w), y2 = std::min(a.y + a.h, b.y + b.h);
    return x2 > x1 && y2 > y1 ? Rect{x1, y1, x2 - x1, y2 - y1} : Rect{};
}

bool contains(const Rect& outer, const Rect& inner) {
    return inner.x >= outer.x && inner.y >= outer.y && inner.x + inner.w <= outer.x + outer.w &&
           inner.y + inner.h <= outer.y + outer.h;
}

// a minus b (b assumed to lie inside a, as an intersection result): up to four rectangles.
void subtract(const Rect& a, const Rect& b, std::vector<Rect>& out) {
    if (b.y > a.y) out.push_back({a.x, a.y, a.w, b.y - a.y});
    if (b.y + b.h < a.y + a.h) out.push_back({a.x, b.y + b.h, a.w, a.y + a.h - (b.y + b.h)});
    if (b.x > a.x) out.push_back({a.x, b.y, b.x - a.x, b.h});
    if (b.x + b.w < a.x + a.w) out.push_back({b.x + b.w, b.y, a.x + a.w - (b.x + b.w), b.h});
}

}  // namespace

CaptureHub::~CaptureHub() {
    stop_ = true;
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void CaptureHub::addDirty(std::vector<Rect>& list, const Rect& r) {
    if (empty(r)) return;
    for (const Rect& e : list)
        if (contains(e, r)) return;  // already pending
    list.push_back(r);
    if (list.size() > kMaxPendingRects) {  // too fragmented: one bounding box is cheaper than tracking them all
        Rect bb = list[0];
        int x2 = bb.x + bb.w, y2 = bb.y + bb.h;
        for (const Rect& d : list) {
            bb.x = std::min(bb.x, d.x); bb.y = std::min(bb.y, d.y);
            x2 = std::max(x2, d.x + d.w); y2 = std::max(y2, d.y + d.h);
        }
        bb.w = x2 - bb.x; bb.h = y2 - bb.y;
        list.assign(1, bb);
    }
}

bool CaptureHub::startLocked() {
    failed_ = false;
    stop_ = false;
    capturer_ = createDesktopCapturer();
    if (!capturer_) {
        LOGI("DXGI duplication unavailable, using GDI capture");
        capturer_ = std::make_unique<GdiCapturer>();
    }
    FrameUpdate u;
    if (!capturer_->grabFull(u)) {
        capturer_.reset();
        return false;
    }
    {
        std::unique_lock<std::shared_mutex> fb(fbMu_);
        fbW_ = capturer_->width();
        fbH_ = capturer_->height();
        fb_.assign(capturer_->pixels(), capturer_->pixels() + static_cast<size_t>(fbW_) * fbH_ * 4);
    }
    layout_ = {capturer_->width(), capturer_->height(), capturer_->screens()};
    thread_ = std::thread([this] { captureLoop(); });
    return true;
}

std::shared_ptr<CaptureHub::Subscription> CaptureHub::subscribe() {
    std::lock_guard<std::mutex> life(lifecycleMu_);
    std::unique_lock<std::mutex> lk(mu_);
    if (subs_.empty() && !startLocked()) return nullptr;
    auto s = std::make_shared<Subscription>();
    s->id_ = nextId_++;
    s->dirty.push_back({0, 0, layout_.w, layout_.h});  // a new client starts with nothing: owes it the whole screen
    subs_.push_back(s);
    return s;
}

void CaptureHub::unsubscribe(const std::shared_ptr<Subscription>& sub) {
    std::lock_guard<std::mutex> life(lifecycleMu_);
    bool last = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        subs_.erase(std::remove(subs_.begin(), subs_.end(), sub), subs_.end());
        last = subs_.empty();
    }
    if (!last) return;
    stop_ = true;
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    capturer_.reset();
    if (originalModeSaved_) {  // give the display back its configured resolution
        ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
        originalModeSaved_ = false;
    }
}

void CaptureHub::captureLoop() {
    while (!stop_) {
        FrameUpdate u;
        const AcquireResult r = capturer_->acquire(30, u);
        if (r == AcquireResult::Failed) {
            LOGI("screen capture failed");
            failed_ = true;
            cv_.notify_all();
            return;
        }
        if (r == AcquireResult::Timeout) continue;
        applyToFramebuffer(u);
        distribute(u);
    }
}

void CaptureHub::applyToFramebuffer(const FrameUpdate& u) {
    std::unique_lock<std::shared_mutex> fb(fbMu_);
    const int w = capturer_->width(), h = capturer_->height();
    const uint8_t* src = capturer_->pixels();
    if (u.resized || w != fbW_ || h != fbH_) {  // geometry changed: the capturer's buffer holds the whole new screen
        fbW_ = w;
        fbH_ = h;
        fb_.assign(src, src + static_cast<size_t>(w) * h * 4);
        return;
    }
    auto copy = [&](const Rect& r) {
        for (int y = r.y; y < r.y + r.h; ++y)
            std::memcpy(&fb_[(static_cast<size_t>(y) * w + r.x) * 4], src + (static_cast<size_t>(y) * w + r.x) * 4,
                        static_cast<size_t>(r.w) * 4);
    };
    for (const MoveRect& m : u.moves) copy(m.dst);
    for (const Rect& r : u.dirty) copy(r);
}

void CaptureHub::distribute(FrameUpdate& u) {
    std::lock_guard<std::mutex> lk(mu_);
    if (u.resized) layout_ = {capturer_->width(), capturer_->height(), capturer_->screens()};
    const bool recentResize = resizeInitiator_ && GetTickCount64() - resizeTime_ < 5000;
    for (auto& sp : subs_) {
        Subscription& s = *sp;
        if (u.resized) {
            s.dirty.assign(1, Rect{0, 0, layout_.w, layout_.h});
            s.moves.clear();
            s.resized = true;
            s.resizeReason = recentResize ? (s.id_ == resizeInitiator_ ? 1 : 2) : 0;
        } else {
            // CopyRect is only safe while the client mirrors the server exactly, i.e. nothing is pending for it.
            const bool inSync = s.dirty.empty() && s.moves.empty();
            for (const MoveRect& m : u.moves) {
                if (inSync) s.moves.push_back(m);
                else addDirty(s.dirty, m.dst);
            }
            for (const Rect& r : u.dirty) addDirty(s.dirty, r);
        }
        if (u.hasCursor) {
            s.hasCursor = true;
            s.cursor = u.cursor;
        }
    }
    cv_.notify_all();
}

bool CaptureHub::wait(Subscription& s, int timeoutMs, const Rect& region) {
    std::unique_lock<std::mutex> lk(mu_);
    auto ready = [&] {
        if (failed_ || s.resized || s.hasCursor || !s.moves.empty()) return true;
        for (const Rect& r : s.dirty)
            if (!empty(intersect(r, region))) return true;
        return false;
    };
    cv_.wait_for(lk, std::chrono::milliseconds(std::max(timeoutMs, 0)), ready);
    return !failed_;
}

bool CaptureHub::take(Subscription& s, const Rect& region, Delta& out) {
    std::lock_guard<std::mutex> lk(mu_);
    out = Delta{};
    bool any = false;
    if (s.resized) {
        out.resized = true;
        out.resizeReason = s.resizeReason;
        s.resized = false;
        any = true;
    }
    if (s.hasCursor) {
        out.hasCursor = true;
        out.cursor = std::move(s.cursor);
        s.hasCursor = false;
        any = true;
    }
    const Rect reg = intersect(region, Rect{0, 0, layout_.w, layout_.h});
    for (const MoveRect& m : s.moves) {
        if (contains(reg, m.dst)) { out.moves.push_back(m); any = true; }
        else addDirty(s.dirty, m.dst);  // partly outside the interest region: send as ordinary damage later
    }
    s.moves.clear();
    std::vector<Rect> keep;
    for (const Rect& r : s.dirty) {
        const Rect in = intersect(r, reg);
        if (empty(in)) { keep.push_back(r); continue; }
        out.dirty.push_back(in);
        any = true;
        subtract(r, in, keep);
    }
    s.dirty = std::move(keep);
    return any;
}

void CaptureHub::markDamage(Subscription& s, const Rect& region) {
    std::lock_guard<std::mutex> lk(mu_);
    addDirty(s.dirty, intersect(region, Rect{0, 0, layout_.w, layout_.h}));
}

CaptureHub::View CaptureHub::view() {
    std::shared_lock<std::shared_mutex> lock(fbMu_);
    View v;
    v.bgra = fb_.data();
    v.w = fbW_;
    v.h = fbH_;
    v.lock = std::move(lock);
    return v;
}

CaptureHub::Layout CaptureHub::layout() {
    std::lock_guard<std::mutex> lk(mu_);
    return layout_;
}

int CaptureHub::requestResize(Subscription& by, int w, int h) {
    const Layout l = layout();
    if (l.screens.size() != 1) return 1;  // several monitors: no sensible way to map one size onto them
    if (w == l.w && h == l.h) return 0;

    DEVMODEW cur{};
    cur.dmSize = sizeof cur;
    if (!EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &cur)) return 1;
    DEVMODEW best{};
    bool found = false;
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &dm); ++i) {
        if (static_cast<int>(dm.dmPelsWidth) != w || static_cast<int>(dm.dmPelsHeight) != h) continue;
        if (dm.dmBitsPerPel != cur.dmBitsPerPel || dm.dmDisplayOrientation != cur.dmDisplayOrientation) continue;
        if (!found || dm.dmDisplayFrequency > best.dmDisplayFrequency) { best = dm; found = true; }
    }
    if (!found) return 3;  // only modes the display really offers; arbitrary sizes need a virtual display driver

    {
        std::lock_guard<std::mutex> lk(mu_);
        resizeInitiator_ = by.id_;
        resizeTime_ = GetTickCount64();
    }
    best.dmFields |= DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
    // CDS_FULLSCREEN: temporary mode, Windows reverts it if this process dies without restoring.
    if (ChangeDisplaySettingsExW(nullptr, &best, nullptr, CDS_FULLSCREEN, nullptr) != DISP_CHANGE_SUCCESSFUL) return 1;
    originalModeSaved_ = true;
    LOGI("display switched to %dx%d on client request", w, h);
    return 0;
}
