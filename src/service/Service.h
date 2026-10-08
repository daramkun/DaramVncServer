#pragma once
#include <string>

namespace service {

constexpr const wchar_t* kServiceName = L"DaramVncServer";

// SCM entry point for `daramvnc service`. Supervises an agent process inside the active console session.
int runDispatcher();

// Management helpers (require Administrator rights). Return a Win32 error code, 0 on success.
int install(const std::string& configPath, int port, bool openFirewall);
int uninstall();
int start();
int stop();

}  // namespace service
