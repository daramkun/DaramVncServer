#pragma once
#include <cstdarg>
#include <string>

namespace logging {

// Timestamped line to stderr and, when set, to a log file (service mode).
void setLogFile(const std::string& path);
void write(const char* fmt, ...);

}  // namespace logging

#define LOGI(...) ::logging::write(__VA_ARGS__)
