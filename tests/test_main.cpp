// Encoder round-trip tests against the independent reference decoders in RefDecoders.h.
#include <cmath>
#include <cstdio>
#include <random>

#include "RefDecoders.h"
#include "encode/RawEncoder.h"
#include "encode/TightEncoder.h"
#include "encode/ZrleEncoder.h"
#include "RefH264Decoder.h"
#include "encode/H264Encoder.h"
#include "config/Config.h"
#include "encode/QualityController.h"
#include "security/Auth.h"

using namespace rfbenc;
using ref::Image;
using ref::Reader;

namespace {

int g_failed = 0;
#define CHECK(cond)                                                           \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);     \
            ++g_failed;                                                       \
        }                                                                     \
    } while (0)

FrameView view(const Image& i) { return {i.px.data(), i.w, i.h}; }

Image makeImage(int w, int h, int kind, uint32_t seed) {
    Image img(w, h);
    std::mt19937 rng(seed);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        uint8_t* p = img.at(x, y);
        uint8_t r = 255, g = 255, b = 255;
        switch (kind) {
            case 0: break;                                                                     // solid white
            case 1: if (((x / 3) ^ (y / 5)) & 1) r = g = b = 20; break;                        // two colours
            case 2: r = static_cast<uint8_t>(x * 255 / w); g = static_cast<uint8_t>(y * 255 / h);
                    b = static_cast<uint8_t>((x + y) * 255 / (w + h)); break;                  // smooth gradient
            case 3: r = static_cast<uint8_t>(rng()); g = static_cast<uint8_t>(rng()); b = static_cast<uint8_t>(rng()); break;  // noise
            case 4: { int q = (x / 16 + y / 16) % 12; r = static_cast<uint8_t>(q * 20); g = static_cast<uint8_t>(255 - q * 9);
                      b = static_cast<uint8_t>(q * 7); break; }                                // 12-colour palette
            default: if (x < w / 2) { if ((x + y) % 7 == 0) r = g = b = 0; }
                     else { r = static_cast<uint8_t>(x * 3 + y); g = static_cast<uint8_t>(x ^ y); b = static_cast<uint8_t>(x * y); } break;  // mixed
        }
        p[0] = b; p[1] = g; p[2] = r; p[3] = 0;
    }
    return img;
}

bool equalInRect(const Image& a, const Image& b, int x0, int y0, int w, int h) {
    for (int y = y0; y < y0 + h; ++y) for (int x = x0; x < x0 + w; ++x) for (int c = 0; c < 3; ++c)
        if (a.at(x, y)[c] != b.at(x, y)[c]) return false;
    return true;
}

double psnr(const Image& a, const Image& b) {
    double se = 0;
    for (int i = 0; i < a.w * a.h; ++i) for (int c = 0; c < 3; ++c) {
        double d = double(a.px[static_cast<size_t>(i) * 4 + c]) - b.px[static_cast<size_t>(i) * 4 + c];
        se += d * d;
    }
    se /= a.w * a.h * 3.0;
    return se == 0 ? 99 : 10 * std::log10(255.0 * 255.0 / se);
}

// Decodes `nRects` rectangles from wire (each with its own 12-byte header) using Tight.
void decodeTightWire(ref::TightDecoder& dec, const std::vector<uint8_t>& wire, int nRects, Image& img) {
    Reader r(wire);
    for (int i = 0; i < nRects; ++i) {
        int x = r.u16(), y = r.u16(), w = r.u16(), h = r.u16();
        if (static_cast<int32_t>(r.u32()) != kTight) throw std::runtime_error("not a tight rect");
        dec.decodeRect(r, x, y, w, h, img);
    }
    if (r.pos != wire.size()) throw std::runtime_error("trailing bytes");
}

void decodeZrleWire(ref::ZrleDecoder& dec, const std::vector<uint8_t>& wire, int nRects, const PixelFormat& pf, Image& img) {
    Reader r(wire);
    for (int i = 0; i < nRects; ++i) {
        int x = r.u16(), y = r.u16(), w = r.u16(), h = r.u16();
        if (static_cast<int32_t>(r.u32()) != kZrle) throw std::runtime_error("not a zrle rect");
        dec.decodeRect(r, x, y, w, h, pf, img);
    }
    if (r.pos != wire.size()) throw std::runtime_error("trailing bytes");
}

void testTightLossless() {
    std::printf("tight lossless, every image class, one persistent encoder/decoder pair\n");
    PixelFormat pf;
    TightEncoder enc(pf, -1, 3);
    ref::TightDecoder dec;
    Image dst(301, 197);
    for (int kind = 0; kind <= 5; ++kind) {
        Image src = makeImage(301, 197, kind, 7 + kind);
        std::vector<uint8_t> wire;
        int n = enc.encode(view(src), {0, 0, src.w, src.h}, wire);
        try { decodeTightWire(dec, wire, n, dst); } catch (const std::exception& ex) { std::printf("  kind %d: %s\n", kind, ex.what()); ++g_failed; continue; }
        CHECK(equalInRect(src, dst, 0, 0, src.w, src.h));
        if (kind == 0) CHECK(n <= 5 && wire.size() < 100);  // solid screen must collapse into a few fills
    }
}

void testTightJpeg() {
    std::printf("tight: JPEG for photo-like tiles, lossless refinement afterwards\n");
    PixelFormat pf;
    TightEncoder enc(pf, 90, 3);
    ref::TightDecoder dec;
    Image src = makeImage(256, 128, 2, 1);
    Image dst(256, 128);
    std::vector<uint8_t> wire;
    int n = enc.encode(view(src), {0, 0, src.w, src.h}, wire);
    CHECK(enc.lastEncodeHadLossy());
    try { decodeTightWire(dec, wire, n, dst); } catch (const std::exception& ex) { std::printf("  %s\n", ex.what()); ++g_failed; return; }
    CHECK(dec.jpegRects > 0);
    double p = psnr(src, dst);
    std::printf("  PSNR %.1f dB, %zu bytes on the wire (raw %zu)\n", p, wire.size(), src.px.size());
    CHECK(p > 38.0);
    CHECK(wire.size() < src.px.size() / 4);

    enc.setLossless(true);
    wire.clear();
    n = enc.encode(view(src), {0, 0, src.w, src.h}, wire);
    CHECK(!enc.lastEncodeHadLossy());
    try { decodeTightWire(dec, wire, n, dst); } catch (const std::exception& ex) { std::printf("  %s\n", ex.what()); ++g_failed; return; }
    CHECK(equalInRect(src, dst, 0, 0, src.w, src.h));
}

void testTightSubrect() {
    std::printf("tight: offset sub-rectangle, wide rect split at 2048\n");
    PixelFormat pf;
    TightEncoder enc(pf, -1, 3);
    ref::TightDecoder dec;
    Image src = makeImage(400, 300, 5, 3), dst(400, 300);
    std::vector<uint8_t> wire;
    int n = enc.encode(view(src), {37, 41, 211, 133}, wire);
    try { decodeTightWire(dec, wire, n, dst); } catch (const std::exception& ex) { std::printf("  %s\n", ex.what()); ++g_failed; return; }
    CHECK(equalInRect(src, dst, 37, 41, 211, 133));

    Image wideSrc = makeImage(4200, 70, 4, 9), wideDst(4200, 70);
    wire.clear();
    n = enc.encode(view(wideSrc), {0, 0, 4200, 70}, wire);
    try { decodeTightWire(dec, wire, n, wideDst); } catch (const std::exception& ex) { std::printf("  %s\n", ex.what()); ++g_failed; return; }
    CHECK(equalInRect(wideSrc, wideDst, 0, 0, 4200, 70));
    Reader r(wire);
    for (int i = 0; i < n; ++i) {  // no rect may exceed 2048 px
        r.u16(); r.u16(); int w = r.u16(); r.u16(); r.u32();
        CHECK(w <= 2048);
        break;
    }
}

void testZrle(const char* name, PixelFormat pf, bool exact) {
    std::printf("zrle %s\n", name);
    ZrleEncoder enc(pf, 3);
    ref::ZrleDecoder dec;
    Image dst(150, 90);
    for (int kind = 0; kind <= 5; ++kind) {  // several frames through the same zlib stream
        Image src = makeImage(150, 90, kind, 11 + kind);
        std::vector<uint8_t> wire;
        int n = enc.encode(view(src), {0, 0, src.w, src.h}, wire);
        try { decodeZrleWire(dec, wire, n, pf, dst); } catch (const std::exception& ex) { std::printf("  kind %d: %s\n", kind, ex.what()); ++g_failed; return; }
        if (exact) CHECK(equalInRect(src, dst, 0, 0, src.w, src.h));
        else CHECK(psnr(src, dst) > 20.0);
    }
}

void testZrleFormatSwitch() {
    std::printf("zrle: pixel format change keeps the zlib stream in sync\n");
    PixelFormat a, b;
    b.bpp = 16; b.depth = 16; b.rMax = 31; b.gMax = 63; b.bMax = 31; b.rShift = 11; b.gShift = 5; b.bShift = 0;
    ZrleEncoder enc(a, 3);
    ref::ZrleDecoder dec;
    Image dst(100, 60), src = makeImage(100, 60, 5, 5);
    std::vector<uint8_t> wire;
    int n = enc.encode(view(src), {0, 0, 100, 60}, wire);
    try { decodeZrleWire(dec, wire, n, a, dst); } catch (const std::exception& ex) { std::printf("  %s\n", ex.what()); ++g_failed; return; }
    CHECK(equalInRect(src, dst, 0, 0, 100, 60));
    enc.setPixelFormat(b);
    wire.clear();
    n = enc.encode(view(src), {0, 0, 100, 60}, wire);
    try { decodeZrleWire(dec, wire, n, b, dst); } catch (const std::exception& ex) { std::printf("  %s\n", ex.what()); ++g_failed; return; }
    CHECK(psnr(src, dst) > 20.0);
}

void testRaw() {
    std::printf("raw encoder\n");
    PixelFormat pf;
    RawEncoder enc(pf);
    Image src = makeImage(20, 10, 3, 2);
    std::vector<uint8_t> wire;
    CHECK(enc.encode(view(src), {5, 2, 7, 4}, wire) == 1);
    CHECK(wire.size() == 12 + 7 * 4 * 4);
    CHECK(std::memcmp(&wire[12], src.at(5, 2), 7 * 4) == 0);
}

void testColourMapped() {
    std::printf("8bpp colour-mapped (RGB332 palette) via ZRLE and Raw\n");
    PixelFormat pf;
    pf.bpp = 8; pf.depth = 8; pf.trueColor = 0; pf.rMax = pf.gMax = pf.bMax = 0; pf.rShift = pf.gShift = pf.bShift = 0;
    CHECK(pf.valid() && pf.colorMapped());
    ref::ZrleDecoder dec;
    for (int i = 0; i < 256; ++i) { uint8_t r, g, b; PixelFormat::rgb332Entry(i, r, g, b); dec.palette[i * 3] = b; dec.palette[i * 3 + 1] = g; dec.palette[i * 3 + 2] = r; }
    ZrleEncoder enc(pf, 3);
    RawEncoder raw(pf);
    for (int kind = 1; kind <= 5; ++kind) {
        Image src = makeImage(130, 70, kind, 40 + kind), dst(130, 70), expect(130, 70);
        for (int y = 0; y < 70; ++y) for (int x = 0; x < 130; ++x) {
            const uint8_t* p = src.at(x, y);
            uint8_t r, g, b;
            PixelFormat::rgb332Entry(PixelFormat::rgb332Index(p[2], p[1], p[0]), r, g, b);
            uint8_t* e = expect.at(x, y); e[0] = b; e[1] = g; e[2] = r;
        }
        std::vector<uint8_t> wire;
        int n = enc.encode(view(src), {0, 0, 130, 70}, wire);
        try { decodeZrleWire(dec, wire, n, pf, dst); } catch (const std::exception& ex) { std::printf("  kind %d: %s\n", kind, ex.what()); ++g_failed; continue; }
        CHECK(equalInRect(expect, dst, 0, 0, 130, 70));
        wire.clear();
        raw.encode(view(src), {0, 0, 130, 70}, wire);
        CHECK(wire.size() == 12 + 130 * 70);
        bool ok = true;
        for (int y = 0; y < 70 && ok; ++y) for (int x = 0; x < 130; ++x) {
            const uint8_t* p = src.at(x, y);
            ok &= wire[12 + static_cast<size_t>(y) * 130 + x] == PixelFormat::rgb332Index(p[2], p[1], p[0]);
        }
        CHECK(ok);
    }
}

void testH264() {
    std::puts("h264: Media Foundation encoder -> independent MF decoder (200x120 rect, padded to 208x128)");
    const int W = 200, H = 120;
    auto enc = H264Encoder::create(W, H, 30, 2000);
    if (!enc) { std::puts("  H.264 encoder unavailable on this machine: skipped"); return; }
    ref::H264Decoder dec;
    CHECK(dec.open(enc->codedWidth(), enc->codedHeight()));
    std::vector<Image> sources;
    int decoded = 0;
    double worst = 99;
    auto check = [&](const std::vector<uint8_t>& bgra) {
        if (decoded >= static_cast<int>(sources.size())) { ++g_failed; return; }
        Image out(W, H);
        out.px = bgra;
        const double q = psnr(sources[static_cast<size_t>(decoded)], out);
        std::printf("  frame %d: PSNR %.1f dB", decoded, q);
        std::puts("");
        worst = std::min(worst, q);
        ++decoded;
    };
    const int kFrames = 8;
    for (int f = 0; f < kFrames; ++f) {
        Image src(W, H);
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {  // moving smooth pattern
            uint8_t* p = src.at(x, y);
            p[2] = static_cast<uint8_t>(128 + 100 * std::sin((x + f * 3) / 23.0));
            p[1] = static_cast<uint8_t>(128 + 100 * std::sin((y + f * 2) / 17.0 + x / 50.0));
            p[0] = static_cast<uint8_t>(128 + 100 * std::sin((x + y) / 31.0 - f / 5.0));
            p[3] = 0;
        }
        sources.push_back(src);
        std::vector<uint8_t> stream, bgra;
        const bool keyframe = f == 0 || f == 5;
        const bool ok = enc->encode(view(src), {0, 0, W, H}, keyframe, stream);
        CHECK(ok);
        if (!ok) continue;
        bool sps = false, idr = false;
        for (size_t i = 0; i + 4 < stream.size(); ++i)
            if (stream[i] == 0 && stream[i + 1] == 0 && stream[i + 2] == 1) {
                sps |= (stream[i + 3] & 0x1F) == 7;
                idr |= (stream[i + 3] & 0x1F) == 5;
            }
        if (keyframe) CHECK(sps && idr);  // random access points must carry parameter sets
        else CHECK(!idr);
        if (dec.decode(stream, W, H, bgra)) check(bgra);
    }
    std::vector<uint8_t> bgra;
    while (dec.drain(W, H, bgra)) check(bgra);
    std::printf("  decoded %d of %d frames, worst PSNR %.1f dB\n", decoded, kFrames, worst);
    CHECK(decoded == kFrames);
    CHECK(worst > 30.0);
}

void testQualityController() {
    std::puts("quality controller: down fast on delay, up slowly when delay is gone, always inside [lo, hi]");
    QualityController c;
    double t = 1000;
    CHECK(c.update(t, 5, 40, 85) == 85);  // starts at the upper limit

    // Sustained delay: -10 every 200 ms until the lower limit, then it holds there.
    int q = 85;
    for (int i = 0; i < 20; ++i) { t += 200; q = c.update(t, 150, 40, 85); }
    CHECK(q == 40);
    t += 200;
    CHECK(c.update(t, 150, 40, 85) == 40);

    // Steps are rate limited: a call 50 ms after a step must not move the quality again.
    QualityController r;
    t = 5000;
    r.update(t, 150, 40, 85);
    t += 200; const int afterFirst = r.update(t, 150, 40, 85);
    CHECK(afterFirst == 75);
    t += 50; CHECK(r.update(t, 150, 40, 85) == 75);

    // In between the thresholds the quality neither rises nor falls.
    for (int i = 0; i < 10; ++i) { t += 200; CHECK(r.update(t, 50, 40, 85) == 75); }

    // Good delay: nothing happens for the first 600 ms, then +3 per step, never above the upper limit.
    t += 200; r.update(t, 5, 40, 85);
    t += 200; CHECK(r.update(t, 5, 40, 85) == 75);
    t += 200; CHECK(r.update(t, 5, 40, 85) == 75);
    t += 200; CHECK(r.update(t, 5, 40, 85) == 78);
    for (int i = 0; i < 30; ++i) { t += 200; q = r.update(t, 5, 40, 85); }
    CHECK(q == 85);

    // A single bad sample in the middle of recovery restarts the "good for 600 ms" wait.
    QualityController g;
    t = 9000;
    g.update(t, 150, 40, 85);
    for (int i = 0; i < 4; ++i) { t += 200; g.update(t, 150, 40, 85); }  // 85 -> 45
    CHECK(g.current() == 45);
    t += 200; g.update(t, 5, 40, 85);
    t += 200; g.update(t, 50, 40, 85);  // delay between the limits: resets the good streak
    t += 200; CHECK(g.update(t, 5, 40, 85) == 45);

    // No measurement: hold.
    QualityController h;
    t = 20000;
    h.update(t, 150, 40, 85);
    t += 200; h.update(t, 150, 40, 85);
    const int held = h.current();
    for (int i = 0; i < 10; ++i) { t += 200; CHECK(h.update(t, -1, 40, 85) == held); }

    // Limits change (client picks a lower quality): applied immediately.
    CHECK(h.update(t + 200, -1, 40, 60) == 60 || h.current() <= 60);
    CHECK(h.update(t + 400, -1, 55, 55) == 55);
}

void testConfigNormalize() {
    std::puts("config: jpeg quality limits are normalised");
    ServerConfig c;
    CHECK(c.jpegQualityMax == 85 && c.jpegQualityMin == 40);  // documented defaults
    c.jpegQualityMax = 500; c.jpegQualityMin = -3;
    normalizeConfig(c);
    CHECK(c.jpegQualityMax == 99 && c.jpegQualityMin == 1);
    c.jpegQualityMax = 30; c.jpegQualityMin = 70;
    normalizeConfig(c);
    CHECK(c.jpegQualityMin == 30 && c.jpegQualityMax == 30);  // min can never exceed max
}

void testPixelFormatValidation() {
    std::printf("pixel format validation\n");
    PixelFormat ok;
    CHECK(ok.valid());
    PixelFormat bad = ok; bad.rShift = 40; CHECK(!bad.valid());       // shift beyond the word
    bad = ok; bad.rMax = 100; CHECK(!bad.valid());                    // not 2^n-1
    bad = ok; bad.gShift = 16; CHECK(!bad.valid());                   // overlapping channels
    bad = ok; bad.trueColor = 0; CHECK(!bad.valid());
    bad = ok; bad.bpp = 24; CHECK(!bad.valid());
    PixelFormat p565; p565.bpp = 16; p565.depth = 16; p565.rMax = 31; p565.gMax = 63; p565.bMax = 31; p565.rShift = 11; p565.gShift = 5; p565.bShift = 0;
    CHECK(p565.valid());
    PixelFormat p8; p8.bpp = 8; p8.depth = 8; p8.rMax = 7; p8.gMax = 7; p8.bMax = 3; p8.rShift = 0; p8.gShift = 3; p8.bShift = 6;
    CHECK(p8.valid());
}

void testAuth() {
    std::printf("auth: password hash, DPAPI, VNC challenge\n");
    const std::string h = auth::hashPassword("correct horse battery");
    CHECK(!h.empty());
    CHECK(auth::verifyPassword("correct horse battery", h));
    CHECK(!auth::verifyPassword("correct horse batterz", h));
    CHECK(!auth::verifyPassword("x", "garbage"));
    const std::string sealed = auth::protectSecret("s3cret!!");
    std::string back;
    CHECK(!sealed.empty() && auth::unprotectSecret(sealed, back) && back == "s3cret!!");
    // Known VNC auth vector: password "password", all-zero challenge.
    uint8_t chal[16] = {0}, resp[16];
    CHECK(auth::vncAuthResponse("password", chal, resp));
    uint8_t chal2[16] = {1}, resp2[16];
    CHECK(auth::vncAuthResponse("password", chal2, resp2) && std::memcmp(resp, resp2, 16) != 0);
}

}  // namespace

int main() {
    testTightLossless();
    testTightJpeg();
    testTightSubrect();
    testRaw();

    PixelFormat native;
    testZrle("32bpp native (3-byte CPIXEL)", native, true);
    PixelFormat bigEndian32 = native; bigEndian32.bigEndian = 1;
    testZrle("32bpp big-endian", bigEndian32, true);
    PixelFormat highShift; highShift.rShift = 24; highShift.gShift = 16; highShift.bShift = 8;
    testZrle("32bpp RGBX (top 3 bytes)", highShift, true);
    PixelFormat p565; p565.bpp = 16; p565.depth = 16; p565.rMax = 31; p565.gMax = 63; p565.bMax = 31; p565.rShift = 11; p565.gShift = 5; p565.bShift = 0;
    testZrle("16bpp 565", p565, false);
    testZrleFormatSwitch();
    testColourMapped();
    testH264();
    testQualityController();
    testConfigNormalize();
    testPixelFormatValidation();
    testAuth();

    std::printf(g_failed ? "\n%d check(s) FAILED\n" : "\nall tests passed\n", g_failed);
    return g_failed ? 1 : 0;
}
