#pragma once
#include <zlib.h>

#include <array>

#include "encode/Encoder.h"

namespace rfbenc {

// Tight encoding (fill / palette / zlib / JPEG). Requires a 32bpp, depth-24, 8-bit-per-channel client format
// (3-byte TPIXEL). Rects are split into 64-row bands; adjacent tiles of the same class are merged.
class TightEncoder : public RectEncoder {
public:
    TightEncoder(const PixelFormat& pf, int jpegQuality, int zlibLevel);
    ~TightEncoder() override;
    TightEncoder(const TightEncoder&) = delete;
    TightEncoder& operator=(const TightEncoder&) = delete;

    // 1..99 enables lossy JPEG for photo-like tiles; anything else keeps every rect lossless.
    void setJpegQuality(int q) { jpegQuality_ = q; }
    void setZlibLevel(int l) { zlibLevel_ = l; }
    // Lossless pass: no JPEG even for photo tiles (used to refine regions previously sent lossy).
    void setLossless(bool on) { lossless_ = on; }
    // Whether the previous encode() emitted any lossy (JPEG) rect.
    bool lastEncodeHadLossy() const { return lastLossy_; }

    int encode(const FrameView& fb, const Rect& r, std::vector<uint8_t>& out) override;

private:
    enum class Kind { Solid, Flat, Photo };
    struct Run {
        int x, y, w, h;
        Kind kind;
        uint32_t color = 0;  // 0x00RRGGBB, solid only
        std::vector<uint8_t> jpeg;
    };

    void encodeBasic(const FrameView& fb, const Run& r, std::vector<uint8_t>& out);
    void deflateRect(int stream, uint8_t ctrl, const std::vector<uint8_t>& prefix, const std::vector<uint8_t>& data,
                     std::vector<uint8_t>& out);

    PixelFormat pf_;
    int jpegQuality_, zlibLevel_;
    bool lossless_ = false, lastLossy_ = false;
    std::array<z_stream, 4> zs_{};
    std::array<bool, 4> zInit_{};
    std::array<int, 4> zLevel_{};
    std::vector<uint8_t> zbuf_;
};

// Writes Tight's 1-3 byte compact length.
void putCompactLength(std::vector<uint8_t>& out, size_t len);

// Maps a client quality level (0..9, from pseudo-encodings -32..-23) to a JPEG quality; 100 means lossless.
int jpegQualityForLevel(int level);

}  // namespace rfbenc
