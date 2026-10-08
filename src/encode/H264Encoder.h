#pragma once
#include <mfidl.h>
#include <mftransform.h>
#include <strmif.h>
#include <wrl/client.h>

#include <memory>
#include <vector>

#include "encode/Encoder.h"

namespace rfbenc {

// H.264 baseline encoder (Media Foundation software MFT, low-latency, no B-frames) for the "Open H.264" RFB
// encoding. Frames are padded up to a multiple of 16 pixels; the RFB client crops the excess.
class H264Encoder {
public:
    // Returns nullptr when Media Foundation or the H.264 encoder is unavailable.
    static std::unique_ptr<H264Encoder> create(int width, int height, int fps, int bitrateKbps);

    int codedWidth() const { return cw_; }
    int codedHeight() const { return ch_; }
    void setBitrate(int kbps);

    // Encodes the rectangle (must equal the size given to create(), up to the 16-pixel padding) and appends an
    // Annex-B access unit to `out`. Returns false on failure or when the encoder produced nothing.
    bool encode(const FrameView& fb, const Rect& r, bool keyframe, std::vector<uint8_t>& out);

private:
    H264Encoder() = default;
    bool init(int width, int height, int fps, int bitrateKbps);

    Microsoft::WRL::ComPtr<IMFTransform> mft_;
    Microsoft::WRL::ComPtr<ICodecAPI> api_;
    int w_ = 0, h_ = 0, cw_ = 0, ch_ = 0, fps_ = 30;
    long long sampleTime_ = 0;
    DWORD outBufSize_ = 0;
    bool mftAllocates_ = false;
    std::vector<uint8_t> nv12_;
};

// BT.709 limited-range BGRA -> NV12 for a w x h region padded to cw x ch by edge replication.
void bgraToNv12(const FrameView& fb, const Rect& r, int cw, int ch, uint8_t* out);

}  // namespace rfbenc
