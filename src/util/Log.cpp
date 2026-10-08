#include "util/Log.h"

#include <windows.h>

#include <cstdio>
#include <mutex>

namespace logging {

namespace {
std::mutex g_mutex;
FILE* g_file = nullptr;
}  // namespace

void setLogFile(const std::string& path) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_file) std::fclose(g_file);
    g_file = nullptr;
    if (!path.empty()) fopen_s(&g_file, path.c_str(), "ab");
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
    std::snprintf(line, sizeof line, "%04d-%02d-%02d %02d:%02d:%02d.%03d [%lu] %s\n", t.wYear, t.wMonth, t.wDay,
                  t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentThreadId(), msg);
    std::lock_guard<std::mutex> lk(g_mutex);
    std::fputs(line, stderr);
    if (g_file) {
        std::fputs(line, g_file);
        std::fflush(g_file);
    }
}

}  // namespace logging
