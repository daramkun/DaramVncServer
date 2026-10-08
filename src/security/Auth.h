#pragma once
#include <cstdint>
#include <string>

namespace auth {

// PBKDF2-HMAC-SHA256 password hash in the config format "pbkdf2-sha256$iters$saltHex$hashHex".
std::string hashPassword(const std::string& password);
bool verifyPassword(const std::string& password, const std::string& stored);

// DPAPI (machine scope) wrapper for the legacy VNC password, hex encoded.
std::string protectSecret(const std::string& secret);
bool unprotectSecret(const std::string& hex, std::string& secret);

// RFB "VNC Authentication": response = DES-ECB(challenge, key) with the bit-reversed 8-byte password key.
bool vncAuthResponse(const std::string& password, const uint8_t challenge[16], uint8_t response[16]);

void randomBytes(void* out, size_t n);
bool constantTimeEqual(const void* a, const void* b, size_t n);

// Generates a self-signed EC P-256 certificate (10 years) if either file is missing.
bool ensureCertificate(const std::string& certFile, const std::string& keyFile, std::string* error);

}  // namespace auth
