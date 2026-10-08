#pragma once
#include <zlib.h>

#include "encode/Encoder.h"

namespace rfbenc {

// ZRLE (64x64 tiles; solid / packed palette / plain RLE / palette RLE / raw) over one persistent zlib stream.
// Works for any true-colour client pixel format.
class ZrleEncoder : public RectEncoder {
public:
    ZrleEncoder(const PixelFormat& pf, int zlibLevel);
    ~ZrleEncoder() override;
    ZrleEncoder(const ZrleEncoder&) = delete;
    ZrleEncoder& operator=(const ZrleEncoder&) = delete;

    // The zlib stream outlives pixel-format changes (the client keeps one inflate stream per connection).
    void setPixelFormat(const PixelFormat& pf);
    int encode(const FrameView& fb, const Rect& r, std::vector<uint8_t>& out) override;

private:
    void appendCPixel(std::vector<uint8_t>& out, uint32_t v) const;
    void encodeTile(const FrameView& fb, int x, int y, int w, int h, std::vector<uint8_t>& out);

    PixelFormat pf_;
    int cpixelBytes_, cpixelOffset_;
    z_stream z_{};
    std::vector<uint8_t> raw_, zbuf_;
};

}  // namespace rfbenc
