#include "util/Desktop.h"

#include <windows.h>

namespace desktop {

namespace {

struct ThreadState {
    HDESK current = nullptr;
    std::string name;
    ULONGLONG lastCheck = 0;
};
thread_local ThreadState t_state;

std::string desktopName(HDESK d) {
    char buf[256] = {0};
    DWORD needed = 0;
    GetUserObjectInformationA(d, UOI_NAME, buf, sizeof buf, &needed);
    return buf;
}

}  // namespace

bool attachToInputDesktop(std::string* name) {
    ThreadState& s = t_state;
    const ULONGLONG now = GetTickCount64();
    if (s.current && now - s.lastCheck < 100) {
        if (name) *name = s.name;
        return false;
    }
    s.lastCheck = now;

    HDESK input = OpenInputDesktop(0, FALSE, GENERIC_ALL);
    if (!input) {  // secure desktop without sufficient rights, or session locked: stay where we are
        if (!s.current) {
            s.current = GetThreadDesktop(GetCurrentThreadId());
            s.name = desktopName(s.current);
            s.current = nullptr;  // not ours to close; try again next time
        }
        if (name) *name = s.name;
        return false;
    }
    const std::string newName = desktopName(input);
    if (!s.name.empty() && newName == s.name && s.current) {
        CloseDesktop(input);
        if (name) *name = s.name;
        return false;
    }
    const bool hadDesktop = !s.name.empty();
    if (SetThreadDesktop(input)) {
        if (s.current) CloseDesktop(s.current);
        s.current = input;
        const bool changed = hadDesktop && newName != s.name;
        s.name = newName;
        if (name) *name = s.name;
        return changed;
    }
    CloseDesktop(input);
    if (name) *name = s.name;
    return false;
}

}  // namespace desktop
