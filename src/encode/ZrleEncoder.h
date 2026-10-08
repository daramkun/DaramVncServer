#pragma once
#include <memory>

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
    struct Zng;  // zlib-ng deflate stream (hidden from this header)
    std::unique_ptr<Zng> z_;
    std::vector<uint8_t> raw_, zbuf_;
};

}  // namespace rfbenc
