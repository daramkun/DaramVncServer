#pragma once
#include <string>

namespace desktop {

// Attaches the calling thread to the desktop that currently receives input (Default, Winlogon/UAC secure
// desktop, ...). Works only with sufficient rights (SYSTEM for Winlogon) and from a thread that owns no windows.
// Throttled per thread. Returns true when the thread moved to a different desktop since the previous call.
bool attachToInputDesktop(std::string* name = nullptr);

}  // namespace desktop
