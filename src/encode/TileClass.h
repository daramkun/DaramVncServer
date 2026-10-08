#pragma once
#include <cstdint>
#include <cstring>

#include "encode/ColorSet.h"
#include "encode/Encoder.h"

namespace rfbenc {

inline uint32_t pixelRgb(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v & 0x00FFFFFF;  // BGRA bytes == 0xAARRGGBB little-endian
}

// Classifies one tile: solid, few colors (<=256, palette/zlib friendly) or photo-like.
struct TileClass {
    int kind;  // 0 solid, 1 flat, 2 photo
    uint32_t color;
};

inline TileClass classifyTile(const FrameView& fb, int x, int y, int w, int h) {
    const uint32_t first = pixelRgb(fb.at(x, y));
    bool solid = true;
    for (int j = 0; j < h && solid; ++j) {
        const uint8_t* p = fb.at(x, y + j);
        for (int i = 0; i < w; ++i)
            if (pixelRgb(p + i * 4) != first) { solid = false; break; }
    }
    if (solid) return {0, first};

    ColorSet cs(ColorSet::kMax);
    for (int j = 0; j < h; ++j) {
        const uint8_t* p = fb.at(x, y + j);
        for (int i = 0; i < w; ++i)
            if (cs.add(pixelRgb(p + i * 4)) < 0) return {2, 0};
    }
    return {1, 0};
}

}  // namespace rfbenc
