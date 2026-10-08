// Test helper: changes the clipboard text once (so servers see a clipboard update), then restores it.
// Refuses to touch a clipboard that holds anything other than plain text. Prints wall-clock milliseconds.
#include <windows.h>

#include <cstdio>
#include <string>

static double wallMs() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER u{{ft.dwLowDateTime, ft.dwHighDateTime}};
    return static_cast<double>(u.QuadPart) / 10000.0;
}

static bool openClip() {
    for (int i = 0; i < 20; ++i) {
        if (OpenClipboard(nullptr)) return true;
        Sleep(20);
    }
    return false;
}

static void setText(const std::wstring& t, bool empty) {
    if (!openClip()) return;
    EmptyClipboard();
    if (!empty) {
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (t.size() + 1) * sizeof(wchar_t));
        if (mem) {
            wchar_t* p = static_cast<wchar_t*>(GlobalLock(mem));
            memcpy(p, t.c_str(), (t.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(mem);
            SetClipboardData(CF_UNICODETEXT, mem);
        }
    }
    CloseClipboard();
}

int main(int argc, char** argv) {
    const int restoreAfterMs = argc > 1 ? atoi(argv[1]) : 500;
    if (!openClip()) { std::puts("cannot open the clipboard"); return 3; }
    bool onlyText = true;
    for (UINT f = EnumClipboardFormats(0); f; f = EnumClipboardFormats(f))
        if (f != CF_TEXT && f != CF_OEMTEXT && f != CF_UNICODETEXT && f != CF_LOCALE) onlyText = false;
    std::wstring original;
    bool hadText = false;
    if (onlyText) {
        if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
            if (const wchar_t* p = static_cast<const wchar_t*>(GlobalLock(h))) { original = p; hadText = true; GlobalUnlock(h); }
        }
    }
    CloseClipboard();
    if (!onlyText) { std::puts("clipboard holds non-text formats: skipping so nothing is lost"); return 2; }

    std::printf("clip_poke: changing clipboard at wall %.1f\n", wallMs());
    setText(L"daramvnc clipboard test", false);
    Sleep(static_cast<DWORD>(restoreAfterMs));
    setText(original, !hadText);
    std::printf("clip_poke: clipboard restored at wall %.1f\n", wallMs());
    return 0;
}
