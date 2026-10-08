#include "capture/CursorCapture.h"

#include <windows.h>

namespace {

bool readBitmap32(HDC dc, HBITMAP bmp, int w, int h, std::vector<uint8_t>& out) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    out.assign(static_cast<size_t>(w) * h * 4, 0);
    return GetDIBits(dc, bmp, 0, static_cast<UINT>(h), out.data(), &bi, DIB_RGB_COLORS) != 0;
}

}  // namespace

bool captureCurrentCursor(CursorShape& out) {
    CURSORINFO ci{sizeof(CURSORINFO)};
    if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !ci.hCursor) return false;
    ICONINFO ii{};
    if (!GetIconInfo(ci.hCursor, &ii)) return false;

    bool ok = false;
    HDC dc = GetDC(nullptr);
    BITMAP bm{};
    if (GetObject(ii.hbmMask, sizeof(bm), &bm)) {
        const int w = bm.bmWidth;
        const int h = ii.hbmColor ? bm.bmHeight : bm.bmHeight / 2;
        std::vector<uint8_t> maskBits, color;
        if (w > 0 && h > 0 && readBitmap32(dc, ii.hbmMask, w, ii.hbmColor ? h : h * 2, maskBits) &&
            (!ii.hbmColor || readBitmap32(dc, ii.hbmColor, w, h, color))) {
            out.w = w; out.h = h; out.hotX = ii.xHotspot; out.hotY = ii.yHotspot;
            out.bgra.assign(static_cast<size_t>(w) * h * 4, 0);
            out.mask.assign(static_cast<size_t>(w) * h, 0);
            bool hasAlpha = false;
            for (size_t i = 3; i < color.size(); i += 4) if (color[i]) { hasAlpha = true; break; }
            for (int i = 0; i < w * h; ++i) {
                const uint8_t* andPx = &maskBits[static_cast<size_t>(i) * 4];
                if (ii.hbmColor) {
                    const uint8_t* c = &color[static_cast<size_t>(i) * 4];
                    out.bgra[i * 4] = c[0]; out.bgra[i * 4 + 1] = c[1]; out.bgra[i * 4 + 2] = c[2];
                    out.mask[i] = hasAlpha ? (c[3] >= 128) : (andPx[0] == 0);
                } else {
                    const uint8_t* xorPx = &maskBits[static_cast<size_t>(i + w * h) * 4];
                    bool a = andPx[0] != 0, x = xorPx[0] != 0;
                    uint8_t v = x && !a ? 255 : 0;  // white only for (and=0,xor=1); invert -> black
                    out.bgra[i * 4] = out.bgra[i * 4 + 1] = out.bgra[i * 4 + 2] = v;
                    out.mask[i] = !(a && !x);
                }
            }
            ok = true;
        }
    }
    ReleaseDC(nullptr, dc);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    return ok;
}
