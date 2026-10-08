#pragma once
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Watches the Windows clipboard (message-only window on its own thread) and reads/writes plain text.
class ClipboardMonitor {
public:
    static ClipboardMonitor& instance();

    // Registers a callback fired (on the monitor thread) when the clipboard text changes through other means than
    // setText(). Returns a token for removeListener().
    int addListener(std::function<void()> cb);
    void removeListener(int token);

    bool getText(std::wstring& out);
    // Writes text and suppresses the change notification it would cause.
    bool setText(const std::wstring& text);

    // Test aid: runs the change listeners on the monitor thread exactly as a real clipboard update would,
    // without touching the user's clipboard.
    void simulateChange();

private:
    ClipboardMonitor();
    void threadMain();

    std::mutex m_;
    std::vector<std::pair<int, std::function<void()>>> listeners_;
    int nextToken_ = 1;
    unsigned long ignoreSeq_ = 0;
    std::thread thread_;
    void* hwnd_ = nullptr;
    friend struct ClipboardWndProc;
};

// Text conversions used by the clipboard protocol.
std::string wideToUtf8(const std::wstring& w);
std::wstring utf8ToWide(const std::string& s);
