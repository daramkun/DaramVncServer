#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

struct PixelFormat {
    uint8_t bpp = 32, depth = 24, bigEndian = 0, trueColor = 1;
    uint16_t rMax = 255, gMax = 255, bMax = 255;
    uint8_t rShift = 16, gShift = 8, bShift = 0;

    // Server framebuffer layout (BGRA in memory) == little-endian 0x00RRGGBB.
    bool isNative32() const {
        return bpp == 32 && !bigEndian && trueColor && rMax == 255 && gMax == 255 && bMax == 255 && rShift == 16 &&
               gShift == 8 && bShift == 0;
    }
    // True when Tight's 3-byte TPIXEL (R,G,B) applies.
    bool tightPixel24() const {
        return bpp == 32 && depth == 24 && trueColor && rMax == 255 && gMax == 255 && bMax == 255;
    }
    int bytesPerPixel() const { return bpp / 8; }

    // 8bpp colour-mapped mode: the server defines the palette (RGB332, see rgb332Palette()) and sends pixel indices.
    bool colorMapped() const { return !trueColor && bpp == 8; }
    static uint8_t rgb332Index(uint8_t r, uint8_t g, uint8_t b) { return static_cast<uint8_t>((r >> 5) << 5 | (g >> 5) << 2 | (b >> 6)); }
    static void rgb332Entry(int i, uint8_t& r, uint8_t& g, uint8_t& b) {
        r = static_cast<uint8_t>(((i >> 5) & 7) * 255 / 7);
        g = static_cast<uint8_t>(((i >> 2) & 7) * 255 / 7);
        b = static_cast<uint8_t>((i & 3) * 255 / 3);
    }

    // Rejects client-supplied formats that would make shifts/masks undefined or overlap.
    bool valid() const {
        if (colorMapped()) return true;
        if (!trueColor || (bpp != 8 && bpp != 16 && bpp != 32)) return false;
        const uint16_t maxes[3] = {rMax, gMax, bMax};
        const uint8_t shifts[3] = {rShift, gShift, bShift};
        uint32_t used = 0;
        for (int i = 0; i < 3; ++i) {
            const uint32_t m = maxes[i];
            if (m == 0 || (m & (m + 1)) != 0) return false;  // must be 2^n - 1
            int bits = 0;
            while ((m >> bits) != 0) ++bits;
            if (shifts[i] + bits > bpp) return false;
            const uint32_t mask = (bits >= 32 ? 0xFFFFFFFFu : ((1u << bits) - 1)) << shifts[i];
            if (used & mask) return false;
            used |= mask;
        }
        return true;
    }

    uint32_t pack(uint8_t r, uint8_t g, uint8_t b) const {
        if (!trueColor) return rgb332Index(r, g, b);
        if (rMax == 255 && gMax == 255 && bMax == 255)
            return static_cast<uint32_t>(r) << rShift | static_cast<uint32_t>(g) << gShift | static_cast<uint32_t>(b) << bShift;
        return (static_cast<uint32_t>(r) * rMax / 255 << rShift) | (static_cast<uint32_t>(g) * gMax / 255 << gShift) |
               (static_cast<uint32_t>(b) * bMax / 255 << bShift);
    }
    void appendPixel(std::vector<uint8_t>& out, uint32_t v) const {
        const int n = bpp / 8;
        for (int k = 0; k < n; ++k) out.push_back(static_cast<uint8_t>(v >> (bigEndian ? (n - 1 - k) * 8 : k * 8)));
    }
    // Converts `count` BGRA pixels into this format and appends them.
    void convertRow(const uint8_t* bgra, int count, std::vector<uint8_t>& out) const {
        if (isNative32()) {
            out.insert(out.end(), bgra, bgra + static_cast<size_t>(count) * 4);
            return;
        }
        for (int i = 0; i < count; ++i) appendPixel(out, pack(bgra[i * 4 + 2], bgra[i * 4 + 1], bgra[i * 4]));
    }
};
