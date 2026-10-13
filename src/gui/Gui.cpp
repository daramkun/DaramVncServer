#include "gui/Gui.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <string>

#include "config/Config.h"
#include "config/ConfigEdit.h"
#include "security/Auth.h"
#include "service/Service.h"

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace gui {

namespace {

// Control ids. The automated GUI test (tests/gui_test.py) uses the same numbers.
enum Id {
    kStatusText = 100, kFirewallText, kBtnInstall, kBtnUninstall, kBtnStart, kBtnStop, kBtnRestart, kBtnFwOpen, kBtnFwClose, kChkFwOnInstall,
    kBind = 120, kPort, kChkViewOnly, kChkH264, kChkLegacy,
    kJpegMax = 130, kJpegMin,
    kUser = 140, kBtnPassword, kBtnVncPassword, kAccountText,
    kLogPath = 150, kLogMaxMb, kLogKeep, kBtnOpenLog,
    kBtnSave = 160, kBtnSaveRestart, kBtnClose, kMessage,
};

constexpr UINT_PTR kRefreshTimer = 1;

struct App {
    HWND wnd = nullptr;
    HFONT font = nullptr;
    int dpi = 96;
    std::string configPath;
    ServerConfig cfg;  // what is in the file now (credentials included); the form edits a copy
} g;

int S(int v) { return MulDiv(v, g.dpi, 96); }

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring trimW(std::wstring s) {
    const wchar_t* ws = L" \t\r\n";
    s.erase(0, s.find_first_not_of(ws) == std::wstring::npos ? s.size() : s.find_first_not_of(ws));
    const size_t e = s.find_last_not_of(ws);
    s.erase(e == std::wstring::npos ? 0 : e + 1);
    return s;
}

HWND ctl(int id) { return GetDlgItem(g.wnd, id); }

std::wstring getText(int id) {
    HWND c = ctl(id);
    const int n = GetWindowTextLengthW(c);
    std::wstring t(static_cast<size_t>(n) + 1, L'\0');
    t.resize(static_cast<size_t>(GetWindowTextW(c, t.data(), n + 1)));
    return t;
}

void setText(int id, const std::wstring& t) { SetWindowTextW(ctl(id), t.c_str()); }
bool isChecked(int id) { return SendMessageW(ctl(id), BM_GETCHECK, 0, 0) == BST_CHECKED; }
void setChecked(int id, bool on) { SendMessageW(ctl(id), BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0); }

void setStatus(const std::wstring& t) { setText(kMessage, t); }

std::wstring describeError(int err) {
    switch (err) {
        case ERROR_ACCESS_DENIED: return L"권한이 없습니다. 관리자 권한으로 실행해야 합니다.";
        case ERROR_SERVICE_EXISTS: return L"이미 설치되어 있습니다.";
        case ERROR_SERVICE_DOES_NOT_EXIST: return L"서비스가 설치되어 있지 않습니다.";
        case ERROR_SERVICE_ALREADY_RUNNING: return L"이미 실행 중입니다.";
        case ERROR_SERVICE_NOT_ACTIVE: return L"실행 중이 아닙니다.";
        case WAIT_TIMEOUT: return L"서비스가 제때 중지되지 않았습니다.";
        default: break;
    }
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, static_cast<DWORD>(err), 0,
                   reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
    std::wstring text = msg ? trimW(msg) : L"";
    if (msg) LocalFree(msg);
    return text + L" (오류 " + std::to_wstring(err) + L")";
}

// ---- controls ------------------------------------------------------------------------------------------------

HWND make(const wchar_t* cls, const wchar_t* text, DWORD style, DWORD exStyle, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), g.wnd,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
    return c;
}

void group(const wchar_t* text, int x, int y, int w, int h) { make(L"BUTTON", text, BS_GROUPBOX, 0, x, y, w, h, 0); }
void label(const wchar_t* text, int x, int y, int w, int id = 0) { make(L"STATIC", text, SS_LEFT, 0, x, y + 3, w, 18, id); }
void edit(int id, int x, int y, int w, DWORD extra = 0) { make(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL | extra, WS_EX_CLIENTEDGE, x, y, w, 22, id); }
void check(int id, const wchar_t* text, int x, int y, int w) { make(L"BUTTON", text, WS_TABSTOP | BS_AUTOCHECKBOX, 0, x, y, w, 20, id); }
void button(int id, const wchar_t* text, int x, int y, int w, bool isDefault = false) {
    make(L"BUTTON", text, WS_TABSTOP | (isDefault ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON), 0, x, y, w, 26, id);
}

void buildWindow() {
    constexpr int kLeft = 24;  // left edge inside a group

    group(L"서비스", 10, 6, 540, 138);
    label(L"상태:", kLeft, 28, 80);
    label(L"", 108, 28, 300, kStatusText);
    label(L"방화벽 규칙:", kLeft, 54, 80);
    label(L"", 108, 54, 160, kFirewallText);
    button(kBtnFwOpen, L"규칙 만들기/갱신", 296, 50, 120);
    button(kBtnFwClose, L"규칙 삭제", 424, 50, 112);
    button(kBtnInstall, L"설치", kLeft, 84, 94);
    button(kBtnUninstall, L"제거", 126, 84, 94);
    button(kBtnStart, L"시작", 228, 84, 94);
    button(kBtnStop, L"중지", 330, 84, 94);
    button(kBtnRestart, L"재시작", 432, 84, 104);
    check(kChkFwOnInstall, L"설치할 때 방화벽 규칙도 만들기", kLeft, 116, 300);
    setChecked(kChkFwOnInstall, true);

    group(L"접속", 10, 152, 540, 112);
    label(L"수신 주소:", kLeft, 176, 90);
    make(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWN | CBS_AUTOHSCROLL, 0, 120, 174, 200, 140, kBind);
    for (const wchar_t* item : {L"0.0.0.0", L"::", L"127.0.0.1", L"::1"}) SendMessageW(ctl(kBind), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
    label(L"포트:", 340, 176, 40);
    edit(kPort, 384, 174, 70);
    check(kChkViewOnly, L"보기 전용 (키보드·마우스·클립보드 입력 무시)", kLeft, 204, 330);
    check(kChkH264, L"H.264 사용", 370, 204, 160);
    check(kChkLegacy, L"RealVNC 호환 인증 (TLS 없음, VNC 비밀번호 필요)", kLeft, 230, 510);

    group(L"화질 (JPEG)", 10, 272, 540, 56);
    label(L"상한 (1~99):", kLeft, 294, 90);
    edit(kJpegMax, 120, 292, 60);
    label(L"하한 (1~99):", 220, 294, 90);
    edit(kJpegMin, 316, 292, 60);

    group(L"계정", 10, 336, 540, 86);
    label(L"로그인 사용자:", kLeft, 360, 90);
    edit(kUser, 120, 358, 140);
    button(kBtnPassword, L"비밀번호 설정...", 276, 356, 130);
    button(kBtnVncPassword, L"VNC 비밀번호 설정...", 412, 356, 124);
    label(L"", kLeft, 390, 500, kAccountText);

    group(L"로그", 10, 430, 540, 86);
    label(L"파일:", kLeft, 454, 40);
    edit(kLogPath, 68, 452, 340);
    button(kBtnOpenLog, L"로그 열기", 416, 450, 120);
    label(L"회전 크기(MB, 0=안 함):", kLeft, 486, 150);
    edit(kLogMaxMb, 180, 484, 56);
    label(L"보관 수:", 260, 486, 60);
    edit(kLogKeep, 322, 484, 50);

    button(kBtnSave, L"저장", 10, 530, 110, true);
    button(kBtnSaveRestart, L"저장 후 서비스 재시작", 128, 530, 170);
    button(kBtnClose, L"닫기", 440, 530, 110);
    label(L"", 10, 566, 540, kMessage);
}

// ---- state <-> controls ---------------------------------------------------------------------------------------

void showAccount() {
    std::wstring t = g.cfg.passwordHash.empty() ? L"로그인 비밀번호: 설정 안 됨" : L"로그인 비밀번호: 설정됨";
    t += g.cfg.vncPasswordProtected.empty() ? L"   |   VNC 비밀번호: 설정 안 됨" : L"   |   VNC 비밀번호: 설정됨";
    if (!g.cfg.hasCredentials() && !g.cfg.isLoopbackOnly())
        t = L"경고: 비밀번호 설정 안 됨 — 누구나 접속 가능 (암호화 없음)";
    setText(kAccountText, t);
}

void configToControls() {
    setText(kBind, widen(g.cfg.bind));
    setText(kPort, std::to_wstring(g.cfg.port));
    setChecked(kChkViewOnly, g.cfg.viewOnly);
    setChecked(kChkH264, g.cfg.h264);
    setChecked(kChkLegacy, g.cfg.allowLegacyVncAuth);
    setText(kJpegMax, std::to_wstring(g.cfg.jpegQualityMax));
    setText(kJpegMin, std::to_wstring(g.cfg.jpegQualityMin));
    setText(kUser, widen(g.cfg.user));
    setText(kLogPath, widen(g.cfg.logFile));
    setText(kLogMaxMb, std::to_wstring(g.cfg.logMaxMb));
    setText(kLogKeep, std::to_wstring(g.cfg.logKeep));
    showAccount();
}

bool parseInt(int id, const wchar_t* what, int& out, std::vector<std::string>& problems) {
    const std::wstring t = trimW(getText(id));
    bool ok = !t.empty() && t.size() <= 6;
    for (wchar_t c : t) ok = ok && c >= L'0' && c <= L'9';
    if (!ok) {
        problems.push_back(narrow(std::wstring(what) + L": 숫자(정수)를 입력하세요."));
        return false;
    }
    out = std::stoi(t);
    return true;
}

// Fills `cfg` from the form. Returns the problems found (unparsable numbers plus validateConfig's findings).
std::vector<std::string> controlsToConfig(ServerConfig& cfg) {
    std::vector<std::string> problems;
    cfg = g.cfg;  // keeps credentials, certificate paths and anything the form does not show
    cfg.bind = narrow(trimW(getText(kBind)));
    cfg.viewOnly = isChecked(kChkViewOnly);
    cfg.h264 = isChecked(kChkH264);
    cfg.allowLegacyVncAuth = isChecked(kChkLegacy);
    cfg.user = narrow(trimW(getText(kUser)));
    cfg.logFile = narrow(trimW(getText(kLogPath)));
    bool numbersOk = parseInt(kPort, L"포트", cfg.port, problems);
    numbersOk &= parseInt(kJpegMax, L"JPEG 품질 상한", cfg.jpegQualityMax, problems);
    numbersOk &= parseInt(kJpegMin, L"JPEG 품질 하한", cfg.jpegQualityMin, problems);
    numbersOk &= parseInt(kLogMaxMb, L"로그 회전 크기", cfg.logMaxMb, problems);
    numbersOk &= parseInt(kLogKeep, L"로그 보관 수", cfg.logKeep, problems);
    if (numbersOk) problems = validateConfig(cfg);
    return problems;
}

void showProblems(const std::vector<std::string>& problems) {
    std::wstring text = L"설정을 저장할 수 없습니다.\n";
    for (const std::string& p : problems) text += L"\n• " + widen(p);
    MessageBoxW(g.wnd, text.c_str(), L"Daram VNC Server", MB_OK | MB_ICONWARNING);
    setStatus(L"저장하지 않았습니다: 설정을 확인하세요.");
}

// ---- service ------------------------------------------------------------------------------------------------

void refreshService() {
    using service::State;
    const State st = service::queryState();
    const wchar_t* text = L"알 수 없음";
    switch (st) {
        case State::NotInstalled: text = L"설치되어 있지 않음"; break;
        case State::Stopped: text = L"중지됨"; break;
        case State::Starting: text = L"시작하는 중..."; break;
        case State::Running: text = L"실행 중"; break;
        case State::Stopping: text = L"중지하는 중..."; break;
        case State::Unknown: break;
    }
    setText(kStatusText, text);
    const bool installed = st != State::NotInstalled && st != State::Unknown;
    EnableWindow(ctl(kBtnInstall), st == State::NotInstalled);
    EnableWindow(ctl(kBtnUninstall), installed);
    EnableWindow(ctl(kBtnStart), st == State::Stopped);
    EnableWindow(ctl(kBtnStop), st == State::Running);
    EnableWindow(ctl(kBtnRestart), st == State::Running || st == State::Stopped);
    EnableWindow(ctl(kBtnSaveRestart), st == State::Running || st == State::Stopped);
}

void refreshFirewall() { setText(kFirewallText, service::firewallRuleExists() ? L"있음" : L"없음"); }

// Runs a slow operation with the wait cursor, then reports its result in the message line.
template <class F>
void doService(const wchar_t* doing, const wchar_t* done, F&& op) {
    setStatus(doing);
    UpdateWindow(g.wnd);
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    const int err = op();
    SetCursor(old);
    setStatus(err ? std::wstring(L"실패: ") + describeError(err) : std::wstring(done));
    refreshService();
    refreshFirewall();
}

bool saveForm() {
    ServerConfig edited;
    const std::vector<std::string> problems = controlsToConfig(edited);
    if (!problems.empty()) {
        showProblems(problems);
        return false;
    }
    if (!saveEditableConfig(g.configPath, edited) || (!edited.user.empty() && !g.cfg.passwordHash.empty() && !updateConfigKey(g.configPath, "user", edited.user))) {
        MessageBoxW(g.wnd, (L"설정 파일에 쓸 수 없습니다.\n" + widen(g.configPath) + L"\n\n관리자 권한이 필요할 수 있습니다.").c_str(), L"Daram VNC Server", MB_OK | MB_ICONERROR);
        setStatus(L"저장하지 못했습니다.");
        return false;
    }
    g.cfg = edited;
    const service::State st = service::queryState();
    setStatus(st == service::State::Running ? L"저장했습니다. 실행 중인 서비스에는 재시작해야 적용됩니다." : L"저장했습니다.");
    showAccount();
    if (!g.cfg.hasCredentials() && !g.cfg.isLoopbackOnly())
        setStatus(L"저장했습니다. 경고: 인증·암호화 없이 접속 허용. 실행 중이면 재시작하세요.");
    return true;
}

// ---- password dialog ------------------------------------------------------------------------------------------

struct PasswordDialog {
    HWND wnd = nullptr, first = nullptr, second = nullptr;
    bool done = false, accepted = false;
    std::wstring password;
};

LRESULT CALLBACK passwordProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* d = reinterpret_cast<PasswordDialog*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (m) {
        case WM_NCCREATE:
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
            break;
        case WM_COMMAND:
            if (LOWORD(w) == IDOK && d) {
                wchar_t a[256] = {}, b[256] = {};
                GetWindowTextW(d->first, a, 256);
                GetWindowTextW(d->second, b, 256);
                if (std::wstring(a).size() < 8) { MessageBoxW(h, L"비밀번호는 8자 이상이어야 합니다.", L"비밀번호", MB_OK | MB_ICONWARNING); return 0; }
                if (std::wstring(a) != b) { MessageBoxW(h, L"두 비밀번호가 같지 않습니다.", L"비밀번호", MB_OK | MB_ICONWARNING); return 0; }
                d->password = a;
                d->accepted = d->done = true;
                return 0;
            }
            if (LOWORD(w) == IDCANCEL && d) { d->done = true; return 0; }
            break;
        case WM_CLOSE:
            if (d) d->done = true;
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// Modal "new password twice" dialog. Returns false when cancelled.
bool askPassword(const wchar_t* title, const wchar_t* hint, std::string& password) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = passwordProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"DaramVncPasswordDialog";
        RegisterClassW(&wc);
        registered = true;
    }
    PasswordDialog d;
    RECT r{0, 0, S(360), S(176)};
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRectEx(&r, style, FALSE, WS_EX_DLGMODALFRAME);
    RECT owner;
    GetWindowRect(g.wnd, &owner);
    d.wnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"DaramVncPasswordDialog", title, style, owner.left + S(60), owner.top + S(80), r.right - r.left, r.bottom - r.top,
                            g.wnd, nullptr, GetModuleHandleW(nullptr), &d);
    auto child = [&](const wchar_t* cls, const wchar_t* text, DWORD st, DWORD ex, int x, int y, int w, int h, int id) {
        HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | st, S(x), S(y), S(w), S(h), d.wnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
        return c;
    };
    child(L"STATIC", hint, SS_LEFT, 0, 14, 12, 332, 36, 0);
    child(L"STATIC", L"새 비밀번호:", SS_LEFT, 0, 14, 58, 100, 18, 0);
    d.first = child(L"EDIT", L"", WS_TABSTOP | ES_PASSWORD | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, 120, 55, 226, 22, 0);
    child(L"STATIC", L"다시 입력:", SS_LEFT, 0, 14, 88, 100, 18, 0);
    d.second = child(L"EDIT", L"", WS_TABSTOP | ES_PASSWORD | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, 120, 85, 226, 22, 0);
    child(L"BUTTON", L"확인", WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 160, 128, 90, 26, IDOK);
    child(L"BUTTON", L"취소", WS_TABSTOP | BS_PUSHBUTTON, 0, 256, 128, 90, 26, IDCANCEL);

    EnableWindow(g.wnd, FALSE);
    ShowWindow(d.wnd, SW_SHOW);
    ShowWindow(d.wnd, SW_SHOW);
    SetFocus(d.first);
    MSG msg;
    while (!d.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(d.wnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(g.wnd, TRUE);
    DestroyWindow(d.wnd);
    SetForegroundWindow(g.wnd);
    if (d.accepted) password = narrow(d.password);
    SecureZeroMemory(d.password.data(), d.password.size() * sizeof(wchar_t));
    return d.accepted;
}

void setLoginPassword() {
    const std::string user = narrow(trimW(getText(kUser)));
    if (user.empty()) {
        MessageBoxW(g.wnd, L"로그인 사용자 이름을 먼저 입력하세요.", L"비밀번호", MB_OK | MB_ICONWARNING);
        return;
    }
    std::string pw;
    if (!askPassword(L"로그인 비밀번호", (L"사용자 '" + widen(user) + L"'의 비밀번호를 정합니다. (8자 이상)").c_str(), pw)) return;
    const std::string hash = auth::hashPassword(pw);
    SecureZeroMemory(pw.data(), pw.size());
    if (hash.empty() || !updateConfigKey(g.configPath, "user", user) || !updateConfigKey(g.configPath, "password_hash", hash)) {
        MessageBoxW(g.wnd, L"설정 파일에 쓸 수 없습니다. 관리자 권한이 필요할 수 있습니다.", L"비밀번호", MB_OK | MB_ICONERROR);
        return;
    }
    g.cfg.user = user;
    g.cfg.passwordHash = hash;
    showAccount();
    setStatus(L"로그인 비밀번호를 저장했습니다. 서비스를 재시작해야 적용됩니다.");
}

void setVncPassword() {
    std::string pw;
    if (!askPassword(L"VNC 비밀번호", L"8자 이상 입력하세요. 인증에는 첫 8바이트만 사용됩니다. RealVNC는 접속 설정의 호환 인증도 켜야 합니다.", pw)) return;
    const std::string sealed = auth::protectSecret(pw);
    SecureZeroMemory(pw.data(), pw.size());
    if (sealed.empty() || !updateConfigKey(g.configPath, "vnc_password_dpapi", sealed)) {
        MessageBoxW(g.wnd, L"설정 파일에 쓸 수 없습니다. 관리자 권한이 필요할 수 있습니다.", L"비밀번호", MB_OK | MB_ICONERROR);
        return;
    }
    g.cfg.vncPasswordProtected = sealed;
    showAccount();
    setStatus(L"VNC 비밀번호를 저장했습니다. 서비스를 재시작해야 적용됩니다.");
}

void openLog() {
    std::string path = g.cfg.logFile.empty() ? exeDirectory() + "\\daramvnc.log" : g.cfg.logFile;
    if (GetFileAttributesW(widen(path).c_str()) == INVALID_FILE_ATTRIBUTES) {
        setStatus(L"로그 파일이 아직 없습니다: " + widen(path));
        return;
    }
    ShellExecuteW(g.wnd, L"open", L"notepad.exe", (L"\"" + widen(path) + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
}

// ---- window ---------------------------------------------------------------------------------------------------

LRESULT CALLBACK windowProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_CREATE:
            g.wnd = h;
            buildWindow();
            configToControls();
            refreshService();
            refreshFirewall();
            SetTimer(h, kRefreshTimer, 2000, nullptr);
            return 0;
        case WM_TIMER:
            if (w == kRefreshTimer) refreshService();
            return 0;
        case WM_COMMAND:
            if (HIWORD(w) != BN_CLICKED) break;
            switch (LOWORD(w)) {
                case kBtnInstall:
                    doService(L"서비스를 설치하는 중...", L"서비스를 설치했습니다. 인증 정보를 설정한 뒤 '시작'을 누르세요.",
                              [] { return service::install(g.configPath, g.cfg.port, isChecked(kChkFwOnInstall)); });
                    break;
                case kBtnUninstall:
                    if (MessageBoxW(h, L"서비스를 중지하고 제거합니다. 방화벽 규칙도 삭제됩니다.\n계속할까요?", L"제거", MB_YESNO | MB_ICONQUESTION) == IDYES)
                        doService(L"서비스를 제거하는 중...", L"서비스를 제거했습니다.", [] { return service::uninstall(); });
                    break;
                case kBtnStart: doService(L"서비스를 시작하는 중...", L"서비스를 시작했습니다.", [] { return service::start(); }); break;
                case kBtnStop: doService(L"서비스를 중지하는 중...", L"서비스를 중지했습니다. 접속 중이던 뷰어는 끊어집니다.", [] { return service::stop(); }); break;
                case kBtnRestart: doService(L"서비스를 재시작하는 중...", L"서비스를 재시작했습니다.", [] { return service::restart(); }); break;
                case kBtnFwOpen: doService(L"방화벽 규칙을 만드는 중...", L"방화벽 규칙을 만들었습니다.", [] { return service::openFirewall(g.cfg.port); }); break;
                case kBtnFwClose: doService(L"방화벽 규칙을 삭제하는 중...", L"방화벽 규칙을 삭제했습니다.", [] { return service::closeFirewall(); }); break;
                case kBtnPassword: setLoginPassword(); break;
                case kBtnVncPassword: setVncPassword(); break;
                case kBtnOpenLog: openLog(); break;
                case kBtnSave: saveForm(); break;
                case kBtnSaveRestart:
                    if (saveForm()) doService(L"서비스를 재시작하는 중...", L"저장하고 서비스를 재시작했습니다.", [] { return service::restart(); });
                    break;
                case kBtnClose: DestroyWindow(h); break;
            }
            return 0;
        case WM_DESTROY:
            KillTimer(h, kRefreshTimer);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

bool isElevated() {
    HANDLE token = nullptr;
    TOKEN_ELEVATION e{};
    DWORD n = 0;
    const bool ok = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) && GetTokenInformation(token, TokenElevation, &e, sizeof e, &n);
    if (token) CloseHandle(token);
    return ok && e.TokenIsElevated;
}

// Starts this executable again with the "runas" verb (UAC prompt). Returns false when the user declined.
bool relaunchElevated(const std::string& configPath) {
    wchar_t exe[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    const std::wstring file(exe, n);
    const std::wstring args = L"gui --config \"" + widen(configPath) + L"\"";
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof sei;
    sei.lpVerb = L"runas";
    sei.lpFile = file.c_str();
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;  // the console of this console-subsystem exe stays invisible; the GUI shows its own window
    return ShellExecuteExW(&sei) != FALSE;
}

}  // namespace

int run(const std::string& configPath, bool noElevate) {
    if (!noElevate && !isElevated()) {
        if (relaunchElevated(configPath)) return 0;
        MessageBoxW(nullptr, L"서비스 제어와 설정 저장에는 관리자 권한이 필요합니다.\n권한 상승을 허용해야 설정 창을 열 수 있습니다.", L"Daram VNC Server", MB_OK | MB_ICONINFORMATION);
        return 1;
    }
    FreeConsole();  // this is a console-subsystem exe: do not keep an empty console window around
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    g.configPath = configPath;
    loadConfig(configPath, g.cfg);
    g.dpi = static_cast<int>(GetDpiForSystem());
    NONCLIENTMETRICSW ncm{sizeof ncm};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    g.font = CreateFontIndirectW(&ncm.lfMessageFont);

    WNDCLASSW wc{};
    wc.lpfnWndProc = windowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = kWindowClass;
    RegisterClassW(&wc);

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r{0, 0, S(560), S(594)};
    AdjustWindowRectEx(&r, style, FALSE, 0);
    const std::wstring title = L"Daram VNC Server 설정";
    HWND wnd = CreateWindowExW(0, kWindowClass, title.c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr,
                               wc.hInstance, nullptr);
    if (!wnd) return 1;
    // The first ShowWindow call of a process obeys STARTUPINFO (SW_HIDE when we were relaunched hidden), so call it twice.
    ShowWindow(wnd, SW_SHOWNORMAL);
    ShowWindow(wnd, SW_SHOWNORMAL);
    UpdateWindow(wnd);
    SetForegroundWindow(wnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(wnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    DeleteObject(g.font);
    return 0;
}

}  // namespace gui
