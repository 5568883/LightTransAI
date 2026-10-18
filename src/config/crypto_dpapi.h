#pragma once

#include <string>

namespace lt {

// 使用 CryptProtectData / CryptUnprotectData（当前用户作用域）加解密。
// 密文以 Base64 字符串形式返回，便于存入 JSON 配置。
//
// 注意：DPAPI 绑定当前 Windows 用户。换账户或换机器后密文无法解开，
// 此时 Decrypt 返回 false，调用方应优雅降级为"请重新输入 Key"。
class CryptoDpapi {
public:
    // 加密；失败返回空字符串
    static std::string Encrypt(const std::string& plaintext);

    // 解密；失败返回 false 并清空 plaintext
    static bool Decrypt(const std::string& ciphertextBase64, std::string& plaintext);

    // 附加熵（可选）：同一份配置绑定到本应用，降低其它程序直接解密的可能
    static void SetEntropy(const std::string& entropy);

private:
    static std::string entropy_;
};

} // namespace lt
