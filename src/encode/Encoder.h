#pragma once
#include <cstdint>
#include <vector>

#include "capture/ICapturer.h"
#include "rfb/PixelFormat.h"

namespace rfbenc {

constexpr int32_t kRaw = 0, kCopyRect = 1, kTight = 7, kZrle = 16;

// Read-only view of the server framebuffer (tightly packed BGRA).
struct FrameView {
    const uint8_t* bgra = nullptr;
    int width = 0, height = 0;
    const uint8_t* at(int x, int y) const { return bgra + (static_cast<size_t>(y) * width + x) * 4; }
};

inline void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(x >> 8); v.push_back(x & 0xFF); }
inline void put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 3; i >= 0; --i) v.push_back(static_cast<uint8_t>(x >> (i * 8)));
}
inline void putRectHeader(std::vector<uint8_t>& v, int x, int y, int w, int h, int32_t enc) {
    put16(v, static_cast<uint16_t>(x)); put16(v, static_cast<uint16_t>(y));
    put16(v, static_cast<uint16_t>(w)); put16(v, static_cast<uint16_t>(h));
    put32(v, static_cast<uint32_t>(enc));
}

// Encoders append complete rectangles (header + payload) and return how many rectangles they wrote.
// They keep per-connection compression state, so one instance serves exactly one client, strictly in order.
class RectEncoder {
public:
    virtual ~RectEncoder() = default;
    virtual int encode(const FrameView& fb, const Rect& r, std::vector<uint8_t>& out) = 0;
};

}  // namespace rfbenc
