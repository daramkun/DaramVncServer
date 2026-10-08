#include "capture/GdiCapturer.h"

#include <cstring>

GdiCapturer::GdiCapturer() {
    width_ = GetSystemMetrics(SM_CXSCREEN);
    height_ = GetSystemMetrics(SM_CYSCREEN);
    screenDc_ = GetDC(nullptr);
    memDc_ = CreateCompatibleDC(screenDc_);
    bmp_ = CreateCompatibleBitmap(screenDc_, width_, height_);
    oldBmp_ = static_cast<HBITMAP>(SelectObject(memDc_, bmp_));
    pixels_.resize(static_cast<size_t>(width_) * height_ * 4);
    tmp_.resize(pixels_.size());
}

GdiCapturer::~GdiCapturer() {
    SelectObject(memDc_, oldBmp_);
    DeleteObject(bmp_);
    DeleteDC(memDc_);
    ReleaseDC(nullptr, screenDc_);
}

void GdiCapturer::capture() {
    BitBlt(memDc_, 0, 0, width_, height_, screenDc_, 0, 0, SRCCOPY | CAPTUREBLT);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = width_;
    bi.bmiHeader.biHeight = -height_;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    GetDIBits(memDc_, bmp_, 0, static_cast<UINT>(height_), tmp_.data(), &bi, DIB_RGB_COLORS);
}

AcquireResult GdiCapturer::acquire(int timeoutMs, FrameUpdate& out) {
    out.clear();
    capture();
    const size_t stride = static_cast<size_t>(width_) * 4;
    int top = 0, bottom = height_;
    while (top < height_ && !std::memcmp(&tmp_[top * stride], &pixels_[top * stride], stride)) ++top;
    if (top == height_) {
        Sleep(static_cast<DWORD>(timeoutMs));
        return AcquireResult::Timeout;
    }
    while (bottom > top && !std::memcmp(&tmp_[(bottom - 1) * stride], &pixels_[(bottom - 1) * stride], stride)) --bottom;
    {
        FrameWriteGuard guard(frameLock_);  // consumers read pixels_ directly, so publish only the changed rows
        std::memcpy(&pixels_[top * stride], &tmp_[top * stride], (bottom - top) * stride);
    }
    out.dirty.push_back({0, top, width_, bottom - top});
    return AcquireResult::Updated;
}

bool GdiCapturer::grabFull(FrameUpdate& out) {
    out.clear();
    capture();
    {
        FrameWriteGuard guard(frameLock_);
        pixels_ = tmp_;
    }
    out.dirty.push_back({0, 0, width_, height_});
    return true;
}
