#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#include "config/Config.h"
#include "rfb/Server.h"
#include "security/Auth.h"
#include "service/Service.h"
#include "util/Log.h"

namespace {

const char* kUsage =
    "DaramVncServer - high-performance VNC (RFB) server for Windows\n"
    "\n"
    "usage: daramvnc [run] [--port N] [--bind ADDR] [--config FILE] [--view-only] [--h264]   run in the foreground\n"
    "       daramvnc passwd <user>        set the login (VeNCrypt X509Plain over TLS) user and password\n"
    "       daramvnc vncpasswd            set an optional VNC password (X509Vnc over TLS; legacy VNC auth if enabled)\n"
    "       daramvnc install [--open-firewall]   install and start the Windows service (Administrator)\n"
    "       daramvnc uninstall | start | stop    manage the service (Administrator)\n"
    "\n"
    "Settings live in daramvnc.conf next to the executable (bind, port, user, password_hash, ...).\n"
    "Without credentials the server only accepts connections from this machine (127.0.0.1).\n";

std::string readSecret(const char* prompt) {
    std::fputs(prompt, stderr);
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    const bool tty = GetConsoleMode(in, &mode) != 0;
    if (tty) SetConsoleMode(in, mode & ~ENABLE_ECHO_INPUT);
    std::string s;
    std::getline(std::cin, s);
    if (tty) {
        SetConsoleMode(in, mode);
        std::fputs("\n", stderr);
    }
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
    return s;
}

std::string readNewPassword() {
    const std::string a = readSecret("New password: ");
    if (a.size() < 8) {
        std::fprintf(stderr, "Password must be at least 8 characters.\n");
        return {};
    }
    if (readSecret("Repeat password: ") != a) {
        std::fprintf(stderr, "Passwords do not match.\n");
        return {};
    }
    return a;
}

int runServer(ServerConfig cfg, bool agent) {
    if (agent) {
        logging::setLogFile(cfg.logFile.empty() ? exeDirectory() + "\\daramvnc.log" : cfg.logFile);
    }
    // Pixel coordinates must match what capture and input use, on any DPI setting.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Server server(cfg);
    std::string err;
    if (!server.start(&err)) {
        LOGI("cannot start: %s", err.c_str());
        return 1;
    }
    if (agent) {
        Sleep(INFINITE);  // the service terminates us
    } else {
        std::fprintf(stderr, "Press Enter to stop.\n");
        std::string line;
        std::getline(std::cin, line);
    }
    server.stop();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string cmd = argc > 1 ? argv[1] : "run";
    if (cmd == "-h" || cmd == "--help" || cmd == "help") {
        std::fputs(kUsage, stdout);
        return 0;
    }

    std::string configPath = defaultConfigPath();
    ServerConfig cfg;
    std::string portArg, bindArg;
    bool openFirewall = false, viewOnlyArg = false, h264Arg = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--config" && i + 1 < argc) configPath = argv[++i];
        else if (a == "--port" && i + 1 < argc) portArg = argv[++i];
        else if (a == "--bind" && i + 1 < argc) bindArg = argv[++i];
        else if (a == "--open-firewall") openFirewall = true;
        else if (a == "--view-only") viewOnlyArg = true;
        else if (a == "--h264") h264Arg = true;
    }
    loadConfig(configPath, cfg);
    if (cfg.certFile.empty()) cfg.certFile = exeDirectory() + "\\server.crt";
    if (cfg.keyFile.empty()) cfg.keyFile = exeDirectory() + "\\server.key";
    if (!portArg.empty()) cfg.port = std::atoi(portArg.c_str());
    if (!bindArg.empty()) cfg.bind = bindArg;
    if (viewOnlyArg) cfg.viewOnly = true;
    if (h264Arg) cfg.h264 = true;

    if (cmd == "passwd") {
        if (argc < 3 || argv[2][0] == '-') { std::fputs(kUsage, stderr); return 2; }
        const std::string pw = readNewPassword();
        if (pw.empty()) return 1;
        const std::string hash = auth::hashPassword(pw);
        if (hash.empty() || !updateConfigKey(configPath, "user", argv[2]) || !updateConfigKey(configPath, "password_hash", hash)) {
            std::fprintf(stderr, "Could not write %s\n", configPath.c_str());
            return 1;
        }
        std::printf("Saved user '%s' to %s\nSet bind=0.0.0.0 in that file (or pass --bind) to accept remote connections.\n", argv[2], configPath.c_str());
        return 0;
    }
    if (cmd == "vncpasswd") {
        const std::string pw = readNewPassword();
        if (pw.empty()) return 1;
        const std::string sealed = auth::protectSecret(pw);
        if (sealed.empty() || !updateConfigKey(configPath, "vnc_password_dpapi", sealed)) {
            std::fprintf(stderr, "Could not store the password in %s\n", configPath.c_str());
            return 1;
        }
        std::printf("VNC password stored (DPAPI-protected). Only the first 8 characters take part in legacy VNC authentication.\n"
                    "Legacy VNC authentication without TLS is weak; it stays off unless allow_legacy_vnc_auth=1 is set.\n");
        return 0;
    }
    if (cmd == "install" || cmd == "uninstall" || cmd == "start" || cmd == "stop") {
        int err = cmd == "install" ? service::install(configPath, cfg.port, openFirewall)
                : cmd == "uninstall" ? service::uninstall()
                : cmd == "start" ? service::start() : service::stop();
        if (err) {
            std::fprintf(stderr, "%s failed (Win32 error %d)%s\n", cmd.c_str(), err, err == 5 ? " - run as Administrator" : "");
            return 1;
        }
        if (cmd == "install") {
            std::printf("Service installed. Configure credentials with 'daramvnc passwd <user>', then 'daramvnc start'.\n");
        }
        return 0;
    }
    if (cmd == "service") {
        const int err = service::runDispatcher();
        if (err) std::fprintf(stderr, "Not started by the service manager (error %d). Use 'daramvnc run' for the console.\n", err);
        return err ? 1 : 0;
    }
    if (cmd == "agent") return runServer(cfg, true);
    if (cmd == "run" || cmd[0] == '-') return runServer(cfg, false);

    std::fputs(kUsage, stderr);
    return 2;
}
