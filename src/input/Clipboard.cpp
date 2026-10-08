#include "input/Clipboard.h"

#include <windows.h>

#include <chrono>

#include "util/ThreadCpu.h"
std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

struct ClipboardWndProc {
    static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_CLIPBOARDUPDATE || msg == WM_APP + 1) {
            ClipboardMonitor& cm = ClipboardMonitor::instance();
            std::vector<std::function<void()>> cbs;
            {
                std::lock_guard<std::mutex> lk(cm.m_);
                if (msg == WM_CLIPBOARDUPDATE && GetClipboardSequenceNumber() == cm.ignoreSeq_) return 0;  // our own write
                for (auto& l : cm.listeners_) cbs.push_back(l.second);
            }
            for (auto& cb : cbs) cb();
            return 0;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }
};

ClipboardMonitor& ClipboardMonitor::instance() {
    static ClipboardMonitor inst;
    return inst;
}

ClipboardMonitor::ClipboardMonitor() {
    thread_ = std::thread([this] { threadMain(); });
    thread_.detach();  // lives for the whole process
}

void ClipboardMonitor::threadMain() {
    threadcpu::Scope cpuScope("clipboard");
    WNDCLASSW wc{};
    wc.lpfnWndProc = ClipboardWndProc::proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"DaramVncClipboard";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    hwnd_ = hwnd;
    if (hwnd) AddClipboardFormatListener(hwnd);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void ClipboardMonitor::simulateChange() {
    if (hwnd_) PostMessageW(static_cast<HWND>(hwnd_), WM_APP + 1, 0, 0);
}

int ClipboardMonitor::addListener(std::function<void()> cb) {
    std::lock_guard<std::mutex> lk(m_);
    listeners_.emplace_back(nextToken_, std::move(cb));
    return nextToken_++;
}

void ClipboardMonitor::removeListener(int token) {
    std::lock_guard<std::mutex> lk(m_);
    for (size_t i = 0; i < listeners_.size(); ++i)
        if (listeners_[i].first == token) { listeners_.erase(listeners_.begin() + static_cast<std::ptrdiff_t>(i)); break; }
}

namespace {
bool openClipboardRetry() {
    for (int i = 0; i < 10; ++i) {
        if (OpenClipboard(nullptr)) return true;
        Sleep(10);
    }
    return false;
}
}  // namespace

bool ClipboardMonitor::getText(std::wstring& out) {
    if (!openClipboardRetry()) return false;
    bool ok = false;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* p = static_cast<const wchar_t*>(GlobalLock(h))) {
            out.assign(p);
            GlobalUnlock(h);
            ok = true;
        }
    }
    CloseClipboard();
    return ok;
}

bool ClipboardMonitor::setText(const std::wstring& text) {
    if (!openClipboardRetry()) return false;
    EmptyClipboard();
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
    bool ok = false;
    if (mem) {
        if (auto* p = static_cast<wchar_t*>(GlobalLock(mem))) {
            std::memcpy(p, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(mem);
            ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
        }
        if (!ok) GlobalFree(mem);
    }
    {
        // Our own write fires WM_CLIPBOARDUPDATE; record its sequence number (before closing, so the monitor
        // thread cannot see the update first) so listeners aren't notified for it.
        std::lock_guard<std::mutex> lk(m_);
        ignoreSeq_ = GetClipboardSequenceNumber();
    }
    CloseClipboard();
    return ok;
}
