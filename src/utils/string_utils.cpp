#include "utils/string_utils.h"

#include <windows.h>

#include <algorithm>

namespace lt {
namespace str {

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int need = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (need <= 0) return {};
    std::string out(static_cast<size_t>(need), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                          out.data(), need, nullptr, nullptr);
    return out;
}

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return {};
    const int need = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                           nullptr, 0);
    if (need <= 0) return {};
    std::wstring out(static_cast<size_t>(need), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                          out.data(), need);
    return out;
}

std::string WideToAnsi(const std::wstring& w) {
    if (w.empty()) return {};
    const int need = ::WideCharToMultiByte(CP_ACP, 0, w.c_str(), static_cast<int>(w.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (need <= 0) return {};
    std::string out(static_cast<size_t>(need), '\0');
    ::WideCharToMultiByte(CP_ACP, 0, w.c_str(), static_cast<int>(w.size()),
                          out.data(), need, nullptr, nullptr);
    return out;
}

// ---------------------------------------------------------------- Base64

namespace {
constexpr char kTable[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int DecodeChar(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
} // namespace

std::string Base64Encode(const uint8_t* data, size_t len) {
    std::string out;
    if (data == nullptr || len == 0) return out;
    out.reserve(((len + 2) / 3) * 4);

    size_t i = 0;
    for (; i + 2 < len; i += 3) {
        const uint32_t v = (static_cast<uint32_t>(data[i]) << 16) |
                           (static_cast<uint32_t>(data[i + 1]) << 8) |
                           static_cast<uint32_t>(data[i + 2]);
        out.push_back(kTable[(v >> 18) & 0x3F]);
        out.push_back(kTable[(v >> 12) & 0x3F]);
        out.push_back(kTable[(v >> 6) & 0x3F]);
        out.push_back(kTable[v & 0x3F]);
    }
    if (i < len) {
        const size_t rem = len - i;
        uint32_t v = static_cast<uint32_t>(data[i]) << 16;
        if (rem == 2) v |= static_cast<uint32_t>(data[i + 1]) << 8;
        out.push_back(kTable[(v >> 18) & 0x3F]);
        out.push_back(kTable[(v >> 12) & 0x3F]);
        out.push_back(rem == 2 ? kTable[(v >> 6) & 0x3F] : '=');
        out.push_back('=');
    }
    return out;
}

std::string Base64Encode(const std::vector<uint8_t>& data) {
    return Base64Encode(data.data(), data.size());
}

std::vector<uint8_t> Base64Decode(const std::string& s) {
    std::vector<uint8_t> out;
    out.reserve((s.size() / 4) * 3);

    int      buf[4];
    int      n = 0;
    for (const char ch : s) {
        const int v = DecodeChar(static_cast<unsigned char>(ch));
        if (v < 0) continue;  // 跳过 '='、换行、空白
        buf[n++] = v;
        if (n == 4) {
            const uint32_t triple = (static_cast<uint32_t>(buf[0]) << 18) |
                                    (static_cast<uint32_t>(buf[1]) << 12) |
                                    (static_cast<uint32_t>(buf[2]) << 6) |
                                    static_cast<uint32_t>(buf[3]);
            out.push_back(static_cast<uint8_t>((triple >> 16) & 0xFF));
            out.push_back(static_cast<uint8_t>((triple >> 8) & 0xFF));
            out.push_back(static_cast<uint8_t>(triple & 0xFF));
            n = 0;
        }
    }
    if (n == 2) {
        const uint32_t triple = (static_cast<uint32_t>(buf[0]) << 18) |
                                (static_cast<uint32_t>(buf[1]) << 12);
        out.push_back(static_cast<uint8_t>((triple >> 16) & 0xFF));
    } else if (n == 3) {
        const uint32_t triple = (static_cast<uint32_t>(buf[0]) << 18) |
                                (static_cast<uint32_t>(buf[1]) << 12) |
                                (static_cast<uint32_t>(buf[2]) << 6);
        out.push_back(static_cast<uint8_t>((triple >> 16) & 0xFF));
        out.push_back(static_cast<uint8_t>((triple >> 8) & 0xFF));
    }
    return out;
}

// ---------------------------------------------------------------- 字符串处理

std::wstring Trim(const std::wstring& s) {
    const auto notSpace = [](wchar_t c) {
        return !iswspace(static_cast<wint_t>(c));
    };
    const auto b = std::find_if(s.begin(), s.end(), notSpace);
    if (b == s.end()) return {};
    const auto e = std::find_if(s.rbegin(), s.rend(), notSpace).base();
    return std::wstring(b, e);
}

std::string TrimA(const std::string& s) {
    const auto notSpace = [](unsigned char c) {
        return !isspace(c);
    };
    const auto b = std::find_if(s.begin(), s.end(), notSpace);
    if (b == s.end()) return {};
    const auto e = std::find_if(s.rbegin(), s.rend(), notSpace).base();
    return std::string(b, e);
}

bool StartsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() &&
           s.compare(0, prefix.size(), prefix) == 0;
}

bool StartsWithW(const std::wstring& s, const std::wstring& prefix) {
    return s.size() >= prefix.size() &&
           s.compare(0, prefix.size(), prefix) == 0;
}

bool EqualsIgnoreCaseW(const std::wstring& a, const std::wstring& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (towlower(a[i]) != towlower(b[i])) return false;
    }
    return true;
}

std::vector<std::wstring> SplitLines(const std::wstring& s) {
    std::vector<std::wstring> out;
    std::wstring              cur;
    for (const wchar_t c : s) {
        if (c == L'\r') continue;
        if (c == L'\n') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

// ---------------------------------------------------------------- 日志脱敏

std::string MaskSecrets(const std::string& text) {
    std::string out;
    out.reserve(text.size());

    size_t i = 0;
    while (i < text.size()) {
        // 规则 1：sk- 开头后跟 >=8 个 token 字符
        if (i + 3 <= text.size() && text.compare(i, 3, "sk-") == 0) {
            size_t j = i + 3;
            while (j < text.size()) {
                const unsigned char c = static_cast<unsigned char>(text[j]);
                if (isalnum(c) || c == '_' || c == '-') {
                    ++j;
                } else {
                    break;
                }
            }
            if (j - (i + 3) >= 8) {
                out += "sk-****";
                i = j;
                continue;
            }
        }

        // 规则 2：Bearer <token>
        if (i + 6 <= text.size() && _strnicmp(text.c_str() + i, "Bearer", 6) == 0) {
            size_t j = i + 6;
            while (j < text.size() && (text[j] == ' ' || text[j] == '\t')) ++j;
            const size_t tokStart = j;
            while (j < text.size()) {
                const unsigned char c = static_cast<unsigned char>(text[j]);
                if (isalnum(c) || c == '_' || c == '-' || c == '.' || c == '~' || c == '+' || c == '/') {
                    ++j;
                } else {
                    break;
                }
            }
            if (j - tokStart >= 8) {
                out += "Bearer ****";
                i = j;
                continue;
            }
        }

        out.push_back(text[i]);
        ++i;
    }
    return out;
}

} // namespace str
} // namespace lt
