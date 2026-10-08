// Encoder micro-benchmark on a real screenshot and a synthetic "video" frame: where does CPU time go?
#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

#include "capture/GdiCapturer.h"
#include "encode/RawEncoder.h"
#include "encode/TightEncoder.h"
#include "encode/TileClass.h"
#include "encode/ZrleEncoder.h"

using namespace rfbenc;
using clk = std::chrono::steady_clock;

namespace {

template <class F>
double timeMs(int iters, F&& f) {
    f();  // warm up
    const auto t0 = clk::now();
    for (int i = 0; i < iters; ++i) f();
    return std::chrono::duration<double, std::milli>(clk::now() - t0).count() / iters;
}

void run(const char* name, const FrameView& fb, int iters) {
    std::printf("== %s (%dx%d)\n", name, fb.width, fb.height);
    const Rect all{0, 0, fb.width, fb.height};
    PixelFormat pf;

    size_t solid = 0, flat = 0, photo = 0;
    const double cls = timeMs(iters, [&] {
        solid = flat = photo = 0;
        for (int y = 0; y < fb.height; y += 64)
            for (int x = 0; x < fb.width; x += 64) {
                const TileClass c = classifyTile(fb, x, y, std::min(64, fb.width - x), std::min(64, fb.height - y));
                (c.kind == 0 ? solid : c.kind == 1 ? flat : photo)++;
            }
    });
    std::printf("  tile classification            %7.2f ms   (tiles: %zu solid, %zu flat, %zu photo)\n", cls, solid, flat, photo);

    for (int level : {1, 3}) {
        TightEncoder enc(pf, -1, level);
        size_t bytes = 0;
        const double ms = timeMs(iters, [&] { std::vector<uint8_t> o; enc.encode(fb, all, o); bytes = o.size(); });
        std::printf("  Tight lossless, zlib %d         %7.2f ms   %8.1f KB\n", level, ms, bytes / 1024.0);
    }
    {
        TightEncoder enc(pf, 80, 3);
        size_t bytes = 0;
        const double ms = timeMs(iters, [&] { std::vector<uint8_t> o; enc.encode(fb, all, o); bytes = o.size(); });
        std::printf("  Tight + JPEG q80, zlib 3       %7.2f ms   %8.1f KB\n", ms, bytes / 1024.0);
    }
    {
        ZrleEncoder enc(pf, 3);
        size_t bytes = 0;
        const double ms = timeMs(iters, [&] { std::vector<uint8_t> o; enc.encode(fb, all, o); bytes = o.size(); });
        std::printf("  ZRLE, zlib 3                   %7.2f ms   %8.1f KB\n", ms, bytes / 1024.0);
    }
    {
        RawEncoder enc(pf);
        const double ms = timeMs(iters, [&] { std::vector<uint8_t> o; enc.encode(fb, all, o); });
        std::printf("  Raw (copy only)                %7.2f ms\n", ms);
    }
}

}  // namespace

int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    GdiCapturer gdi;
    FrameUpdate u;
    gdi.grabFull(u);
    const FrameView screen{gdi.pixels(), gdi.width(), gdi.height()};
    run("real screenshot", screen, 10);

    const int w = 1280, h = 720;  // smooth moving gradient with sensor-like noise: a stand-in for video
    std::vector<uint8_t> video(static_cast<size_t>(w) * h * 4);
    unsigned seed = 1;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            seed = seed * 1664525u + 1013904223u;
            const int n = (seed >> 24) & 7;
            uint8_t* p = &video[(static_cast<size_t>(y) * w + x) * 4];
            p[2] = static_cast<uint8_t>(128 + 100 * std::sin(x / 40.0) + n);
            p[1] = static_cast<uint8_t>(128 + 100 * std::sin(y / 30.0 + x / 90.0) + n);
            p[0] = static_cast<uint8_t>(128 + 100 * std::sin((x + y) / 50.0) + n);
        }
    run("synthetic video frame", FrameView{video.data(), w, h}, 10);
    return 0;
}
