#pragma once
#include <memory>

#include "capture/DxgiCapturer.h"

// Composes one DXGI duplication per monitor into a single framebuffer covering the virtual desktop.
class MultiCapturer : public ICapturer {
public:
    // Returns nullptr when any output cannot be duplicated.
    static std::unique_ptr<MultiCapturer> create();

    int width() const override { return w_; }
    int height() const override { return h_; }
    const uint8_t* pixels() const override { return fb_.data(); }
    std::vector<ScreenInfo> screens() const override { return screens_; }
    AcquireResult acquire(int timeoutMs, FrameUpdate& out) override;
    bool grabFull(FrameUpdate& out) override;

private:
    struct Sub {
        std::unique_ptr<DxgiCapturer> cap;
        int ox = 0, oy = 0;
    };
    bool build();
    void blit(const Sub& s, const Rect& local);
    void merge(const Sub& s, FrameUpdate& su, FrameUpdate& out, bool full);

    std::vector<Sub> subs_;
    std::vector<ScreenInfo> screens_;
    std::vector<uint8_t> fb_;
    int w_ = 0, h_ = 0;
    bool layoutChangedPending_ = false;
};

// DXGI capture of the whole desktop (one output or several); nullptr when DXGI duplication is unavailable.
std::unique_ptr<ICapturer> createDesktopCapturer();
