#include "security/Auth.h"

#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace auth {

namespace {

constexpr ULONG kIterations = 200000;

std::string toHex(const uint8_t* d, size_t n) {
    static const char* h = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) { s += h[d[i] >> 4]; s += h[d[i] & 15]; }
    return s;
}

bool fromHex(const std::string& s, std::vector<uint8_t>& out) {
    if (s.size() % 2) return false;
    out.clear();
    for (size_t i = 0; i < s.size(); i += 2) {
        unsigned v;
        if (std::sscanf(s.substr(i, 2).c_str(), "%2x", &v) != 1) return false;
        out.push_back(static_cast<uint8_t>(v));
    }
    return true;
}

bool pbkdf2(const std::string& password, const uint8_t* salt, size_t saltLen, ULONG iters, uint8_t* out, size_t outLen) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0) return false;
    NTSTATUS st = BCryptDeriveKeyPBKDF2(alg, reinterpret_cast<PUCHAR>(const_cast<char*>(password.data())),
                                        static_cast<ULONG>(password.size()), const_cast<PUCHAR>(salt),
                                        static_cast<ULONG>(saltLen), iters, out, static_cast<ULONG>(outLen), 0);
    BCryptCloseAlgorithmProvider(alg, 0);
    return st == 0;
}

}  // namespace

void randomBytes(void* out, size_t n) { BCryptGenRandom(nullptr, static_cast<PUCHAR>(out), static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG); }

bool constantTimeEqual(const void* a, const void* b, size_t n) {
    const auto* x = static_cast<const uint8_t*>(a);
    const auto* y = static_cast<const uint8_t*>(b);
    uint8_t d = 0;
    for (size_t i = 0; i < n; ++i) d |= x[i] ^ y[i];
    return d == 0;
}

std::string hashPassword(const std::string& password) {
    uint8_t salt[16], hash[32];
    randomBytes(salt, sizeof salt);
    if (!pbkdf2(password, salt, sizeof salt, kIterations, hash, sizeof hash)) return {};
    return "pbkdf2-sha256$" + std::to_string(kIterations) + "$" + toHex(salt, sizeof salt) + "$" + toHex(hash, sizeof hash);
}

bool verifyPassword(const std::string& password, const std::string& stored) {
    char prefix[32], saltHex[128], hashHex[128];
    unsigned iters = 0;
    if (std::sscanf(stored.c_str(), "%31[^$]$%u$%127[^$]$%127s", prefix, &iters, saltHex, hashHex) != 4) return false;
    if (std::strcmp(prefix, "pbkdf2-sha256") != 0 || iters < 1000) return false;
    std::vector<uint8_t> salt, expect;
    if (!fromHex(saltHex, salt) || !fromHex(hashHex, expect) || expect.size() != 32) return false;
    uint8_t got[32];
    if (!pbkdf2(password, salt.data(), salt.size(), iters, got, sizeof got)) return false;
    return constantTimeEqual(got, expect.data(), 32);
}

std::string protectSecret(const std::string& secret) {
    DATA_BLOB in{static_cast<DWORD>(secret.size()), reinterpret_cast<BYTE*>(const_cast<char*>(secret.data()))}, out{};
    if (!CryptProtectData(&in, L"daramvnc", nullptr, nullptr, nullptr, CRYPTPROTECT_LOCAL_MACHINE | CRYPTPROTECT_UI_FORBIDDEN, &out))
        return {};
    std::string hex = toHex(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return hex;
}

bool unprotectSecret(const std::string& hex, std::string& secret) {
    std::vector<uint8_t> bytes;
    if (!fromHex(hex, bytes)) return false;
    DATA_BLOB in{static_cast<DWORD>(bytes.size()), bytes.data()}, out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return false;
    secret.assign(reinterpret_cast<char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return true;
}

bool vncAuthResponse(const std::string& password, const uint8_t challenge[16], uint8_t response[16]) {
    uint8_t key[8] = {0};
    for (size_t i = 0; i < 8 && i < password.size(); ++i) {  // VNC quirk: each key byte is bit-reversed
        uint8_t b = static_cast<uint8_t>(password[i]), r = 0;
        for (int k = 0; k < 8; ++k) r |= ((b >> k) & 1) << (7 - k);
        key[i] = r;
    }
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE hk = nullptr;
    bool ok = false;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_DES_ALGORITHM, nullptr, 0) == 0) {
        BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_ECB)),
                          sizeof(BCRYPT_CHAIN_MODE_ECB), 0);
        if (BCryptGenerateSymmetricKey(alg, &hk, nullptr, 0, key, sizeof key, 0) == 0) {
            ULONG got = 0;
            uint8_t tmp[16];
            std::memcpy(tmp, challenge, 16);
            ok = BCryptEncrypt(hk, tmp, 16, nullptr, nullptr, 0, response, 16, &got, 0) == 0 && got == 16;
            BCryptDestroyKey(hk);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    SecureZeroMemory(key, sizeof key);
    return ok;
}

bool ensureCertificate(const std::string& certFile, const std::string& keyFile, std::string* error) {
    auto exists = [](const std::string& p) { return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES; };
    if (exists(certFile) && exists(keyFile)) return true;

    auto fail = [&](const char* what) {
        if (error) *error = what;
        return false;
    };
    EVP_PKEY* pkey = EVP_EC_gen("P-256");
    if (!pkey) return fail("EC key generation failed");
    X509* x = X509_new();
    X509_set_version(x, 2);
    uint8_t serial[8];
    randomBytes(serial, sizeof serial);
    serial[0] &= 0x7F;
    BIGNUM* bn = BN_bin2bn(serial, sizeof serial, nullptr);
    BN_to_ASN1_INTEGER(bn, X509_get_serialNumber(x));
    BN_free(bn);
    X509_gmtime_adj(X509_getm_notBefore(x), -3600);
    X509_gmtime_adj(X509_getm_notAfter(x), 60L * 60 * 24 * 365 * 10);
    X509_set_pubkey(x, pkey);

    char host[256] = "daramvnc";
    DWORD n = sizeof host;
    GetComputerNameExA(ComputerNameDnsHostname, host, &n);
    X509_NAME* name = X509_get_subject_name(x);
    X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("DaramVncServer"), -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<unsigned char*>(host), -1, -1, 0);
    X509_set_issuer_name(x, name);

    X509V3_CTX ctx;
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, x, x, nullptr, nullptr, 0);
    std::string san = std::string("DNS:") + host + ",DNS:localhost,IP:127.0.0.1";
    if (X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &ctx, NID_subject_alt_name, san.c_str())) {
        X509_add_ext(x, ext, -1);
        X509_EXTENSION_free(ext);
    }
    bool ok = X509_sign(x, pkey, EVP_sha256()) > 0;

    if (ok) {
        FILE* f = nullptr;
        ok = fopen_s(&f, certFile.c_str(), "wb") == 0 && f && PEM_write_X509(f, x) == 1;
        if (f) std::fclose(f);
        f = nullptr;
        ok = ok && fopen_s(&f, keyFile.c_str(), "wb") == 0 && f && PEM_write_PrivateKey(f, pkey, nullptr, nullptr, 0, nullptr, nullptr) == 1;
        if (f) std::fclose(f);
        // Restrict the private key to SYSTEM, Administrators and the creating user (best effort).
        const char* user = std::getenv("USERNAME");
        std::string cmd = "icacls \"" + keyFile + "\" /inheritance:r /grant:r SYSTEM:F Administrators:F";
        if (user && *user) cmd += std::string(" \"") + user + "\":F";
        cmd += " >nul 2>&1";
        std::system(cmd.c_str());
    }
    X509_free(x);
    EVP_PKEY_free(pkey);
    return ok || fail("could not write certificate files");
}

}  // namespace auth
