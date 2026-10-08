#include "util/ThreadCpu.h"

#include <windows.h>

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "util/Log.h"

namespace threadcpu {

namespace {

struct Entry {
    DWORD tid;
    HANDLE handle;
    std::string name;
    ULONGLONG lastUser = 0, lastKernel = 0;
};

struct Totals {
    ULONGLONG user = 0, kernel = 0;  // 100 ns units
};

std::mutex g_mutex;
std::vector<Entry> g_threads;
std::map<std::string, Totals> g_retired;  // CPU of threads that already exited, not yet reported
ULONGLONG g_lastReportTick = 0;
ULONGLONG g_lastProcUser = 0, g_lastProcKernel = 0;

ULONGLONG ft(const FILETIME& f) { return (static_cast<ULONGLONG>(f.dwHighDateTime) << 32) | f.dwLowDateTime; }

bool times(HANDLE h, ULONGLONG& user, ULONGLONG& kernel) {
    FILETIME c, e, k, u;
    if (!GetThreadTimes(h, &c, &e, &k, &u)) return false;
    user = ft(u);
    kernel = ft(k);
    return true;
}

}  // namespace

Scope::Scope(const char* name) {
    const DWORD tid = GetCurrentThreadId();
    HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
    Entry e{tid, h, name};
    if (h) times(h, e.lastUser, e.lastKernel);  // count only from now on
    std::lock_guard<std::mutex> lk(g_mutex);
    g_threads.push_back(std::move(e));
}

Scope::~Scope() {
    const DWORD tid = GetCurrentThreadId();
    std::lock_guard<std::mutex> lk(g_mutex);
    for (size_t i = 0; i < g_threads.size(); ++i) {
        if (g_threads[i].tid != tid) continue;
        ULONGLONG u, k;
        if (g_threads[i].handle && times(g_threads[i].handle, u, k)) {
            g_retired[g_threads[i].name].user += u - g_threads[i].lastUser;
            g_retired[g_threads[i].name].kernel += k - g_threads[i].lastKernel;
        }
        if (g_threads[i].handle) CloseHandle(g_threads[i].handle);
        g_threads.erase(g_threads.begin() + static_cast<std::ptrdiff_t>(i));
        break;
    }
}

void logReport() {
    std::lock_guard<std::mutex> lk(g_mutex);
    const ULONGLONG now = GetTickCount64();
    FILETIME c, e, pk, pu;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &pk, &pu);
    if (g_lastReportTick == 0) {  // first call only sets the baseline: threads and process must start from the same instant
        g_lastReportTick = now;
        g_lastProcUser = ft(pu);
        g_lastProcKernel = ft(pk);
        g_retired.clear();
        for (Entry& t : g_threads)
            if (t.handle) times(t.handle, t.lastUser, t.lastKernel);
        return;
    }
    const double wallSec = static_cast<double>(now - g_lastReportTick) / 1000.0;
    if (wallSec < 1.0) return;

    std::map<std::string, Totals> byName = std::move(g_retired);
    g_retired.clear();
    for (Entry& t : g_threads) {
        ULONGLONG u, k;
        if (!t.handle || !times(t.handle, u, k)) continue;
        byName[t.name].user += u - t.lastUser;
        byName[t.name].kernel += k - t.lastKernel;
        t.lastUser = u;
        t.lastKernel = k;
    }
    const ULONGLONG procUser = ft(pu) - g_lastProcUser, procKernel = ft(pk) - g_lastProcKernel;
    g_lastProcUser = ft(pu);
    g_lastProcKernel = ft(pk);
    g_lastReportTick = now;

    auto pct = [&](ULONGLONG v) { return static_cast<double>(v) / 1e7 / wallSec * 100.0; };
    std::string line;
    ULONGLONG namedUser = 0, namedKernel = 0;
    char buf[160];
    for (const auto& kv : byName) {
        namedUser += kv.second.user;
        namedKernel += kv.second.kernel;
        std::snprintf(buf, sizeof buf, " %s %.1f%% (user %.1f, kernel %.1f);", kv.first.c_str(),
                      pct(kv.second.user + kv.second.kernel), pct(kv.second.user), pct(kv.second.kernel));
        line += buf;
    }
    const ULONGLONG total = procUser + procKernel, named = namedUser + namedKernel;
    std::snprintf(buf, sizeof buf, " other %.1f%%; process total %.1f%%", pct(total > named ? total - named : 0), pct(total));
    line += buf;
    LOGI("cpu over %.0f s (percent of one core):%s", wallSec, line.c_str());
}

}  // namespace threadcpu
