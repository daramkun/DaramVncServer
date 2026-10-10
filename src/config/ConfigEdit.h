#pragma once
#include <string>
#include <vector>

#include "config/Config.h"

// Validation and saving of the settings the GUI edits. Kept apart from the window code so it can be unit tested.

// Returns one human-readable (Korean) message per problem; empty means the settings can be used as they are.
// Also catches the combination the server refuses at start-up: no credentials on a non-loopback address.
std::vector<std::string> validateConfig(const ServerConfig& cfg);

// True for "localhost", an IPv4 address or an IPv6 address (what Server accepts as `bind`).
bool isValidBindAddress(const std::string& bind);

// Writes the GUI-editable keys (bind, port, view_only, allow_legacy_vnc_auth, h264, jpeg_quality_*, log, log_*)
// into the config file, keeping every other line (credentials, certificate paths, comments) as it is.
bool saveEditableConfig(const std::string& path, const ServerConfig& cfg);
