#pragma once
#include <cstdarg>
#include <cstddef>
#include <string>

namespace logging {

// Timestamped line to stderr and, when a file is set (service mode), to that file.
//
// The file is rotated by size so a long-running service cannot fill the disk: once it would grow past `maxBytes`
// it is renamed to <path>.1, the previous <path>.1 becomes <path>.2, and so on; at most `keep` old files stay
// (keep = 0 simply starts the file over). maxBytes = 0 turns rotation off.
void setLogFile(const std::string& path, size_t maxBytes = 5u * 1024 * 1024, int keep = 3);
// Console (stderr) output; a service has no console, tests do not want the noise.
void setConsoleOutput(bool enabled);
void write(const char* fmt, ...);

}  // namespace logging

#define LOGI(...) ::logging::write(__VA_ARGS__)
