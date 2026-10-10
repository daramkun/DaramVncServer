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
int restart();  // stops the service (waits up to 20 s for it to end) and starts it again

enum class State { NotInstalled, Stopped, Starting, Running, Stopping, Unknown };
State queryState();  // needs no special rights

// Inbound TCP rule "DaramVncServer" for this executable. openFirewall replaces an existing rule, so it also updates the port.
int openFirewall(int port);
int closeFirewall();
bool firewallRuleExists();

}  // namespace service
