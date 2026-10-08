#include "input/InputInjector.h"

#include <windows.h>

#include <algorithm>

#include "util/Desktop.h"

namespace {

struct Named {
    uint32_t keysym;
    WORD vk;
    bool extended;
};

// Non-printable X11 keysyms -> Windows virtual keys.
const Named kNamed[] = {
    {0xFF08, VK_BACK, false},    {0xFF09, VK_TAB, false},      {0xFF0D, VK_RETURN, false},  {0xFF13, VK_PAUSE, false},
    {0xFF14, VK_SCROLL, false},  {0xFF1B, VK_ESCAPE, false},   {0xFF50, VK_HOME, true},     {0xFF51, VK_LEFT, true},
    {0xFF52, VK_UP, true},       {0xFF53, VK_RIGHT, true},     {0xFF54, VK_DOWN, true},     {0xFF55, VK_PRIOR, true},
    {0xFF56, VK_NEXT, true},     {0xFF57, VK_END, true},       {0xFF61, VK_SNAPSHOT, true}, {0xFF63, VK_INSERT, true},
    {0xFF67, VK_APPS, true},     {0xFF7F, VK_NUMLOCK, true},   {0xFF8D, VK_RETURN, true},   {0xFFFF, VK_DELETE, true},
    {0xFFE1, VK_LSHIFT, false},  {0xFFE2, VK_RSHIFT, false},   {0xFFE3, VK_LCONTROL, false}, {0xFFE4, VK_RCONTROL, true},
    {0xFFE5, VK_CAPITAL, false}, {0xFFE7, VK_LMENU, false},    {0xFFE8, VK_RMENU, true},    {0xFFE9, VK_LMENU, false},
    {0xFFEA, VK_RMENU, true},    {0xFFEB, VK_LWIN, true},      {0xFFEC, VK_RWIN, true},     {0xFE03, VK_RMENU, true},
    {0xFF31, VK_HANGUL, false},  {0xFF34, VK_HANJA, false},    {0xFFAA, VK_MULTIPLY, false},{0xFFAB, VK_ADD, false},
    {0xFFAD, VK_SUBTRACT, false},{0xFFAE, VK_DECIMAL, false},  {0xFFAF, VK_DIVIDE, true},
};

bool lookupNamed(uint32_t ks, WORD& vk, bool& ext) {
    if (ks >= 0xFFBE && ks <= 0xFFD5) { vk = static_cast<WORD>(VK_F1 + (ks - 0xFFBE)); ext = false; return true; }
    if (ks >= 0xFFB0 && ks <= 0xFFB9) { vk = static_cast<WORD>(VK_NUMPAD0 + (ks - 0xFFB0)); ext = false; return true; }
    for (const Named& n : kNamed)
        if (n.keysym == ks) { vk = n.vk; ext = n.extended; return true; }
    return false;
}

void sendSas() {  // Ctrl+Alt+Del: only effective from a service / with SoftwareSASGeneration policy
    if (HMODULE m = LoadLibraryA("sas.dll")) {
        using Fn = VOID(WINAPI*)(BOOL);
        if (auto fn = reinterpret_cast<Fn>(GetProcAddress(m, "SendSAS"))) fn(FALSE);
        FreeLibrary(m);
    }
}

void sendInputs(INPUT* in, UINT n) {
    if (SendInput(n, in, sizeof(INPUT)) == 0) {  // likely a desktop switch: re-attach and retry once
        desktop::attachToInputDesktop();
        SendInput(n, in, sizeof(INPUT));
    }
}

}  // namespace

void InputInjector::pointer(uint8_t mask, int x, int y) {
    desktop::attachToInputDesktop();
    x = std::clamp(x, 0, w_ > 0 ? w_ - 1 : 0);  // client-controlled: keep the 65535 scaling below within int range
    y = std::clamp(y, 0, h_ > 0 ? h_ - 1 : 0);
    INPUT in[8]{};
    int n = 0;
    if (x != curX_ || y != curY_ || mask == lastMask_) {
        in[n].type = INPUT_MOUSE;
        in[n].mi.dx = static_cast<LONG>(x * 65535 / (w_ > 1 ? w_ - 1 : 1));
        in[n].mi.dy = static_cast<LONG>(y * 65535 / (h_ > 1 ? h_ - 1 : 1));
        // Framebuffer == virtual desktop when it spans all monitors (single monitor: identical to the primary).
        const bool virt = GetSystemMetrics(SM_CXVIRTUALSCREEN) == w_ && GetSystemMetrics(SM_CYVIRTUALSCREEN) == h_;
        in[n].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | (virt ? MOUSEEVENTF_VIRTUALDESK : 0);
        ++n;
        curX_ = x;
        curY_ = y;
    }

    struct Btn { uint8_t bit; DWORD down, up; };
    static const Btn btns[] = {
        {1, MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP},
        {2, MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP},
        {4, MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP},
    };
    const uint8_t changed = mask ^ lastMask_;
    for (const auto& b : btns) {
        if (changed & b.bit) {
            in[n].type = INPUT_MOUSE;
            in[n].mi.dwFlags = (mask & b.bit) ? b.down : b.up;
            ++n;
        }
    }
    // Buttons 4/5 (bits 8/16) are the vertical wheel, 6/7 (bits 32/64) the horizontal one; each press = one notch.
    struct Wheel { uint8_t bit; DWORD flag; int delta; };
    static const Wheel wheels[] = {{8, MOUSEEVENTF_WHEEL, WHEEL_DELTA},       {16, MOUSEEVENTF_WHEEL, -WHEEL_DELTA},
                                   {32, MOUSEEVENTF_HWHEEL, -WHEEL_DELTA},    {64, MOUSEEVENTF_HWHEEL, WHEEL_DELTA}};
    for (const auto& w : wheels) {
        if ((changed & w.bit) && (mask & w.bit)) {
            in[n].type = INPUT_MOUSE;
            in[n].mi.dwFlags = w.flag;
            in[n].mi.mouseData = static_cast<DWORD>(w.delta);
            ++n;
        }
    }
    if (n) sendInputs(in, static_cast<UINT>(n));
    lastMask_ = mask;
}

void InputInjector::send(const Pressed& p, bool down) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    if (p.scan) {
        in.ki.wScan = p.scanCode;
        in.ki.dwFlags = KEYEVENTF_SCANCODE | (p.extended ? KEYEVENTF_EXTENDEDKEY : 0) | (down ? 0 : KEYEVENTF_KEYUP);
    } else if (p.unicode) {
        in.ki.wScan = p.unit;
        in.ki.dwFlags = KEYEVENTF_UNICODE | (down ? 0 : KEYEVENTF_KEYUP);
    } else {
        in.ki.wVk = p.vk;
        in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(p.vk, MAPVK_VK_TO_VSC));
        in.ki.dwFlags = (p.extended ? KEYEVENTF_EXTENDEDKEY : 0) | (down ? 0 : KEYEVENTF_KEYUP);
    }
    sendInputs(&in, 1);
}

void InputInjector::key(bool down, uint32_t keysym) {
    desktop::attachToInputDesktop();

    if (!down) {  // release exactly what the press injected
        auto it = down_.find(keysym);
        if (it == down_.end()) return;
        send(it->second, false);
        const WORD vk = it->second.vk;
        if (!it->second.unicode) {
            if (vk == VK_LSHIFT || vk == VK_RSHIFT) shiftDown_ = false;
            if (vk == VK_LCONTROL || vk == VK_RCONTROL) ctrlDown_ = false;
            if (vk == VK_LMENU || vk == VK_RMENU) altDown_ = false;
        }
        down_.erase(it);
        return;
    }
    if (down_.count(keysym)) {  // auto-repeat: press again without a release
        send(down_[keysym], true);
        return;
    }

    Pressed p;
    WORD vk;
    bool ext;
    if (lookupNamed(keysym, vk, ext)) {
        p.vk = vk;
        p.extended = ext;
        if (vk == VK_LSHIFT || vk == VK_RSHIFT) shiftDown_ = true;
        if (vk == VK_LCONTROL || vk == VK_RCONTROL) ctrlDown_ = true;
        if (vk == VK_LMENU || vk == VK_RMENU) altDown_ = true;
        if (vk == VK_DELETE && ctrlDown_ && altDown_) {
            sendSas();
            return;
        }
    } else {
        const uint32_t cp = keysym >= 0x01000000 ? keysym - 0x01000000 : keysym;
        // Keysyms below 0x01000000 are Latin-1 only up to 0xFF; the rest are legacy X11 symbols we can't map.
        if (cp == 0 || cp > 0xFFFF || (keysym < 0x01000000 && keysym > 0xFF)) return;
        p.unicode = true;
        p.unit = static_cast<uint16_t>(cp);
        // Use the layout-aware VK path when the character is reachable with the modifiers currently held,
        // so shortcuts such as Ctrl+C work; otherwise inject the character itself.
        HKL layout = GetKeyboardLayout(0);
        SHORT scan = VkKeyScanExW(static_cast<WCHAR>(cp), layout);
        if (scan != -1) {
            const BYTE state = HIBYTE(scan);
            const bool needShift = state & 1, needCtrl = state & 2, needAlt = state & 4;
            const bool shortcut = ctrlDown_ || altDown_;
            if (shortcut || (needShift == shiftDown_ && !needCtrl && !needAlt)) {
                p.unicode = false;
                p.vk = LOBYTE(scan);
                p.extended = false;
            }
            (void)needCtrl; (void)needAlt;
        }
    }
    down_[keysym] = p;
    send(p, true);
}

void InputInjector::releaseAll() {
    for (auto& kv : down_) send(kv.second, false);
    down_.clear();
    shiftDown_ = ctrlDown_ = altDown_ = false;
    if (lastMask_) {
        INPUT in[3]{};
        int n = 0;
        if (lastMask_ & 1) { in[n].type = INPUT_MOUSE; in[n++].mi.dwFlags = MOUSEEVENTF_LEFTUP; }
        if (lastMask_ & 2) { in[n].type = INPUT_MOUSE; in[n++].mi.dwFlags = MOUSEEVENTF_MIDDLEUP; }
        if (lastMask_ & 4) { in[n].type = INPUT_MOUSE; in[n++].mi.dwFlags = MOUSEEVENTF_RIGHTUP; }
        if (n) SendInput(static_cast<UINT>(n), in, sizeof(INPUT));
        lastMask_ = 0;
    }
}

namespace {
bool isPrintableKeysym(uint32_t ks) {
    return (ks >= 0x20 && ks <= 0x7E) || (ks >= 0xA0 && ks <= 0xFF) || ks >= 0x01000000;
}
}  // namespace

void InputInjector::keyExt(bool down, uint32_t keysym, uint32_t keycode) {
    const uint32_t scanKey = 0x80000000u | keycode;
    if (!down) {
        auto it = down_.find(scanKey);
        if (keycode && it != down_.end()) {
            desktop::attachToInputDesktop();
            send(it->second, false);
            const uint16_t sc = it->second.scanCode;
            if (sc == 0x2A || sc == 0x36) shiftDown_ = false;
            if (sc == 0x1D) ctrlDown_ = false;
            if (sc == 0x38) altDown_ = false;
            down_.erase(it);
            return;
        }
        key(false, keysym);
        return;
    }
    const bool shortcut = ctrlDown_ || altDown_;
    if (keycode == 0 || (isPrintableKeysym(keysym) && !shortcut)) {
        key(true, keysym);
        return;
    }
    desktop::attachToInputDesktop();
    if (down_.count(scanKey)) {  // auto-repeat
        send(down_[scanKey], true);
        return;
    }
    Pressed p;
    p.scan = true;
    p.scanCode = static_cast<uint16_t>(keycode & 0x7F);
    p.extended = (keycode & 0x80) != 0;
    if (p.scanCode == 0x2A || p.scanCode == 0x36) shiftDown_ = true;
    if (p.scanCode == 0x1D) ctrlDown_ = true;
    if (p.scanCode == 0x38) altDown_ = true;
    down_[scanKey] = p;
    send(p, true);
}
