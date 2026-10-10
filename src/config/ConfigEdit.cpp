#include "config/ConfigEdit.h"

#include <winsock2.h>
#include <ws2tcpip.h>

bool isValidBindAddress(const std::string& bind) {
    if (bind == "localhost") return true;
    in_addr a4;
    in6_addr a6;
    return inet_pton(AF_INET, bind.c_str(), &a4) == 1 || inet_pton(AF_INET6, bind.c_str(), &a6) == 1;
}

std::vector<std::string> validateConfig(const ServerConfig& cfg) {
    std::vector<std::string> problems;
    if (!isValidBindAddress(cfg.bind)) problems.push_back("수신 주소가 올바른 IPv4/IPv6 주소가 아닙니다: " + cfg.bind);
    if (cfg.port < 1 || cfg.port > 65535) problems.push_back("포트는 1~65535 사이여야 합니다.");
    const bool qualityOk = cfg.jpegQualityMax >= 1 && cfg.jpegQualityMax <= 99 && cfg.jpegQualityMin >= 1 && cfg.jpegQualityMin <= 99;
    if (!qualityOk) problems.push_back("JPEG 품질은 1~99 사이여야 합니다.");
    else if (cfg.jpegQualityMin > cfg.jpegQualityMax) problems.push_back("JPEG 품질 하한이 상한보다 클 수 없습니다.");
    if (cfg.logMaxMb < 0 || cfg.logMaxMb > 10240) problems.push_back("로그 파일 최대 크기(MB)는 0~10240 사이여야 합니다. (0 = 회전 안 함)");
    if (cfg.logKeep < 0 || cfg.logKeep > 50) problems.push_back("보관할 이전 로그 파일 수는 0~50 사이여야 합니다.");
    if (problems.empty() && !cfg.hasCredentials() && !cfg.isLoopbackOnly())
        problems.push_back("로그인 비밀번호나 VNC 비밀번호가 없으면 수신 주소를 127.0.0.1(또는 ::1)로만 둘 수 있습니다. 먼저 비밀번호를 설정하세요.");
    return problems;
}

bool saveEditableConfig(const std::string& path, const ServerConfig& cfg) {
    auto flag = [](bool b) { return std::string(b ? "1" : "0"); };
    return updateConfigKey(path, "bind", cfg.bind) && updateConfigKey(path, "port", std::to_string(cfg.port)) &&
           updateConfigKey(path, "view_only", flag(cfg.viewOnly)) &&
           updateConfigKey(path, "allow_legacy_vnc_auth", flag(cfg.allowLegacyVncAuth)) && updateConfigKey(path, "h264", flag(cfg.h264)) &&
           updateConfigKey(path, "jpeg_quality_max", std::to_string(cfg.jpegQualityMax)) &&
           updateConfigKey(path, "jpeg_quality_min", std::to_string(cfg.jpegQualityMin)) && updateConfigKey(path, "log", cfg.logFile) &&
           updateConfigKey(path, "log_max_mb", std::to_string(cfg.logMaxMb)) && updateConfigKey(path, "log_keep", std::to_string(cfg.logKeep));
}
