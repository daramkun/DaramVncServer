#include "encode/TightEncoder.h"

#include <turbojpeg.h>

#include <algorithm>
#include <cassert>
#include <future>
#include <thread>

#include "encode/ColorSet.h"
#include "encode/TileClass.h"

namespace rfbenc {

namespace {

constexpr int kTile = 64;
constexpr int kMaxTightWidth = 2048;
constexpr size_t kMinZlibSize = 12;

void putRgb(std::vector<uint8_t>& out, uint32_t rgb) {
    out.push_back(static_cast<uint8_t>(rgb >> 16));
    out.push_back(static_cast<uint8_t>(rgb >> 8));
    out.push_back(static_cast<uint8_t>(rgb));
}

int jpegSubsampling(int quality) { return quality >= 80 ? TJSAMP_444 : TJSAMP_420; }

void compressJpeg(const FrameView& fb, int x, int y, int w, int h, int quality, std::vector<uint8_t>& out) {
    thread_local tjhandle handle = tjInitCompress();
    const int samp = jpegSubsampling(quality);
    out.resize(tjBufSize(w, h, samp));
    unsigned char* buf = out.data();
    unsigned long size = out.size();
    if (tjCompress2(handle, fb.at(x, y), w, fb.width * 4, h, TJPF_BGRX, &buf, &size, samp, quality,
                    TJFLAG_FASTDCT | TJFLAG_NOREALLOC) != 0)
        size = 0;
    out.resize(size);
}

}  // namespace

void putCompactLength(std::vector<uint8_t>& out, size_t len) {
    if (len < 128) {
        out.push_back(static_cast<uint8_t>(len));
    } else if (len < 16384) {
        out.push_back(static_cast<uint8_t>(len & 0x7F) | 0x80);
        out.push_back(static_cast<uint8_t>(len >> 7));
    } else {
        out.push_back(static_cast<uint8_t>(len & 0x7F) | 0x80);
        out.push_back(static_cast<uint8_t>((len >> 7) & 0x7F) | 0x80);
        out.push_back(static_cast<uint8_t>(len >> 14));
    }
}

int jpegQualityForLevel(int level) {
    static const int table[10] = {15, 29, 41, 42, 62, 77, 79, 86, 92, 100};
    return table[std::clamp(level, 0, 9)];
}

TightEncoder::TightEncoder(const PixelFormat& pf, int jpegQuality, int zlibLevel)
    : pf_(pf), jpegQuality_(jpegQuality), zlibLevel_(zlibLevel) {
    assert(pf.tightPixel24());
}

TightEncoder::~TightEncoder() {
    for (size_t i = 0; i < zs_.size(); ++i)
        if (zInit_[i]) deflateEnd(&zs_[i]);
}

void TightEncoder::deflateRect(int stream, uint8_t ctrl, const std::vector<uint8_t>& prefix,
                               const std::vector<uint8_t>& data, std::vector<uint8_t>& out) {
    out.push_back(ctrl);
    out.insert(out.end(), prefix.begin(), prefix.end());
    if (data.size() < kMinZlibSize) {
        out.insert(out.end(), data.begin(), data.end());
        return;
    }
    z_stream& z = zs_[stream];
    const int strategy = stream == 0 ? Z_DEFAULT_STRATEGY : Z_FILTERED;
    if (!zInit_[stream]) {
        deflateInit2(&z, zlibLevel_, Z_DEFLATED, 15, 8, strategy);
        zInit_[stream] = true;
        zLevel_[stream] = zlibLevel_;
    } else if (zLevel_[stream] != zlibLevel_) {
        deflateParams(&z, zlibLevel_, strategy);
        zLevel_[stream] = zlibLevel_;
    }
    zbuf_.resize(deflateBound(&z, static_cast<uLong>(data.size())) + 64);
    z.next_in = const_cast<Bytef*>(data.data());
    z.avail_in = static_cast<uInt>(data.size());
    z.next_out = zbuf_.data();
    z.avail_out = static_cast<uInt>(zbuf_.size());
    deflate(&z, Z_SYNC_FLUSH);
    const size_t produced = zbuf_.size() - z.avail_out;
    putCompactLength(out, produced);
    out.insert(out.end(), zbuf_.begin(), zbuf_.begin() + produced);
}

void TightEncoder::encodeBasic(const FrameView& fb, const Run& r, std::vector<uint8_t>& out) {
    ColorSet cs(ColorSet::kMax);
    for (int j = 0; j < r.h && !cs.overflow(); ++j) {
        const uint8_t* p = fb.at(r.x, r.y + j);
        for (int i = 0; i < r.w; ++i)
            if (cs.add(pixelRgb(p + i * 4)) < 0) break;
    }

    std::vector<uint8_t> data, prefix;
    if (!cs.overflow() && cs.size() == 1) {  // merged tiles turned out uniform
        out.push_back(0x80);
        putRgb(out, cs.color(0));
        return;
    }
    if (cs.overflow()) {  // full colour, copy filter, stream 0
        data.reserve(static_cast<size_t>(r.w) * r.h * 3);
        for (int j = 0; j < r.h; ++j) {
            const uint8_t* p = fb.at(r.x, r.y + j);
            for (int i = 0; i < r.w; ++i) putRgb(data, pixelRgb(p + i * 4));
        }
        deflateRect(0, 0x00, prefix, data, out);
        return;
    }

    prefix.push_back(1);  // PaletteFilter
    prefix.push_back(static_cast<uint8_t>(cs.size() - 1));
    for (int i = 0; i < cs.size(); ++i) putRgb(prefix, cs.color(i));

    if (cs.size() == 2) {  // 1 bit per pixel, MSB = leftmost, rows byte-aligned
        const int rowBytes = (r.w + 7) / 8;
        data.assign(static_cast<size_t>(rowBytes) * r.h, 0);
        for (int j = 0; j < r.h; ++j) {
            const uint8_t* p = fb.at(r.x, r.y + j);
            for (int i = 0; i < r.w; ++i)
                if (pixelRgb(p + i * 4) == cs.color(1)) data[static_cast<size_t>(j) * rowBytes + i / 8] |= 0x80 >> (i % 8);
        }
        deflateRect(1, 0x40 | (1 << 4), prefix, data, out);
    } else {
        data.reserve(static_cast<size_t>(r.w) * r.h);
        for (int j = 0; j < r.h; ++j) {
            const uint8_t* p = fb.at(r.x, r.y + j);
            for (int i = 0; i < r.w; ++i) data.push_back(static_cast<uint8_t>(cs.add(pixelRgb(p + i * 4))));
        }
        deflateRect(2, 0x40 | (2 << 4), prefix, data, out);
    }
}

int TightEncoder::encode(const FrameView& fb, const Rect& rect, std::vector<uint8_t>& out) {
    lastLossy_ = false;
    const bool allowJpeg = !lossless_ && jpegQuality_ >= 1 && jpegQuality_ < 100;

    // 1. Plan: classify tiles per 64-row band and merge neighbours into runs.
    std::vector<Run> runs;
    for (int by = rect.y; by < rect.y + rect.h; by += kTile) {
        const int bh = std::min(kTile, rect.y + rect.h - by);
        const size_t bandStart = runs.size();
        for (int bx = rect.x; bx < rect.x + rect.w; bx += kTile) {
            const int bw = std::min(kTile, rect.x + rect.w - bx);
            TileClass tc = classifyTile(fb, bx, by, bw, bh);
            Kind kind = tc.kind == 0 ? Kind::Solid : (tc.kind == 1 || !allowJpeg) ? Kind::Flat : Kind::Photo;
            if (tc.kind == 2 && !allowJpeg) kind = Kind::Flat;  // encodeBasic falls back to full-colour zlib
            if (runs.size() > bandStart) {
                Run& last = runs.back();
                if (last.kind == kind && (kind != Kind::Solid || last.color == tc.color) &&
                    last.w + bw <= kMaxTightWidth && last.x + last.w == bx) {
                    last.w += bw;
                    continue;
                }
            }
            runs.push_back({bx, by, bw, bh, kind, tc.color, {}});
        }
        // Merge solid runs vertically with the previous band.
        if (bandStart > 0) {
            for (size_t i = bandStart; i < runs.size();) {
                bool merged = false;
                if (runs[i].kind == Kind::Solid) {
                    for (size_t j = 0; j < bandStart; ++j) {
                        Run& up = runs[j];
                        if (up.kind == Kind::Solid && up.color == runs[i].color && up.x == runs[i].x &&
                            up.w == runs[i].w && up.y + up.h == runs[i].y) {
                            up.h += runs[i].h;
                            runs.erase(runs.begin() + static_cast<std::ptrdiff_t>(i));
                            merged = true;
                            break;
                        }
                    }
                }
                if (!merged) ++i;
            }
        }
    }

    // 2. JPEG-compress photo runs in parallel (zlib state is sequential, JPEG is stateless).
    std::vector<Run*> photo;
    for (Run& r : runs)
        if (r.kind == Kind::Photo) photo.push_back(&r);
    if (!photo.empty()) {
        lastLossy_ = true;
        const int q = jpegQuality_;
        const size_t workers = std::min<size_t>(photo.size(), std::max(1u, std::min(4u, std::thread::hardware_concurrency())));
        std::vector<std::future<void>> jobs;
        for (size_t t = 1; t < workers; ++t)
            jobs.push_back(std::async(std::launch::async, [&, t] {
                for (size_t i = t; i < photo.size(); i += workers)
                    compressJpeg(fb, photo[i]->x, photo[i]->y, photo[i]->w, photo[i]->h, q, photo[i]->jpeg);
            }));
        for (size_t i = 0; i < photo.size(); i += workers)
            compressJpeg(fb, photo[i]->x, photo[i]->y, photo[i]->w, photo[i]->h, q, photo[i]->jpeg);
        for (auto& j : jobs) j.get();
    }

    // 3. Emit in order.
    for (const Run& r : runs) {
        putRectHeader(out, r.x, r.y, r.w, r.h, kTight);
        if (r.kind == Kind::Solid) {
            out.push_back(0x80);
            putRgb(out, r.color);
        } else if (r.kind == Kind::Photo && !r.jpeg.empty()) {
            out.push_back(0x90);
            putCompactLength(out, r.jpeg.size());
            out.insert(out.end(), r.jpeg.begin(), r.jpeg.end());
        } else {
            encodeBasic(fb, r, out);
        }
    }
    return static_cast<int>(runs.size());
}

}  // namespace rfbenc
