#pragma once
#include <windows.h>

#include "capture/ICapturer.h"

// Fallback capturer (GDI BitBlt + row-band diff) for when DXGI duplication is unavailable.
class GdiCapturer : public ICapturer {
public:
    GdiCapturer();
    ~GdiCapturer() override;
    GdiCapturer(const GdiCapturer&) = delete;
    GdiCapturer& operator=(const GdiCapturer&) = delete;

    int width() const override { return width_; }
    int height() const override { return height_; }
    const uint8_t* pixels() const override { return pixels_.data(); }
    AcquireResult acquire(int timeoutMs, FrameUpdate& out) override;
    bool grabFull(FrameUpdate& out) override;

private:
    void capture();

    int width_ = 0, height_ = 0;
    HDC screenDc_ = nullptr, memDc_ = nullptr;
    HBITMAP bmp_ = nullptr, oldBmp_ = nullptr;
    std::vector<uint8_t> pixels_, tmp_;  // tmp_ receives each screenshot; pixels_ is what consumers read
};
