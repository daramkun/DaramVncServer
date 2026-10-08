#include "config/Config.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

namespace {

std::string trim(std::string s) {
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

bool parseBool(const std::string& v) { return v == "1" || v == "true" || v == "yes" || v == "on"; }

}  // namespace

void normalizeConfig(ServerConfig& cfg) {
    cfg.jpegQualityMax = std::clamp(cfg.jpegQualityMax, 1, 99);
    cfg.jpegQualityMin = std::clamp(cfg.jpegQualityMin, 1, 99);
    if (cfg.jpegQualityMin > cfg.jpegQualityMax) cfg.jpegQualityMin = cfg.jpegQualityMax;
}

std::string exeDirectory() {
    char path[MAX_PATH * 2];
    DWORD n = GetModuleFileNameA(nullptr, path, sizeof path);
    std::string p(path, n);
    size_t slash = p.find_last_of("\\/");
    return slash == std::string::npos ? "." : p.substr(0, slash);
}

std::string defaultConfigPath() { return exeDirectory() + "\\daramvnc.conf"; }

bool loadConfig(const std::string& path, ServerConfig& cfg) {
    std::ifstream in(path);
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (k == "bind") cfg.bind = v;
        else if (k == "port") cfg.port = std::atoi(v.c_str());
        else if (k == "user") cfg.user = v;
        else if (k == "password_hash") cfg.passwordHash = v;
        else if (k == "vnc_password_dpapi") cfg.vncPasswordProtected = v;
        else if (k == "jpeg_quality_max") cfg.jpegQualityMax = std::atoi(v.c_str());
        else if (k == "jpeg_quality_min") cfg.jpegQualityMin = std::atoi(v.c_str());
        else if (k == "h264") cfg.h264 = parseBool(v);
        else if (k == "view_only") cfg.viewOnly = parseBool(v);
        else if (k == "allow_legacy_vnc_auth") cfg.allowLegacyVncAuth = parseBool(v);
        else if (k == "cert") cfg.certFile = v;
        else if (k == "key") cfg.keyFile = v;
        else if (k == "log") cfg.logFile = v;
    }
    const std::string dir = exeDirectory();
    if (cfg.certFile.empty()) cfg.certFile = dir + "\\server.crt";
    if (cfg.keyFile.empty()) cfg.keyFile = dir + "\\server.key";
    normalizeConfig(cfg);
    return true;
}

bool updateConfigKey(const std::string& path, const std::string& key, const std::string& value) {
    std::vector<std::string> lines;
    {
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line)) lines.push_back(line);
    }
    bool found = false;
    for (std::string& l : lines) {
        size_t eq = l.find('=');
        if (eq != std::string::npos && trim(l.substr(0, eq)) == key) {
            l = key + "=" + value;
            found = true;
        }
    }
    if (!found) lines.push_back(key + "=" + value);
    std::ofstream out(path, std::ios::trunc);
    if (!out) return false;
    for (const std::string& l : lines) out << l << "\n";
    return true;
}
