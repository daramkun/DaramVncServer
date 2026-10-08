// How much CPU does lowering JPEG quality (or changing other encoder knobs) really save, and what does it cost
// in picture quality? Single-threaded libjpeg-turbo on 1280x720 frames compressed in 192-row stripes, the way
// the server does it.
#include <turbojpeg.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

using clk = std::chrono::steady_clock;

namespace {

constexpr int W = 1280, H = 720, kStripe = 192;

struct Image {
    const char* name;
    std::vector<uint8_t> bgra;
};

// Smooth moving colour field. `noise` adds sensor-like noise (bits), `edges` adds sharp content (text-like
// overlays, object borders) that stresses chroma subsampling.
Image makeImage(const char* name, int noise, bool edges) {
    Image img{name, std::vector<uint8_t>(static_cast<size_t>(W) * H * 4)};
    unsigned seed = 7;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            seed = seed * 1664525u + 1013904223u;
            const int n = noise ? static_cast<int>((seed >> 24) & ((1u << noise) - 1)) : 0;
            double r = 128 + 100 * std::sin(x / 40.0), g = 128 + 100 * std::sin(y / 30.0 + x / 90.0), b = 128 + 100 * std::sin((x + y) / 50.0);
            if (edges && ((x / 24 + y / 24) % 5 == 0)) { r = 230; g = 40; b = 40; }               // saturated blocks: hard chroma edges
            if (edges && (y % 90) < 3) { r = g = b = 250; }                                          // thin bright lines
            uint8_t* p = &img.bgra[(static_cast<size_t>(y) * W + x) * 4];
            p[2] = static_cast<uint8_t>(std::fmin(255, r + n));
            p[1] = static_cast<uint8_t>(std::fmin(255, g + n));
            p[0] = static_cast<uint8_t>(std::fmin(255, b + n));
            p[3] = 0;
        }
    return img;
}

struct Result {
    double ms = 0, kb = 0, psnr = 0;
};

Result run(const Image& img, int quality, int subsamp, int flags, int iters) {
    tjhandle enc = tjInitCompress(), dec = tjInitDecompress();
    std::vector<std::vector<uint8_t>> stripes;
    Result res;
    auto compressAll = [&](bool keep) {
        size_t total = 0;
        if (keep) stripes.clear();
        for (int y = 0; y < H; y += kStripe) {
            const int h = std::min(kStripe, H - y);
            std::vector<uint8_t> buf(tjBufSize(W, h, subsamp));
            unsigned char* p = buf.data();
            unsigned long size = buf.size();
            tjCompress2(enc, img.bgra.data() + static_cast<size_t>(y) * W * 4, W, W * 4, h, TJPF_BGRX, &p, &size, subsamp, quality,
                        flags | TJFLAG_NOREALLOC);
            total += size;
            if (keep) { buf.resize(size); stripes.push_back(std::move(buf)); }
        }
        return total;
    };
    compressAll(true);  // warm-up, keeps the data for the quality check
    size_t bytes = 0;
    double best = 1e9;
    for (int round = 0; round < 7; ++round) {  // best of several rounds: other processes only ever make a round slower
        const auto t0 = clk::now();
        for (int i = 0; i < iters; ++i) bytes = compressAll(false);
        best = std::min(best, std::chrono::duration<double, std::milli>(clk::now() - t0).count() / iters);
    }
    res.ms = best;
    res.kb = bytes / 1024.0;

    double se = 0;
    int stripe = 0;
    for (int y = 0; y < H; y += kStripe, ++stripe) {
        const int h = std::min(kStripe, H - y);
        std::vector<uint8_t> out(static_cast<size_t>(W) * h * 4);
        tjDecompress2(dec, stripes[stripe].data(), static_cast<unsigned long>(stripes[stripe].size()), out.data(), W, W * 4, h, TJPF_BGRX, 0);
        for (size_t i = 0; i < static_cast<size_t>(W) * h; ++i)
            for (int c = 0; c < 3; ++c) {
                const double d = double(out[i * 4 + c]) - img.bgra[(static_cast<size_t>(y) * W + i) * 4 + c];
                se += d * d;
            }
    }
    se /= static_cast<double>(W) * H * 3;
    res.psnr = se == 0 ? 99 : 10 * std::log10(255.0 * 255.0 / se);
    tjDestroy(enc);
    tjDestroy(dec);
    return res;
}

const char* sampName(int s) { return s == TJSAMP_444 ? "4:4:4" : s == TJSAMP_422 ? "4:2:2" : s == TJSAMP_420 ? "4:2:0" : "gray"; }

}  // namespace

// Real photograph: decode `path`, take the central 1280x720 crop as the "screen content".
bool loadPhoto(const char* path, Image& out) {
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (data.empty()) return false;
    tjhandle dec = tjInitDecompress();
    int w = 0, h = 0, ss = 0, cs = 0;
    if (tjDecompressHeader3(dec, data.data(), static_cast<unsigned long>(data.size()), &w, &h, &ss, &cs) != 0 || w < W || h < H) {
        tjDestroy(dec);
        return false;
    }
    std::vector<uint8_t> full(static_cast<size_t>(w) * h * 4);
    tjDecompress2(dec, data.data(), static_cast<unsigned long>(data.size()), full.data(), w, w * 4, h, TJPF_BGRX, 0);
    tjDestroy(dec);
    out.name = path;
    out.bgra.resize(static_cast<size_t>(W) * H * 4);
    const int x0 = (w - W) / 2, y0 = (h - H) / 2;
    for (int y = 0; y < H; ++y)
        std::memcpy(&out.bgra[static_cast<size_t>(y) * W * 4], &full[(static_cast<size_t>(y0 + y) * w + x0) * 4], static_cast<size_t>(W) * 4);
    return true;
}

// Compress with the server's stripe layout, decode again; returns the reconstructed frame.
std::vector<uint8_t> roundTrip(const Image& img, int quality, int subsamp) {
    tjhandle enc = tjInitCompress(), dec = tjInitDecompress();
    std::vector<uint8_t> out(static_cast<size_t>(W) * H * 4);
    for (int y = 0; y < H; y += kStripe) {
        const int h = std::min(kStripe, H - y);
        std::vector<uint8_t> buf(tjBufSize(W, h, subsamp));
        unsigned char* p = buf.data();
        unsigned long size = buf.size();
        tjCompress2(enc, img.bgra.data() + static_cast<size_t>(y) * W * 4, W, W * 4, h, TJPF_BGRX, &p, &size, subsamp, quality,
                    TJFLAG_FASTDCT | TJFLAG_NOREALLOC);
        tjDecompress2(dec, p, size, out.data() + static_cast<size_t>(y) * W * 4, W, W * 4, h, TJPF_BGRX, 0);
    }
    tjDestroy(enc);
    tjDestroy(dec);
    return out;
}

// 2x2 comparison sheet of one detail crop (zoomed 3x): q92 | q75 / q60 | q40, all 4:2:0 like the server.
void writeSheet(const Image& img, const char* path, int cx, int cy) {
    constexpr int cw = 300, ch = 170, zoom = 3;
    const int qs[4] = {92, 75, 60, 40};
    const int sw = cw * zoom * 2 + 8, sh = ch * zoom * 2 + 8;
    std::vector<uint8_t> sheet(static_cast<size_t>(sw) * sh * 4, 255);
    for (int k = 0; k < 4; ++k) {
        const std::vector<uint8_t> rt = roundTrip(img, qs[k], TJSAMP_420);
        const int ox = (k % 2) * (cw * zoom + 8), oy = (k / 2) * (ch * zoom + 8);
        for (int y = 0; y < ch * zoom; ++y)
            for (int x = 0; x < cw * zoom; ++x)
                std::memcpy(&sheet[(static_cast<size_t>(oy + y) * sw + ox + x) * 4], &rt[(static_cast<size_t>(cy + y / zoom) * W + cx + x / zoom) * 4], 4);
    }
    tjhandle enc = tjInitCompress();
    unsigned char* p = nullptr;
    unsigned long size = 0;
    tjCompress2(enc, sheet.data(), sw, sw * 4, sh, TJPF_BGRX, &p, &size, TJSAMP_444, 100, 0);
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(p), static_cast<std::streamsize>(size));
    tjFree(p);
    tjDestroy(enc);
}

void printTable(const Image& img) {
    std::printf("\n== %s\n", img.name);
    std::printf("  %-6s %-8s %9s %9s %9s\n", "qual", "chroma", "CPU ms", "KB", "PSNR dB");
    for (int q : {95, 92, 85, 75, 60, 40})
        for (int s : {TJSAMP_444, TJSAMP_420}) {
            if (s == TJSAMP_444 && q != 92) continue;
            const Result r = run(img, q, s, TJFLAG_FASTDCT, 20);
            std::printf("  %-6d %-8s %9.2f %9.1f %9.1f\n", q, sampName(s), r.ms, r.kb, r.psnr);
        }
}

int main(int argc, char** argv) {
    if (argc > 1) {  // real photographs
        int n = 0;
        for (int i = 1; i < argc; ++i) {
            Image img{"", {}};
            if (!loadPhoto(argv[i], img)) { std::printf("skipping %s (unreadable or smaller than 1280x720)\n", argv[i]); continue; }
            printTable(img);
            char out[64];
            std::snprintf(out, sizeof out, "photo_sheet_%d.jpg", n++);
            writeSheet(img, out, 500, 250);
            std::printf("  comparison sheet: %s (q92 | q75 / q60 | q40, 3x zoom)\n", out);
        }
        return 0;
    }
    const Image images[] = {makeImage("noisy video-like (+-7 noise)", 3, false), makeImage("clean smooth video", 0, false),
                            makeImage("smooth + saturated edges/lines", 0, true)};
    for (const Image& img : images) {
        std::printf("\n== %s, 1280x720 in %d-row stripes, single thread\n", img.name, kStripe);
        std::printf("  %-6s %-8s %-5s %9s %9s %9s\n", "qual", "chroma", "dct", "CPU ms", "KB", "PSNR dB");
        for (int q : {92, 85, 75, 60, 40})
            for (int s : {TJSAMP_444, TJSAMP_420}) {
                if (s == TJSAMP_444 && q != 92 && q != 60) continue;  // keep the table readable
                const Result r = run(img, q, s, TJFLAG_FASTDCT, 20);
                std::printf("  %-6d %-8s %-5s %9.2f %9.1f %9.1f\n", q, sampName(s), "fast", r.ms, r.kb, r.psnr);
            }
        const Result acc = run(img, 75, TJSAMP_420, TJFLAG_ACCURATEDCT, 20);
        std::printf("  %-6d %-8s %-5s %9.2f %9.1f %9.1f   (accurate DCT, for reference)\n", 75, "4:2:0", "accur", acc.ms, acc.kb, acc.psnr);
        const Result g = run(img, 75, TJSAMP_GRAY, TJFLAG_FASTDCT, 20);
        std::printf("  %-6d %-8s %-5s %9.2f %9.1f %9.1f   (luma only: lower bound, not usable)\n", 75, "gray", "fast", g.ms, g.kb, g.psnr);
    }
    return 0;
}
