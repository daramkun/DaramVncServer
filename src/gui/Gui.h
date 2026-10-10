#pragma once
#include <string>

namespace gui {

// Settings window: controls the Windows service and edits daramvnc.conf. Service control and writing the config next
// to the executable need Administrator rights, so unless `noElevate` is set the window relaunches itself elevated
// (UAC prompt) first. `noElevate` exists for automated tests that use a config file in a temp folder.
int run(const std::string& configPath, bool noElevate);

// Window class name, used by the automated GUI test to find the window.
constexpr const wchar_t* kWindowClass = L"DaramVncSettingsWindow";

}  // namespace gui
