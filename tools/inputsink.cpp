// Test aid for the input injector: records every *injected* keyboard/mouse event system-wide (low-level hooks)
// and swallows it, so nothing reaches any application or moves the real cursor. Real user input is left alone.
//
//   inputsink <log file> <seconds>
//
// The log gets one line per event ("K down vk=0x41 scan=0x1E ext=0", "M move x=100 y=200", ...). The line
// "READY" is written once the hooks are installed.
#include <windows.h>

#include <share.h>

#include <cstdio>
#include <cstdlib>

static FILE* g_log = nullptr;

static LRESULT CALLBACK keyboardProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        const auto* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
        if (k->flags & LLKHF_INJECTED) {
            const bool up = wp == WM_KEYUP || wp == WM_SYSKEYUP;
            if (k->vkCode == VK_PACKET)  // KEYEVENTF_UNICODE: the UTF-16 unit travels in the scan code field
                std::fprintf(g_log, "K %s unicode=U+%04X\n", up ? "up  " : "down", k->scanCode);
            else
                std::fprintf(g_log, "K %s vk=0x%02X scan=0x%02X ext=%d\n", up ? "up  " : "down", k->vkCode, k->scanCode,
                             (k->flags & LLKHF_EXTENDED) ? 1 : 0);
            std::fflush(g_log);
            return 1;  // swallow: the keystroke never reaches a window
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

static LRESULT CALLBACK mouseProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        const auto* m = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
        if (m->flags & LLMHF_INJECTED) {
            const char* what = "?";
            switch (wp) {
                case WM_MOUSEMOVE: what = "move"; break;
                case WM_LBUTTONDOWN: what = "left down"; break;
                case WM_LBUTTONUP: what = "left up"; break;
                case WM_RBUTTONDOWN: what = "right down"; break;
                case WM_RBUTTONUP: what = "right up"; break;
                case WM_MBUTTONDOWN: what = "middle down"; break;
                case WM_MBUTTONUP: what = "middle up"; break;
                case WM_MOUSEWHEEL: what = "wheel"; break;
                case WM_MOUSEHWHEEL: what = "hwheel"; break;
            }
            const int delta = static_cast<short>(HIWORD(m->mouseData));
            std::fprintf(g_log, "M %-11s x=%ld y=%ld delta=%d\n", what, m->pt.x, m->pt.y, (wp == WM_MOUSEWHEEL || wp == WM_MOUSEHWHEEL) ? delta : 0);
            std::fflush(g_log);
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: inputsink <log file> <seconds>\n"); return 2; }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);  // pointer positions in real pixels
    g_log = _fsopen(argv[1], "w", _SH_DENYNO);  // the test script reads the log while we are still writing it
    if (!g_log) return 3;
    HHOOK kb = SetWindowsHookExW(WH_KEYBOARD_LL, keyboardProc, GetModuleHandleW(nullptr), 0);
    HHOOK ms = SetWindowsHookExW(WH_MOUSE_LL, mouseProc, GetModuleHandleW(nullptr), 0);
    if (!kb || !ms) { std::fprintf(g_log, "ERROR: cannot install hooks\n"); return 4; }
    std::fprintf(g_log, "READY\n");
    std::fflush(g_log);
    const DWORD end = GetTickCount() + static_cast<DWORD>(std::atoi(argv[2])) * 1000;
    MSG msg;
    while (GetTickCount() < end) {  // hooks are called from inside this message loop
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
    }
    UnhookWindowsHookEx(kb);
    UnhookWindowsHookEx(ms);
    std::fprintf(g_log, "DONE\n");
    std::fclose(g_log);
    return 0;
}
