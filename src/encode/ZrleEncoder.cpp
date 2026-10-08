#include "encode/ZrleEncoder.h"

#include <zlib-ng.h>

#include <algorithm>

#include "encode/ColorSet.h"

namespace rfbenc {

namespace {

constexpr int kTile = 64;

struct PixRun {
    uint32_t value;
    int len;
};

int runLengthBytes(int len) { return (len - 1) / 255 + 1; }

void putRunLength(std::vector<uint8_t>& out, int len) {
    int rem = len - 1;
    while (rem >= 255) { out.push_back(255); rem -= 255; }
    out.push_back(static_cast<uint8_t>(rem));
}

}  // namespace

struct ZrleEncoder::Zng {
    zng_stream z{};
};

ZrleEncoder::ZrleEncoder(const PixelFormat& pf, int zlibLevel) : z_(std::make_unique<Zng>()) {
    zng_deflateInit(&z_->z, zlibLevel);
    setPixelFormat(pf);
}

void ZrleEncoder::setPixelFormat(const PixelFormat& pf) {
    pf_ = pf;
    // CPIXEL: 3 bytes when a 32bpp format only uses the low or high 3 bytes of the pixel.
    cpixelBytes_ = pf.bpp / 8;
    cpixelOffset_ = 0;
    if (pf.bpp == 32) {
        const uint32_t used = (static_cast<uint32_t>(pf.rMax) << pf.rShift) | (static_cast<uint32_t>(pf.gMax) << pf.gShift) |
                              (static_cast<uint32_t>(pf.bMax) << pf.bShift);
        const bool low = (used >> 24) == 0, high = (used & 0xFF) == 0;
        if (low || high) {
            cpixelBytes_ = 3;
            // Wire order is little- or big-endian; pick the matching 3-byte slice (low preferred).
            cpixelOffset_ = (low != static_cast<bool>(pf.bigEndian)) ? 0 : 1;
        }
    }
}

ZrleEncoder::~ZrleEncoder() { zng_deflateEnd(&z_->z); }

void ZrleEncoder::appendCPixel(std::vector<uint8_t>& out, uint32_t v) const {
    uint8_t tmp[4];
    for (int k = 0; k < 4; ++k) tmp[k] = static_cast<uint8_t>(v >> (pf_.bigEndian ? (3 - k) * 8 : k * 8));
    if (pf_.bpp == 32) {
        out.insert(out.end(), tmp + cpixelOffset_, tmp + cpixelOffset_ + cpixelBytes_);
    } else {
        pf_.appendPixel(out, v);
    }
}

void ZrleEncoder::encodeTile(const FrameView& fb, int x, int y, int w, int h, std::vector<uint8_t>& out) {
    const int n = w * h;
    std::vector<uint32_t> px(static_cast<size_t>(n));
    for (int j = 0; j < h; ++j) {
        const uint8_t* p = fb.at(x, y + j);
        for (int i = 0; i < w; ++i) px[static_cast<size_t>(j) * w + i] = pf_.pack(p[i * 4 + 2], p[i * 4 + 1], p[i * 4]);
    }

    std::vector<PixRun> runs;
    for (uint32_t v : px) {
        if (!runs.empty() && runs.back().value == v) ++runs.back().len;
        else runs.push_back({v, 1});
    }
    if (runs.size() == 1) {
        out.push_back(1);
        appendCPixel(out, runs[0].value);
        return;
    }

    ColorSet cs(128);
    for (const PixRun& r : runs)
        if (cs.add(r.value) < 0) break;
    const int p = cs.overflow() ? 0 : cs.size();
    const int cp = cpixelBytes_;

    size_t rawSize = 1 + static_cast<size_t>(n) * cp;
    size_t rleSize = 1;
    size_t palRleSize = SIZE_MAX, packedSize = SIZE_MAX;
    for (const PixRun& r : runs) rleSize += cp + runLengthBytes(r.len);
    if (p > 0) {
        palRleSize = 1 + static_cast<size_t>(p) * cp;
        for (const PixRun& r : runs) palRleSize += 1 + (r.len > 1 ? runLengthBytes(r.len) : 0);
        if (p <= 16) {
            const int bits = p == 2 ? 1 : p <= 4 ? 2 : 4;
            const int rowBytes = (w * bits + 7) / 8;
            packedSize = 1 + static_cast<size_t>(p) * cp + static_cast<size_t>(rowBytes) * h;
        }
    }

    const size_t best = std::min({rawSize, rleSize, palRleSize, packedSize});
    if (best == packedSize) {
        out.push_back(static_cast<uint8_t>(p));
        for (int i = 0; i < p; ++i) appendCPixel(out, cs.color(i));
        const int bits = p == 2 ? 1 : p <= 4 ? 2 : 4;
        for (int j = 0; j < h; ++j) {
            uint8_t acc = 0;
            int used = 0;
            for (int i = 0; i < w; ++i) {
                acc = static_cast<uint8_t>(acc << bits | cs.add(px[static_cast<size_t>(j) * w + i]));
                used += bits;
                if (used == 8) { out.push_back(acc); acc = 0; used = 0; }
            }
            if (used) out.push_back(static_cast<uint8_t>(acc << (8 - used)));
        }
    } else if (best == palRleSize) {
        out.push_back(static_cast<uint8_t>(128 + p));
        for (int i = 0; i < p; ++i) appendCPixel(out, cs.color(i));
        for (const PixRun& r : runs) {
            const int idx = cs.add(r.value);
            if (r.len == 1) {
                out.push_back(static_cast<uint8_t>(idx));
            } else {
                out.push_back(static_cast<uint8_t>(idx | 128));
                putRunLength(out, r.len);
            }
        }
    } else if (best == rleSize) {
        out.push_back(128);
        for (const PixRun& r : runs) {
            appendCPixel(out, r.value);
            putRunLength(out, r.len);
        }
    } else {
        out.push_back(0);
        for (uint32_t v : px) appendCPixel(out, v);
    }
}

int ZrleEncoder::encode(const FrameView& fb, const Rect& r, std::vector<uint8_t>& out) {
    raw_.clear();
    for (int ty = r.y; ty < r.y + r.h; ty += kTile)
        for (int tx = r.x; tx < r.x + r.w; tx += kTile)
            encodeTile(fb, tx, ty, std::min(kTile, r.x + r.w - tx), std::min(kTile, r.y + r.h - ty), raw_);

    zng_stream& z = z_->z;
    const size_t bound = zng_deflateBound(&z, raw_.size()) + 64;
    if (zbuf_.size() < bound) zbuf_.resize(bound);
    z.next_in = raw_.data();
    z.avail_in = static_cast<uint32_t>(raw_.size());
    z.next_out = zbuf_.data();
    z.avail_out = static_cast<uint32_t>(zbuf_.size());
    zng_deflate(&z, Z_SYNC_FLUSH);
    const size_t produced = zbuf_.size() - z.avail_out;

    putRectHeader(out, r.x, r.y, r.w, r.h, kZrle);
    put32(out, static_cast<uint32_t>(produced));
    out.insert(out.end(), zbuf_.begin(), zbuf_.begin() + produced);
    return 1;
}

}  // namespace rfbenc
