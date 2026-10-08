#include "service/Service.h"

#include <windows.h>
#include <userenv.h>
#include <wtsapi32.h>

#include <atomic>
#include <string>

#include "config/Config.h"
#include "util/Log.h"

namespace service {

namespace {

SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
SERVICE_STATUS g_status{};
HANDLE g_stopEvent = nullptr;
HANDLE g_wakeEvent = nullptr;  // session changed: re-evaluate which session the agent should run in

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::wstring exePath() {
    wchar_t p[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, p, static_cast<DWORD>(std::size(p)));
    return std::wstring(p, n);
}

void report(DWORD state, DWORD exitCode = NO_ERROR, DWORD waitHint = 0) {
    static DWORD checkpoint = 1;
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState = state;
    g_status.dwControlsAccepted =
        state == SERVICE_START_PENDING ? 0 : (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_SESSIONCHANGE);
    g_status.dwWin32ExitCode = exitCode;
    g_status.dwWaitHint = waitHint;
    g_status.dwCheckPoint = (state == SERVICE_RUNNING || state == SERVICE_STOPPED) ? 0 : checkpoint++;
    SetServiceStatus(g_statusHandle, &g_status);
}

DWORD WINAPI handler(DWORD control, DWORD eventType, LPVOID, LPVOID) {
    switch (control) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            report(SERVICE_STOP_PENDING, NO_ERROR, 5000);
            SetEvent(g_stopEvent);
            return NO_ERROR;
        case SERVICE_CONTROL_SESSIONCHANGE:
            if (eventType == WTS_CONSOLE_CONNECT || eventType == WTS_CONSOLE_DISCONNECT || eventType == WTS_SESSION_LOGON ||
                eventType == WTS_SESSION_LOGOFF || eventType == WTS_REMOTE_CONNECT || eventType == WTS_REMOTE_DISCONNECT)
                SetEvent(g_wakeEvent);
            return NO_ERROR;
        case SERVICE_CONTROL_INTERROGATE:
            return NO_ERROR;
    }
    return ERROR_CALL_NOT_IMPLEMENTED;
}

// Starts `daramvnc agent` as SYSTEM inside `session`, on the interactive desktop. Running as SYSTEM lets the
// agent follow the input desktop onto the Winlogon/UAC secure desktop.
HANDLE launchAgent(DWORD session) {
    HANDLE self = nullptr, token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &self)) return nullptr;
    const BOOL dup = DuplicateTokenEx(self, MAXIMUM_ALLOWED, nullptr, SecurityIdentification, TokenPrimary, &token);
    CloseHandle(self);
    if (!dup) return nullptr;
    SetTokenInformation(token, TokenSessionId, &session, sizeof session);

    LPVOID env = nullptr;
    CreateEnvironmentBlock(&env, token, FALSE);
    std::wstring cmd = L"\"" + exePath() + L"\" agent";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    wchar_t desk[] = L"winsta0\\default";
    si.lpDesktop = desk;
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessAsUserW(token, nullptr, cmd.data(), nullptr, nullptr, FALSE,
                                         CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, env, nullptr, &si, &pi);
    if (env) DestroyEnvironmentBlock(env);
    CloseHandle(token);
    if (!ok) {
        LOGI("CreateProcessAsUser failed: %lu", GetLastError());
        return nullptr;
    }
    CloseHandle(pi.hThread);
    LOGI("agent started in session %lu (pid %lu)", session, pi.dwProcessId);
    return pi.hProcess;
}

void supervise() {
    HANDLE agent = nullptr;
    DWORD agentSession = 0xFFFFFFFF;
    auto killAgent = [&] {
        if (!agent) return;
        TerminateProcess(agent, 0);
        WaitForSingleObject(agent, 5000);
        CloseHandle(agent);
        agent = nullptr;
    };

    for (;;) {
        const DWORD session = WTSGetActiveConsoleSessionId();
        if (agent && (session != agentSession || WaitForSingleObject(agent, 0) == WAIT_OBJECT_0)) {
            killAgent();
            Sleep(500);  // give the listening port a moment to be released
        }
        if (!agent && session != 0xFFFFFFFF) {
            agent = launchAgent(session);
            agentSession = session;
        }
        HANDLE waits[3] = {g_stopEvent, g_wakeEvent, agent};
        const DWORD n = agent ? 3 : 2;
        const DWORD r = WaitForMultipleObjects(n, waits, FALSE, agent ? INFINITE : 3000);
        if (r == WAIT_OBJECT_0) break;
        if (r == WAIT_OBJECT_0 + 2) Sleep(2000);  // agent exited on its own: restart after a pause
    }
    killAgent();
}

void WINAPI serviceMain(DWORD, LPWSTR*) {
    g_statusHandle = RegisterServiceCtrlHandlerExW(kServiceName, handler, nullptr);
    if (!g_statusHandle) return;
    report(SERVICE_START_PENDING, NO_ERROR, 3000);
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ServerConfig cfg;
    loadConfig(defaultConfigPath(), cfg);
    logging::setLogFile(cfg.logFile.empty() ? exeDirectory() + "\\daramvnc.log" : cfg.logFile,
                        static_cast<size_t>(cfg.logMaxMb) * 1024 * 1024, cfg.logKeep);
    logging::setConsoleOutput(false);
    report(SERVICE_RUNNING);
    supervise();
    report(SERVICE_STOPPED);
}

int scmError() { return static_cast<int>(GetLastError()); }

}  // namespace

int runDispatcher() {
    SERVICE_TABLE_ENTRYW table[] = {{const_cast<LPWSTR>(kServiceName), serviceMain}, {nullptr, nullptr}};
    return StartServiceCtrlDispatcherW(table) ? 0 : scmError();
}

int install(const std::string& configPath, int port, bool openFirewall) {
    (void)configPath;
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!scm) return scmError();
    const std::wstring bin = L"\"" + exePath() + L"\" service";
    SC_HANDLE svc = CreateServiceW(scm, kServiceName, L"Daram VNC Server", SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
                                   SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, bin.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);
    int err = svc ? 0 : scmError();
    if (svc) {
        wchar_t desc[] = L"High-performance VNC (RFB) server for Windows";
        SERVICE_DESCRIPTIONW d{desc};
        ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &d);
        SC_ACTION actions[] = {{SC_ACTION_RESTART, 5000}, {SC_ACTION_RESTART, 15000}, {SC_ACTION_NONE, 0}};
        SERVICE_FAILURE_ACTIONSW fa{86400, nullptr, nullptr, 3, actions};
        ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    if (!err && openFirewall) {
        const std::wstring cmd = L"netsh advfirewall firewall add rule name=\"DaramVncServer\" dir=in action=allow protocol=TCP localport=" +
                                 std::to_wstring(port) + L" program=\"" + exePath() + L"\" >nul";
        _wsystem(cmd.c_str());
    }
    return err;
}

int uninstall() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return scmError();
    SC_HANDLE svc = OpenServiceW(scm, kServiceName, DELETE | SERVICE_STOP);
    int err = 0;
    if (svc) {
        SERVICE_STATUS st;
        ControlService(svc, SERVICE_CONTROL_STOP, &st);
        err = DeleteService(svc) ? 0 : scmError();
        CloseServiceHandle(svc);
    } else {
        err = scmError();
    }
    CloseServiceHandle(scm);
    _wsystem(L"netsh advfirewall firewall delete rule name=\"DaramVncServer\" >nul 2>&1");
    return err;
}

int start() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return scmError();
    SC_HANDLE svc = OpenServiceW(scm, kServiceName, SERVICE_START);
    int err = svc ? (StartServiceW(svc, 0, nullptr) ? 0 : scmError()) : scmError();
    if (svc) CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return err;
}

int stop() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return scmError();
    SC_HANDLE svc = OpenServiceW(scm, kServiceName, SERVICE_STOP);
    SERVICE_STATUS st;
    int err = svc ? (ControlService(svc, SERVICE_CONTROL_STOP, &st) ? 0 : scmError()) : scmError();
    if (svc) CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return err;
}

}  // namespace service
