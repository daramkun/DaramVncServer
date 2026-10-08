// Benchmark helper: opens a 1280x720 topmost window that redraws at ~60 Hz for N seconds, with a text-like left
// half (few colours) and a photo-like right half (smooth moving gradient + noise), so the VNC server sees steady
// changes of both kinds. Usage: animwin [seconds] [text|photo|both]
#include <windows.h>

#include <cstdlib>
#include <cstring>
#include <string>

static DWORD g_end;
static int g_mode = 2;  // 0 text, 1 photo, 2 both

LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    if (m == WM_ERASEBKGND) return 1;
    return DefWindowProcW(h, m, w, l);
}

int main(int argc, char** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const int seconds = argc > 1 ? std::atoi(argv[1]) : 8;
    if (argc > 2) g_mode = !std::strcmp(argv[2], "text") ? 0 : !std::strcmp(argv[2], "photo") ? 1 : 2;
    WNDCLASSW wc{};
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"DaramAnim";
    RegisterClassW(&wc);
    const int W = 1280, H = 720;
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"VNC benchmark animation", WS_POPUP | WS_VISIBLE, 100, 100, W, H, nullptr, nullptr, wc.hInstance, nullptr);
    HDC dc = GetDC(hwnd);
    HDC mem = CreateCompatibleDC(dc);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB};
    uint32_t* px = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, reinterpret_cast<void**>(&px), nullptr, 0);
    SelectObject(mem, bmp);
    HFONT font = CreateFontW(18, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Consolas");
    SelectObject(mem, font);
    SetBkColor(mem, RGB(255, 255, 255));
    SetBkMode(mem, OPAQUE);

    g_end = GetTickCount() + static_cast<DWORD>(seconds) * 1000;
    unsigned frame = 0, seed = 1;
    MSG msg;
    while (GetTickCount() < g_end) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        const int split = g_mode == 0 ? W : g_mode == 1 ? 0 : W / 2;
        if (split > 0) {
            RECT r{0, 0, split, H};
            FillRect(mem, &r, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
            for (int line = 0; line < H / 20; ++line) {
                std::wstring t = L"line " + std::to_wstring(line + frame / 2) + L": the quick brown fox jumps over the lazy dog";
                TextOutW(mem, 8, line * 20, t.c_str(), static_cast<int>(t.size()));
            }
        }
        if (split < W) {
            for (int y = 0; y < H; ++y)
                for (int x = split; x < W; ++x) {
                    seed = seed * 1664525u + 1013904223u;
                    const unsigned noise = (seed >> 24) & 7;
                    const unsigned r = ((x + frame * 3) & 255), g = ((y * 255 / H + frame) & 255), b = (((x + y) / 2 + frame * 2) & 255);
                    px[y * W + x] = ((r + noise) & 255) << 16 | ((g + noise) & 255) << 8 | ((b + noise) & 255);
                }
        }
        BitBlt(dc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
        ++frame;
        Sleep(8);
    }
    DestroyWindow(hwnd);
    return 0;
}
