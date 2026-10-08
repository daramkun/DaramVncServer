#include "capture/MultiCapturer.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

std::unique_ptr<ICapturer> createDesktopCapturer() {
    const auto outputs = DxgiCapturer::enumerateOutputs();
    if (outputs.empty()) return nullptr;
    if (outputs.size() == 1) return DxgiCapturer::tryCreate(outputs[0]);
    return MultiCapturer::create();
}

std::unique_ptr<MultiCapturer> MultiCapturer::create() {
    auto m = std::unique_ptr<MultiCapturer>(new MultiCapturer());
    m->layoutChangedPending_ = false;
    return m->build() ? std::move(m) : nullptr;
}

bool MultiCapturer::build() {
    const auto outputs = DxgiCapturer::enumerateOutputs();
    if (outputs.empty()) return false;

    std::vector<Sub> subs;
    long minL = LONG_MAX, minT = LONG_MAX, maxR = LONG_MIN, maxB = LONG_MIN;
    for (const auto& o : outputs) {
        auto cap = DxgiCapturer::tryCreate(o);
        if (!cap) return false;
        minL = std::min(minL, o.left); minT = std::min(minT, o.top);
        maxR = std::max(maxR, o.right); maxB = std::max(maxB, o.bottom);
        subs.push_back({std::move(cap), static_cast<int>(o.left), static_cast<int>(o.top)});
    }
    std::vector<ScreenInfo> screens;
    uint32_t id = 1;
    for (Sub& s : subs) {
        s.ox -= static_cast<int>(minL);
        s.oy -= static_cast<int>(minT);
        screens.push_back({id++, s.ox, s.oy, s.cap->width(), s.cap->height()});
    }
    const int w = static_cast<int>(maxR - minL), h = static_cast<int>(maxB - minT);
    auto same = [](const ScreenInfo& a, const ScreenInfo& b) { return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h; };
    bool changed = screens.size() != screens_.size() || w != w_ || h != h_;
    for (size_t i = 0; !changed && i < screens.size(); ++i) changed = !same(screens[i], screens_[i]);
    if (!screens_.empty() && changed) layoutChangedPending_ = true;

    subs_ = std::move(subs);
    screens_ = std::move(screens);
    w_ = w;
    h_ = h;
    fb_.assign(static_cast<size_t>(w_) * h_ * 4, 0);
    return true;
}

void MultiCapturer::blit(const Sub& s, const Rect& r) {
    const uint8_t* src = s.cap->pixels();
    const int sw = s.cap->width();
    for (int y = r.y; y < r.y + r.h; ++y)
        std::memcpy(&fb_[(static_cast<size_t>(s.oy + y) * w_ + s.ox + r.x) * 4], src + (static_cast<size_t>(y) * sw + r.x) * 4,
                    static_cast<size_t>(r.w) * 4);
}

void MultiCapturer::merge(const Sub& s, FrameUpdate& su, FrameUpdate& out, bool full) {
    for (const MoveRect& m : su.moves) {
        blit(s, m.dst);
        out.moves.push_back({{m.dst.x + s.ox, m.dst.y + s.oy, m.dst.w, m.dst.h}, m.srcX + s.ox, m.srcY + s.oy});
    }
    for (const Rect& r : su.dirty) {
        blit(s, r);
        if (!full) out.dirty.push_back({r.x + s.ox, r.y + s.oy, r.w, r.h});
    }
    if (su.hasCursor) {
        out.cursor = std::move(su.cursor);
        out.hasCursor = true;
    }
}

AcquireResult MultiCapturer::acquire(int timeoutMs, FrameUpdate& out) {
    out.clear();
    const auto deadline = GetTickCount64() + static_cast<ULONGLONG>(std::max(timeoutMs, 0));
    const int slice = std::clamp(timeoutMs / (2 * static_cast<int>(subs_.size())), 0, 8);
    for (;;) {
        for (Sub& s : subs_) {
            FrameUpdate su;
            const AcquireResult r = s.cap->acquire(slice, su);
            if (r == AcquireResult::Failed || su.resized) {
                // A monitor went away, appeared or changed mode: rebuild everything and resend the screen.
                if (!build()) return AcquireResult::Failed;
                layoutChangedPending_ = true;
                return grabFull(out) ? AcquireResult::Updated : AcquireResult::Failed;
            }
            if (r == AcquireResult::Updated) merge(s, su, out, false);
        }
        if (!out.dirty.empty() || !out.moves.empty() || out.hasCursor) return AcquireResult::Updated;
        if (GetTickCount64() >= deadline) return AcquireResult::Timeout;
    }
}

bool MultiCapturer::grabFull(FrameUpdate& out) {
    out.clear();
    for (Sub& s : subs_) {
        FrameUpdate su;
        if (!s.cap->grabFull(su)) return false;
        const Rect all{0, 0, s.cap->width(), s.cap->height()};
        blit(s, all);
        if (su.hasCursor) {
            out.cursor = std::move(su.cursor);
            out.hasCursor = true;
        }
    }
    out.dirty.push_back({0, 0, w_, h_});
    out.resized = layoutChangedPending_;
    layoutChangedPending_ = false;
    return true;
}
