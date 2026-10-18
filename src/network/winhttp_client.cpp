#include "network/winhttp_client.h"

#pragma comment(lib, "winhttp.lib")

#include <chrono>

#include "utils/logger.h"
#include "utils/string_utils.h"

namespace lt {

namespace {

constexpr int kChunkBytes = 16 * 1024;

// RAII 句柄包装
class WinHttpHandle {
public:
    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET h) : h_(h) {}
    ~WinHttpHandle() { Close(); }

    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;

    WinHttpHandle(WinHttpHandle&& o) noexcept : h_(o.h_) { o.h_ = nullptr; }
    WinHttpHandle& operator=(WinHttpHandle&& o) noexcept {
        if (this != &o) { Close(); h_ = o.h_; o.h_ = nullptr; }
        return *this;
    }

    HINTERNET Get() const { return h_; }
    explicit operator bool() const { return h_ != nullptr; }

    // 原子地取出句柄并置空，供取消线程安全关闭
    HINTERNET Release() {
        HINTERNET t = h_;
        h_ = nullptr;
        return t;
    }

    void Close() {
        if (h_) {
            ::WinHttpCloseHandle(h_);
            h_ = nullptr;
        }
    }

private:
    HINTERNET h_ = nullptr;
};

} // namespace

// ------------------------------------------------------------------ URL 解析

WinHttpClient::UrlParts WinHttpClient::ParseUrl(const std::wstring& url) {
    UrlParts p{};

    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);

    wchar_t hostBuf[256]{};
    wchar_t pathBuf[2048]{};
    uc.lpszHostName     = hostBuf;
    uc.dwHostNameLength = static_cast<DWORD>(std::size(hostBuf));
    uc.lpszUrlPath      = pathBuf;
    uc.dwUrlPathLength  = static_cast<DWORD>(std::size(pathBuf));

    if (!::WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &uc)) {
        return p;
    }

    p.host  = std::wstring(uc.lpszHostName, uc.dwHostNameLength);
    p.path  = std::wstring(uc.lpszUrlPath, uc.dwUrlPathLength);
    p.port  = uc.nPort;
    p.https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    p.valid = !p.host.empty();
    return p;
}

// ------------------------------------------------------------------ 错误翻译

std::string WinHttpClient::DescribeWin32Error(DWORD err) {
    switch (err) {
        case ERROR_WINHTTP_TIMEOUT:
            return "请求超时，请检查网络连接或稍后重试。";
        case ERROR_WINHTTP_NAME_NOT_RESOLVED:
            return "无法解析服务器域名，请检查 API 地址是否正确以及网络是否通畅。";
        case ERROR_WINHTTP_CANNOT_CONNECT:
            return "无法连接到服务器，请检查网络、防火墙或代理设置。";
        case ERROR_WINHTTP_CONNECTION_ERROR:
            return "网络连接被中断，请检查网络稳定性后重试。";
        case ERROR_WINHTTP_INVALID_URL:
            return "API 地址格式无效，请填写完整的 https:// 开头的地址。";
        case ERROR_WINHTTP_UNRECOGNIZED_SCHEME:
            return "无法识别的协议，请使用 https:// 开头的地址。";
        case ERROR_WINHTTP_RESEND_REQUEST:
            return "请求需要重发，请重试。";
        case ERROR_NOT_ENOUGH_MEMORY:
        case ERROR_OUTOFMEMORY:
            return "内存不足，无法完成请求。";
        case ERROR_INVALID_PARAMETER:
            return "请求参数无效，请检查 API 地址设置。";
        case ERROR_WINHTTP_OPERATION_CANCELLED:
            return "请求已取消。";

        // ---- TLS / 证书相关 ----
        // 企业网络中的代理常做 TLS 中间人检查，会替换证书；
        // 这类失败在真实环境中很常见，必须给出可操作的中文指引，
        // 而不是抛一个裸错误码让用户无从下手。
        case ERROR_WINHTTP_SECURE_FAILURE:
        case ERROR_WINHTTP_SECURE_CHANNEL_ERROR:
            return "HTTPS 安全连接失败（TLS 握手未完成）。"
                   "若您在公司网络下，可能是代理的证书检查所致，"
                   "请尝试更换网络或联系网络管理员。";
        case ERROR_WINHTTP_SECURE_INVALID_CERT:
            return "服务器证书无效。若在公司网络下，通常是代理替换了证书，"
                   "请尝试更换网络后重试。";
        case ERROR_WINHTTP_SECURE_CERT_REVOKED:
            return "服务器证书已被吊销，出于安全考虑已中止连接。";
        case ERROR_WINHTTP_SECURE_CERT_WRONG_USAGE:
            return "服务器证书用途不匹配，无法建立安全连接。";
        case ERROR_WINHTTP_SECURE_FAILURE_PROXY:
            return "通过代理建立 HTTPS 连接失败，请检查代理设置。";
        case ERROR_WINHTTP_CLIENT_CERT_NO_PRIVATE_KEY:
        case ERROR_WINHTTP_CLIENT_CERT_NO_ACCESS_PRIVATE_KEY:
        case ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED:
        case ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED_PROXY:
            return "服务器要求客户端证书，当前配置不支持。";
        case ERROR_WINHTTP_HTTP_PROTOCOL_MISMATCH:
            return "协议不匹配（服务器可能要求 HTTP/2 或响应异常），请稍后重试。";
        case ERROR_WINHTTP_INVALID_SERVER_RESPONSE:
            return "服务器返回了无法识别的响应，可能被网络中间设备拦截。";

        default:
            // 兜底：明确告知这是网络层失败并带上错误码，便于排查
            return "网络请求失败（WinHTTP 错误码 " + std::to_string(err) + "）。";
    }
}

std::string WinHttpClient::DescribeHttpStatus(int status, const std::string& body) {
    // 尽量从响应体里抽取服务端给出的 message，便于用户定位
    std::string detail;
    if (!body.empty() && body.size() < 4096) {
        const size_t k = body.find("\"message\"");
        if (k != std::string::npos) {
            const size_t colon = body.find(':', k);
            const size_t q1    = (colon == std::string::npos) ? std::string::npos
                                                             : body.find('"', colon + 1);
            if (q1 != std::string::npos) {
                const size_t q2 = body.find('"', q1 + 1);
                if (q2 != std::string::npos && q2 > q1 + 1) {
                    detail = body.substr(q1 + 1, q2 - q1 - 1);
                }
            }
        }
    }

    std::string base;
    switch (status) {
        case 400: base = "请求格式错误，请检查模型名称等参数设置。"; break;
        case 401: base = "认证失败：API Key 无效或已失效，请在设置中重新填写。"; break;
        case 402: base = "账户余额不足，请前往 DeepSeek 开放平台充值。"; break;
        case 403: base = "访问被拒绝，请确认该 API Key 有权限调用此模型。"; break;
        case 404: base = "接口地址不存在，请检查 API 地址是否填写正确。"; break;
        case 413: base = "上传内容过大，请缩小框选区域后重试。"; break;
        case 422: base = "请求参数错误，请检查模型名称与参数设置。"; break;
        case 429: base = "请求过于频繁，已触发限速，请稍后重试。"; break;
        case 500: base = "服务器内部故障，请稍后重试。"; break;
        case 502:
        case 503: base = "服务器繁忙，请稍后重试。"; break;
        case 504: base = "服务器响应超时，请稍后重试。"; break;
        default:
            base = "服务返回错误（HTTP " + std::to_string(status) + "）。";
            break;
    }

    if (!detail.empty()) {
        base += " 服务端信息：" + str::TrimA(detail);
    }
    return base;
}

// ------------------------------------------------------------------ 核心请求

HttpResponse WinHttpClient::Request(const std::wstring&              method,
                                    const std::wstring&              url,
                                    const std::vector<std::wstring>& headers,
                                    const std::string&               body,
                                    int                              timeout_ms,
                                    std::atomic<bool>&               cancel) {
    using Clock = std::chrono::steady_clock;
    const auto t0 = Clock::now();

    HttpResponse res{};

    const UrlParts parts = ParseUrl(url);
    if (!parts.valid) {
        res.error = "API 地址无效，请填写形如 https://api.deepseek.com/chat/completions 的完整地址。";
        return res;
    }

    // 打开会话
    WinHttpHandle session(::WinHttpOpen(
        L"LightTransAI/0.1 (Windows)",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        res.win32_error = ::GetLastError();
        res.error = DescribeWin32Error(res.win32_error);
        return res;
    }

    // 整体超时：解析/连接/发送/接收四段都要设，否则某一段会无限等待
    ::WinHttpSetTimeouts(session.Get(), timeout_ms, timeout_ms, timeout_ms, timeout_ms);

    if (cancel.load()) {
        res.win32_error = ERROR_WINHTTP_OPERATION_CANCELLED;
        res.error = "请求已取消。";
        return res;
    }

    WinHttpHandle connect(::WinHttpConnect(session.Get(), parts.host.c_str(), parts.port, 0));
    if (!connect) {
        res.win32_error = ::GetLastError();
        res.error = DescribeWin32Error(res.win32_error);
        return res;
    }

    const DWORD flags = parts.https ? WINHTTP_FLAG_SECURE : 0;
    WinHttpHandle request(::WinHttpOpenRequest(
        connect.Get(), method.c_str(), parts.path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!request) {
        res.win32_error = ::GetLastError();
        res.error = DescribeWin32Error(res.win32_error);
        return res;
    }

    // 取消检查点：置位则关闭句柄，让下面阻塞中的 WinHttpSendRequest/ReceiveResponse 立即返回
    auto checkCancel = [&]() -> bool {
        if (cancel.load()) {
            ::WinHttpCloseHandle(request.Release());
            res.win32_error = ERROR_WINHTTP_OPERATION_CANCELLED;
            res.error = "请求已取消。";
            return true;
        }
        return false;
    };

    // 组装请求头
    std::wstring headerBlock;
    for (const auto& h : headers) {
        headerBlock += h;
        headerBlock += L"\r\n";
    }

    if (!::WinHttpSendRequest(request.Get(),
                              headerBlock.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS
                                                  : headerBlock.c_str(),
                              headerBlock.empty() ? 0
                                                  : static_cast<DWORD>(headerBlock.size()),
                              body.empty() ? WINHTTP_NO_REQUEST_DATA
                                           : const_cast<char*>(body.data()),
                              static_cast<DWORD>(body.size()),
                              static_cast<DWORD>(body.size()), 0)) {
        if (cancel.load()) {
            res.win32_error = ERROR_WINHTTP_OPERATION_CANCELLED;
            res.error = "请求已取消。";
        } else {
            res.win32_error = ::GetLastError();
            res.error = DescribeWin32Error(res.win32_error);
        }
        return res;
    }

    if (checkCancel()) return res;

    if (!::WinHttpReceiveResponse(request.Get(), nullptr)) {
        if (cancel.load()) {
            res.win32_error = ERROR_WINHTTP_OPERATION_CANCELLED;
            res.error = "请求已取消。";
        } else {
            res.win32_error = ::GetLastError();
            res.error = DescribeWin32Error(res.win32_error);
        }
        return res;
    }

    // 读取状态码
    {
        DWORD statusCode = 0;
        DWORD len        = sizeof(statusCode);
        if (::WinHttpQueryHeaders(request.Get(),
                                  WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                  WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &len,
                                  WINHTTP_NO_HEADER_INDEX)) {
            res.http_status = static_cast<int>(statusCode);
        }
    }

    // 分块读取响应体
    for (;;) {
        if (cancel.load()) {
            res.win32_error = ERROR_WINHTTP_OPERATION_CANCELLED;
            res.error = "请求已取消。";
            res.ok = false;
            return res;
        }

        DWORD avail = 0;
        if (!::WinHttpQueryDataAvailable(request.Get(), &avail)) {
            res.win32_error = ::GetLastError();
            res.error = DescribeWin32Error(res.win32_error);
            return res;
        }
        if (avail == 0) break;  // 读取完毕

        const DWORD toRead = (avail > static_cast<DWORD>(kChunkBytes))
                                 ? static_cast<DWORD>(kChunkBytes)
                                 : avail;
        const size_t oldSize = res.body.size();
        res.body.resize(oldSize + toRead);

        DWORD read = 0;
        if (!::WinHttpReadData(request.Get(), res.body.data() + oldSize, toRead, &read)) {
            res.win32_error = ::GetLastError();
            res.error = DescribeWin32Error(res.win32_error);
            res.body.resize(oldSize);
            return res;
        }
        res.body.resize(oldSize + read);
        if (read == 0) break;
    }

    res.ok = true;
    res.elapsed_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count());

    if (res.http_status < 200 || res.http_status >= 300) {
        res.error = DescribeHttpStatus(res.http_status, res.body);
    }

    return res;
}

HttpResponse WinHttpClient::PostJson(const std::wstring&              url,
                                     const std::vector<std::wstring>& headers,
                                     const std::string&               body,
                                     int                              timeout_ms,
                                     std::atomic<bool>&               cancel) {
    return Request(L"POST", url, headers, body, timeout_ms, cancel);
}

HttpResponse WinHttpClient::Get(const std::wstring&              url,
                                const std::vector<std::wstring>& headers,
                                int                              timeout_ms,
                                std::atomic<bool>&               cancel) {
    return Request(L"GET", url, headers, std::string{}, timeout_ms, cancel);
}

} // namespace lt
