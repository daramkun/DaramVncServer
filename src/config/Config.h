#pragma once
#include <string>

struct ServerConfig {
    std::string bind = "127.0.0.1";
    int port = 5900;

    // VeNCrypt X509Plain credentials (PBKDF2-SHA256 hash, never the password itself).
    std::string user;
    std::string passwordHash;  // "pbkdf2-sha256$iterations$saltHex$hashHex"
    // Legacy VNC Authentication needs a reversible secret: DPAPI (machine scope) protected, hex encoded.
    std::string vncPasswordProtected;

    // JPEG quality range for photo-like regions. The server starts at the upper limit (or the client's own
    // request if lower) and moves down when the client sees delay and back up when delay is gone.
    int jpegQualityMax = 85, jpegQualityMin = 40;

    bool h264 = false;                // use H.264 (RFB encoding 50) for video-like regions when the client supports it
    bool viewOnly = false;            // ignore keyboard, mouse and clipboard input from clients
    bool allowLegacyVncAuth = false;  // offer RFB "VNC Authentication" (DES challenge) when a VNC password is set
    std::string certFile, keyFile;    // PEM; generated on first start when missing
    std::string logFile;

    bool hasCredentials() const { return !passwordHash.empty() || !vncPasswordProtected.empty(); }
    bool isLoopbackOnly() const { return bind == "127.0.0.1" || bind == "::1" || bind == "localhost"; }
};

// Directory holding the executable (config, certificate and log live here by default).
std::string exeDirectory();
std::string defaultConfigPath();

// Brings out-of-range or inconsistent settings into a usable state (quality 1..99, min <= max).
void normalizeConfig(ServerConfig& cfg);

// key=value text file; unknown keys ignored. Missing file leaves defaults untouched.
bool loadConfig(const std::string& path, ServerConfig& cfg);
// Rewrites only the given keys, preserving everything else in the file.
bool updateConfigKey(const std::string& path, const std::string& key, const std::string& value);
