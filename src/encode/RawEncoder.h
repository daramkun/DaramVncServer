#pragma once
#include "encode/Encoder.h"

namespace rfbenc {

// Uncompressed pixels in the client's format; fallback for clients without Tight/ZRLE.
class RawEncoder : public RectEncoder {
public:
    explicit RawEncoder(const PixelFormat& pf) : pf_(pf) {}
    void setPixelFormat(const PixelFormat& pf) { pf_ = pf; }
    int encode(const FrameView& fb, const Rect& r, std::vector<uint8_t>& out) override {
        putRectHeader(out, r.x, r.y, r.w, r.h, kRaw);
        out.reserve(out.size() + static_cast<size_t>(r.w) * r.h * (pf_.bpp / 8));
        for (int y = r.y; y < r.y + r.h; ++y) pf_.convertRow(fb.at(r.x, y), r.w, out);
        return 1;
    }

private:
    PixelFormat pf_;
};

}  // namespace rfbenc
