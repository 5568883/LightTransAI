#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace lt {
namespace str {

// ---- 编码转换 ----
std::string  ToUtf8(const std::wstring& w);
std::wstring FromUtf8(const std::string& s);
std::string  WideToAnsi(const std::wstring& w);   // 少数只接受 ANSI 的 Win32 API 用

// ---- Base64 ----
std::string          Base64Encode(const uint8_t* data, size_t len);
std::string          Base64Encode(const std::vector<uint8_t>& data);
std::vector<uint8_t> Base64Decode(const std::string& s);

// ---- 通用字符串处理 ----
std::wstring Trim(const std::wstring& s);
std::string  TrimA(const std::string& s);
bool         StartsWith(const std::string& s, const std::string& prefix);
bool         StartsWithW(const std::wstring& s, const std::wstring& prefix);
bool         EqualsIgnoreCaseW(const std::wstring& a, const std::wstring& b);
std::vector<std::wstring> SplitLines(const std::wstring& s);

// 把 sk-xxxx / "Bearer xxxx" 掩码为 sk-****，用于日志脱敏
std::string MaskSecrets(const std::string& text);

} // namespace str
} // namespace lt
