#pragma once

#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace lt {

struct HttpResponse {
    bool         ok           = false;   // 传输层是否成功（能拿到 HTTP 响应）
    int          http_status  = 0;       // HTTP 状态码；0 表示传输失败
    std::string  body;                   // 响应体（UTF-8）
    std::string  error;                  // 中文错误描述
    DWORD        win32_error  = 0;       // 原始 Win32 错误码（便于诊断）
    int          elapsed_ms   = 0;       // 耗时
};

// 同步 HTTPS POST/GET 客户端。
// 支持：超时控制、取消令牌（调用方置位后立即 WinHttpCloseHandle 中断请求）、
//       Win32 错误码到简体中文提示的转换。
class WinHttpClient {
public:
    WinHttpClient() = default;
    ~WinHttpClient() = default;

    // 发起 POST JSON 请求。
    //  - url:        完整 URL，如 https://api.deepseek.com/chat/completions
    //  - headers:    额外请求头，如 {"Authorization: Bearer xxx"}
    //  - body:       请求体（UTF-8）
    //  - timeout_ms: 整体超时
    //  - cancel:     取消令牌；请求期间被置 true 则立即中断并返回"已取消"
    static HttpResponse PostJson(const std::wstring&              url,
                                 const std::vector<std::wstring>& headers,
                                 const std::string&               body,
                                 int                              timeout_ms,
                                 std::atomic<bool>&               cancel);

    // 简易 GET（用于连通性测试）
    static HttpResponse Get(const std::wstring&              url,
                            const std::vector<std::wstring>& headers,
                            int                              timeout_ms,
                            std::atomic<bool>&               cancel);

    // 把 WinHTTP/Win32 错误码翻译成简体中文提示
    static std::string DescribeWin32Error(DWORD err);
    // 把 HTTP 状态码翻译成简体中文提示（DeepSeek 错误码规范）
    static std::string DescribeHttpStatus(int status, const std::string& body);

private:
    struct UrlParts {
        std::wstring host;
        std::wstring path;
        INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
        bool         https = true;
        bool         valid = false;
    };

    static UrlParts ParseUrl(const std::wstring& url);

    static HttpResponse Request(const std::wstring&              method,
                                const std::wstring&              url,
                                const std::vector<std::wstring>& headers,
                                const std::string&               body,
                                int                              timeout_ms,
                                std::atomic<bool>&               cancel);
};

} // namespace lt
