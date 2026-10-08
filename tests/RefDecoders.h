// Reference RFB decoders written from the spec (not from the encoder code) with persistent zlib state, used by
// the unit tests and by the integration client.
#pragma once
#include <turbojpeg.h>
#include <zlib.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "rfb/PixelFormat.h"

namespace ref {

struct Reader {
    const uint8_t* d;
    size_t n, pos = 0;
    Reader(const uint8_t* data, size_t size) : d(data), n(size) {}
    explicit Reader(const std::vector<uint8_t>& v) : d(v.data()), n(v.size()) {}
    uint8_t u8() { if (pos >= n) throw std::runtime_error("eof"); return d[pos++]; }
    uint16_t u16() { uint16_t a = u8(); return static_cast<uint16_t>(a << 8 | u8()); }
    uint32_t u32() { uint32_t a = u16(); return a << 16 | u16(); }
    const uint8_t* take(size_t k) { if (pos + k > n) throw std::runtime_error("eof"); auto* p = d + pos; pos += k; return p; }
    size_t compactLen() {
        size_t v = u8();
        if (v & 0x80) { size_t b = u8(); v = (v & 0x7F) | (b & 0x7F) << 7; if (b & 0x80) v |= static_cast<size_t>(u8()) << 14; }
        return v;
    }
};

// BGRA framebuffer like the server's.
struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> px;
    Image() = default;
    Image(int w_, int h_) : w(w_), h(h_), px(static_cast<size_t>(w_) * h_ * 4, 0) {}
    uint8_t* at(int x, int y) { return &px[(static_cast<size_t>(y) * w + x) * 4]; }
    const uint8_t* at(int x, int y) const { return &px[(static_cast<size_t>(y) * w + x) * 4]; }
};

class Inflater {
public:
    Inflater() { inflateInit(&z_); }
    ~Inflater() { inflateEnd(&z_); }
    Inflater(const Inflater&) = delete;
    Inflater& operator=(const Inflater&) = delete;
    z_stream& raw() { return z_; }
    // Inflates everything produced from `in` (a sync-flushed chunk); `expected` = 0 means "until input is consumed".
    std::vector<uint8_t> run(const uint8_t* in, size_t n, size_t expected) {
        std::vector<uint8_t> out;
        z_.next_in = const_cast<Bytef*>(in);
        z_.avail_in = static_cast<uInt>(n);
        uint8_t chunk[65536];
        for (;;) {
            z_.next_out = chunk;
            z_.avail_out = sizeof chunk;
            int rc = inflate(&z_, Z_SYNC_FLUSH);
            out.insert(out.end(), chunk, chunk + (sizeof chunk - z_.avail_out));
            if (rc != Z_OK && rc != Z_BUF_ERROR) throw std::runtime_error("inflate error");
            if (z_.avail_out != 0) break;
        }
        if (expected && out.size() != expected) throw std::runtime_error("unexpected inflated size");
        return out;
    }

private:
    z_stream z_{};
};

class TightDecoder {
public:
    TightDecoder() : jd_(tjInitDecompress()) {}
    ~TightDecoder() { tjDestroy(jd_); }
    TightDecoder(const TightDecoder&) = delete;
    TightDecoder& operator=(const TightDecoder&) = delete;
    int jpegRects = 0;

    void decodeRect(Reader& r, int x, int y, int w, int h, Image& img) {
        const uint8_t ctrl = r.u8();
        for (int s = 0; s < 4; ++s)
            if (ctrl & (1 << s)) { inflateReset(&streams_[s].raw()); }
        auto put = [&](int px, int py, uint8_t R, uint8_t G, uint8_t B) {
            uint8_t* p = img.at(x + px, y + py);
            p[0] = B; p[1] = G; p[2] = R; p[3] = 0;
        };
        if ((ctrl & 0xF0) == 0x80) {
            const uint8_t* c = r.take(3);
            for (int j = 0; j < h; ++j) for (int k = 0; k < w; ++k) put(k, j, c[0], c[1], c[2]);
        } else if ((ctrl & 0xF0) == 0x90) {
            ++jpegRects;
            size_t len = r.compactLen();
            const uint8_t* jp = r.take(len);
            std::vector<uint8_t> tmp(static_cast<size_t>(w) * h * 4);
            if (tjDecompress2(jd_, jp, static_cast<unsigned long>(len), tmp.data(), w, w * 4, h, TJPF_BGRX, 0) != 0)
                throw std::runtime_error("jpeg decode");
            for (int j = 0; j < h; ++j) for (int k = 0; k < w; ++k) {
                const uint8_t* s = &tmp[(static_cast<size_t>(j) * w + k) * 4];
                put(k, j, s[2], s[1], s[0]);
            }
        } else {
            if (ctrl & 0x80) throw std::runtime_error("bad tight control byte");
            const int stream = (ctrl >> 4) & 3;
            int filter = 0;
            if (ctrl & 0x40) filter = r.u8();
            std::vector<uint8_t> pal;
            int nPal = 0;
            if (filter == 1) { nPal = r.u8() + 1; const uint8_t* p = r.take(static_cast<size_t>(nPal) * 3); pal.assign(p, p + nPal * 3); }
            if (filter == 2) throw std::runtime_error("gradient filter unsupported");
            const size_t expected = filter == 1 ? (nPal == 2 ? static_cast<size_t>((w + 7) / 8) * h : static_cast<size_t>(w) * h)
                                                : static_cast<size_t>(w) * h * 3;
            std::vector<uint8_t> raw;
            if (expected < 12) { const uint8_t* p = r.take(expected); raw.assign(p, p + expected); }
            else { size_t len = r.compactLen(); raw = streams_[stream].run(r.take(len), len, expected); }
            if (filter == 0) {
                for (int j = 0; j < h; ++j) for (int k = 0; k < w; ++k) { const uint8_t* c = &raw[(static_cast<size_t>(j) * w + k) * 3]; put(k, j, c[0], c[1], c[2]); }
            } else if (nPal == 2) {
                const int rowBytes = (w + 7) / 8;
                for (int j = 0; j < h; ++j) for (int k = 0; k < w; ++k) {
                    int idx = (raw[static_cast<size_t>(j) * rowBytes + k / 8] >> (7 - k % 8)) & 1;
                    put(k, j, pal[idx * 3], pal[idx * 3 + 1], pal[idx * 3 + 2]);
                }
            } else {
                for (int j = 0; j < h; ++j) for (int k = 0; k < w; ++k) { int idx = raw[static_cast<size_t>(j) * w + k]; put(k, j, pal.at(idx * 3), pal.at(idx * 3 + 1), pal.at(idx * 3 + 2)); }
            }
        }
    }

private:
    Inflater streams_[4];
    tjhandle jd_;
};

class ZrleDecoder {
public:
    // Palette for colour-mapped (8bpp, !trueColor) formats, as received in SetColourMapEntries: 256 BGR triples.
    std::vector<uint8_t> palette = std::vector<uint8_t>(768, 0);

    void decodeRect(Reader& r, int x, int y, int w, int h, const PixelFormat& pf, Image& img) {
        const uint32_t used = (uint32_t(pf.rMax) << pf.rShift) | (uint32_t(pf.gMax) << pf.gShift) | (uint32_t(pf.bMax) << pf.bShift);
        int cp = pf.bpp / 8;
        bool low3 = true;
        if (pf.bpp == 32 && ((used >> 24) == 0 || (used & 0xFF) == 0)) { cp = 3; low3 = (used >> 24) == 0; }
        const size_t len = r.u32();
        const uint8_t* zp = r.take(len);
        std::vector<uint8_t> raw = inf_.run(zp, len, 0);
        Reader t(raw);
        auto readPixel = [&]() -> uint32_t {
            const uint8_t* p = t.take(static_cast<size_t>(cp));
            uint32_t v = 0;
            if (cp == pf.bpp / 8) {
                for (int k = 0; k < cp; ++k) v |= static_cast<uint32_t>(p[k]) << (pf.bigEndian ? (cp - 1 - k) * 8 : k * 8);
            } else {
                for (int k = 0; k < 3; ++k) v |= static_cast<uint32_t>(p[k]) << ((low3 ? 0 : 8) + (pf.bigEndian ? 2 - k : k) * 8);
            }
            return v;
        };
        auto readRun = [&]() { int n = 1; for (;;) { int b = t.u8(); n += b; if (b != 255) break; } return n; };
        for (int ty = 0; ty < h; ty += 64) for (int tx = 0; tx < w; tx += 64) {
            const int tw = std::min(64, w - tx), th = std::min(64, h - ty);
            const uint8_t sub = t.u8();
            std::vector<uint32_t> px(static_cast<size_t>(tw) * th);
            if (sub == 0) { for (auto& v : px) v = readPixel(); }
            else if (sub == 1) { uint32_t v = readPixel(); for (auto& q : px) q = v; }
            else if (sub <= 16) {
                std::vector<uint32_t> pal(sub); for (auto& v : pal) v = readPixel();
                const int bits = sub == 2 ? 1 : sub <= 4 ? 2 : 4;
                const int rowBytes = (tw * bits + 7) / 8;
                for (int j = 0; j < th; ++j) {
                    const uint8_t* row = t.take(static_cast<size_t>(rowBytes));
                    for (int k = 0; k < tw; ++k) { int bit = k * bits; px[static_cast<size_t>(j) * tw + k] = pal.at((row[bit / 8] >> (8 - bits - bit % 8)) & ((1 << bits) - 1)); }
                }
            } else if (sub == 128) {
                size_t n = 0; while (n < px.size()) { uint32_t v = readPixel(); int l = readRun(); for (int q = 0; q < l; ++q) px.at(n++) = v; }
            } else if (sub >= 130) {
                std::vector<uint32_t> pal(sub - 128); for (auto& v : pal) v = readPixel();
                size_t n = 0; while (n < px.size()) { uint8_t b = t.u8(); int l = (b & 128) ? readRun() : 1; for (int q = 0; q < l; ++q) px.at(n++) = pal.at(b & 127); }
            } else throw std::runtime_error("bad zrle subencoding");
            for (int j = 0; j < th; ++j) for (int k = 0; k < tw; ++k) {
                const uint32_t v = px[static_cast<size_t>(j) * tw + k];
                uint8_t* o = img.at(x + tx + k, y + ty + j);
                if (!pf.trueColor) { const uint8_t* e = &palette.at((v & 255) * 3); o[0] = e[0]; o[1] = e[1]; o[2] = e[2]; o[3] = 0; continue; }
                o[2] = static_cast<uint8_t>(((v >> pf.rShift) & pf.rMax) * 255 / pf.rMax);
                o[1] = static_cast<uint8_t>(((v >> pf.gShift) & pf.gMax) * 255 / pf.gMax);
                o[0] = static_cast<uint8_t>(((v >> pf.bShift) & pf.bMax) * 255 / pf.bMax);
                o[3] = 0;
            }
        }
        if (t.pos != raw.size()) throw std::runtime_error("zrle trailing bytes");
    }

private:
    Inflater inf_;
};

}  // namespace ref
