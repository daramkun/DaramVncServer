#include "util/Log.h"

#include <windows.h>

#include <share.h>

#include <cstdio>
#include <mutex>

namespace logging {

namespace {
std::mutex g_mutex;
FILE* g_file = nullptr;
std::string g_path;
size_t g_size = 0, g_max = 0;
int g_keep = 3;
bool g_console = true;

std::string numbered(int i) { return g_path + "." + std::to_string(i); }

size_t fileSize(const std::string& path) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &d)) return 0;
    return (static_cast<size_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
}

// Called with g_mutex held.
void rotateLocked() {
    if (g_file) std::fclose(g_file);
    g_file = nullptr;
    bool truncate = false;  // true: the live file could not be renamed away, so it is emptied in place
    if (g_keep <= 0) {
        truncate = !DeleteFileA(g_path.c_str());
    } else {
        DeleteFileA(numbered(g_keep).c_str());  // the oldest one goes
        for (int i = g_keep - 1; i >= 1; --i)   // .2 -> .3, .1 -> .2
            MoveFileExA(numbered(i).c_str(), numbered(i + 1).c_str(), MOVEFILE_REPLACE_EXISTING);
        if (!MoveFileExA(g_path.c_str(), numbered(1).c_str(), MOVEFILE_REPLACE_EXISTING)) {
            // Somebody (tail, an editor) has the live file open without allowing renames. Copy it to .1 and empty
            // it where it is, so the disk use stays bounded even then.
            truncate = CopyFileA(g_path.c_str(), numbered(1).c_str(), FALSE) != 0;
        }
    }
    g_file = _fsopen(g_path.c_str(), truncate ? "wb" : "ab", _SH_DENYWR);
    // If nothing worked, logging continues by appending; the counter restarts so the next attempt comes after
    // another `max` bytes instead of on every line.
    g_size = 0;
}
}  // namespace

void setLogFile(const std::string& path, size_t maxBytes, int keep) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_file) std::fclose(g_file);
    g_file = nullptr;
    g_path = path;
    g_max = maxBytes;
    g_keep = keep;
    g_size = 0;
    if (!path.empty()) {
        g_file = _fsopen(path.c_str(), "ab", _SH_DENYWR);  // tail/Notepad can open it while the service runs
        g_size = fileSize(path);  // an existing log continues where it left off
    }
}

void setConsoleOutput(bool enabled) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_console = enabled;
}

void write(const char* fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[1200];
    const int len = std::snprintf(line, sizeof line, "%04d-%02d-%02d %02d:%02d:%02d.%03d [%lu] %s\n", t.wYear, t.wMonth, t.wDay,
                                  t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentThreadId(), msg);
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_console) std::fputs(line, stderr);
    if (g_file) {
        if (g_max && g_size > 0 && g_size + static_cast<size_t>(len) > g_max) rotateLocked();
        if (g_file) {
            std::fputs(line, g_file);
            std::fflush(g_file);
            g_size += static_cast<size_t>(len);
        }
    }
}

}  // namespace logging
