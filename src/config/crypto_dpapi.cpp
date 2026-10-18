#include "config/crypto_dpapi.h"

#include <windows.h>
#include <wincrypt.h>

#pragma comment(lib, "crypt32.lib")

#include <vector>

#include "utils/string_utils.h"

namespace lt {

std::string CryptoDpapi::entropy_ = "LightTransAI/v1/DPAPI";

void CryptoDpapi::SetEntropy(const std::string& entropy) {
    entropy_ = entropy;
}

namespace {
DATA_BLOB MakeBlob(const std::string& s) {
    DATA_BLOB b{};
    b.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(s.data()));
    b.cbData = static_cast<DWORD>(s.size());
    return b;
}
} // namespace

std::string CryptoDpapi::Encrypt(const std::string& plaintext) {
    if (plaintext.empty()) return {};

    DATA_BLOB in  = MakeBlob(plaintext);
    DATA_BLOB ent = MakeBlob(entropy_);
    DATA_BLOB out{};

    const BOOL ok = ::CryptProtectData(
        &in, L"LightTransAI API Key", &ent, nullptr, nullptr,
        CRYPTPROTECT_UI_FORBIDDEN, &out);

    if (!ok || out.pbData == nullptr) {
        return {};
    }

    std::string result = str::Base64Encode(out.pbData, out.cbData);
    ::SecureZeroMemory(out.pbData, out.cbData);
    ::LocalFree(out.pbData);
    return result;
}

bool CryptoDpapi::Decrypt(const std::string& ciphertextBase64, std::string& plaintext) {
    plaintext.clear();
    if (ciphertextBase64.empty()) return false;

    const std::vector<uint8_t> raw = str::Base64Decode(ciphertextBase64);
    if (raw.empty()) return false;

    DATA_BLOB in{};
    in.pbData = const_cast<BYTE*>(raw.data());
    in.cbData = static_cast<DWORD>(raw.size());

    DATA_BLOB ent = MakeBlob(entropy_);
    DATA_BLOB out{};

    const BOOL ok = ::CryptUnprotectData(
        &in, nullptr, &ent, nullptr, nullptr,
        CRYPTPROTECT_UI_FORBIDDEN, &out);

    if (!ok || out.pbData == nullptr) {
        return false;
    }

    plaintext.assign(reinterpret_cast<char*>(out.pbData), out.cbData);
    ::SecureZeroMemory(out.pbData, out.cbData);
    ::LocalFree(out.pbData);
    return true;
}

} // namespace lt
