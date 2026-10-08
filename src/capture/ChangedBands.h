#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "capture/ICapturer.h"

// Applications (and DXGI itself) often report regions as dirty that were repainted with identical pixels
// (caret blinks, whole-window invalidations). Comparing the new pixels against what we already hold shrinks a
// reported rectangle to the bands that really differ, so no client spends encode time or bandwidth on them.
//
// `oldBase`/`newBase` point at pixel (0,0) of two BGRA images with the given row strides in bytes.
inline void changedBands(const uint8_t* oldBase, size_t oldStride, const uint8_t* newBase, size_t newStride, const Rect& r,
                         std::vector<Rect>& out) {
    constexpr int kBand = 16;
    const size_t rowBytes = static_cast<size_t>(r.w) * 4;
    Rect cur;
    auto flush = [&] {
        if (cur.w > 0) out.push_back(cur);
        cur = Rect{};
    };
    for (int by = r.y; by < r.y + r.h; by += kBand) {
        const int bh = std::min(kBand, r.y + r.h - by);
        int minx = r.w, maxx = -1;
        for (int y = by; y < by + bh; ++y) {
            const uint32_t* a = reinterpret_cast<const uint32_t*>(oldBase + static_cast<size_t>(y) * oldStride + static_cast<size_t>(r.x) * 4);
            const uint32_t* b = reinterpret_cast<const uint32_t*>(newBase + static_cast<size_t>(y) * newStride + static_cast<size_t>(r.x) * 4);
            if (std::memcmp(a, b, rowBytes) == 0) continue;
            int i = 0;
            while (i < r.w && a[i] == b[i]) ++i;
            int j = r.w - 1;
            while (j > i && a[j] == b[j]) --j;
            minx = std::min(minx, i);
            maxx = std::max(maxx, j);
        }
        if (maxx < 0) { flush(); continue; }
        const Rect band{r.x + minx, by, maxx - minx + 1, bh};
        const bool touches = cur.w > 0 && cur.y + cur.h == by && band.x <= cur.x + cur.w && cur.x <= band.x + band.w;
        if (touches) {
            const int x1 = std::min(cur.x, band.x), x2 = std::max(cur.x + cur.w, band.x + band.w);
            cur = {x1, cur.y, x2 - x1, cur.h + bh};
        } else {
            flush();
            cur = band;
        }
    }
    flush();
}
