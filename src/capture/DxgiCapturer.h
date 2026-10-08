#pragma once
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <memory>

#include "capture/ICapturer.h"

// DXGI Desktop Duplication capture of the primary output. Only dirty regions are read back from the GPU;
// move rects are applied to the CPU framebuffer and reported so they can go out as CopyRect.
class DxgiCapturer : public ICapturer {
public:
    // One attached monitor: its position (virtual desktop coordinates) and where DXGI finds it.
    struct OutputDesc {
        unsigned adapter = 0, output = 0;
        long left = 0, top = 0, right = 0, bottom = 0;
    };
    static std::vector<OutputDesc> enumerateOutputs();
    // Returns nullptr when duplication is unavailable.
    static std::unique_ptr<DxgiCapturer> tryCreate(const OutputDesc& output);

    int width() const override { return w_; }
    int height() const override { return h_; }
    const uint8_t* pixels() const override { return fb_.data(); }
    AcquireResult acquire(int timeoutMs, FrameUpdate& out) override;
    bool grabFull(FrameUpdate& out) override;

private:
    bool init();
    void shutdown();
    AcquireResult recover();
    bool readPointerShape(const DXGI_OUTDUPL_FRAME_INFO& info, CursorShape& out);
    void applyMove(const MoveRect& m);

    OutputDesc sel_;
    Microsoft::WRL::ComPtr<ID3D11Device> dev_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx_;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> dup_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;
    int w_ = 0, h_ = 0;
    std::vector<uint8_t> fb_;
    std::vector<uint8_t> meta_;
    bool needFull_ = true;
    bool resizedPending_ = false;
    unsigned long long failSince_ = 0;  // GetTickCount64() of the first consecutive recovery failure
};
