#include "translation/deepseek_client.h"

#include <nlohmann/json.hpp>

#include "network/winhttp_client.h"
#include "utils/logger.h"
#include "utils/string_utils.h"

using nlohmann::json;

namespace lt {

const wchar_t* DeepSeekClient::TextSystemPrompt() {
    return L"你是一个专业、精准的简体中文翻译引擎。"
           L"请将用户发送的文本直接翻译为自然流畅的简体中文。"
           L"严禁输出任何解释、寒暄、注音或重复原文，只输出翻译后的中文结果。"
           L"如果输入文本已经是中文，请原样返回，不要做任何改动或说明。";
}

bool DeepSeekClient::ExtractContent(const std::string& body,
                                    std::wstring&      out,
                                    std::wstring&      err) {
    out.clear();
    err.clear();

    json j;
    try {
        j = json::parse(body);
    } catch (const std::exception& e) {
        err = L"无法解析服务端返回的数据（JSON 格式错误）：" +
              str::FromUtf8(e.what());
        return false;
    }

    // 服务端可能以 {"error": {...}} 形式返回错误
    if (j.contains("error") && j["error"].is_object()) {
        std::string msg;
        if (j["error"].contains("message") && j["error"]["message"].is_string()) {
            msg = j["error"]["message"].get<std::string>();
        }
        err = msg.empty() ? L"服务端返回错误，但未提供详细信息。"
                          : L"服务端错误：" + str::FromUtf8(msg);
        return false;
    }

    if (!j.contains("choices") || !j["choices"].is_array() || j["choices"].empty()) {
        err = L"服务端返回内容为空（未包含 choices 字段）。";
        return false;
    }

    const json& choice = j["choices"][0];

    // finish_reason 诊断：输出被截断时给出明确提示
    std::wstring finish;
    if (choice.contains("finish_reason") && choice["finish_reason"].is_string()) {
        finish = str::FromUtf8(choice["finish_reason"].get<std::string>());
    }

    if (!choice.contains("message") || !choice["message"].is_object()) {
        err = L"服务端返回结构异常（缺少 message 字段）。";
        return false;
    }

    const json& msg = choice["message"];
    if (!msg.contains("content") || !msg["content"].is_string()) {
        // 思考模式下 content 可能为空而只有 reasoning_content
        if (msg.contains("reasoning_content") && msg["reasoning_content"].is_string()) {
            err = L"模型只返回了思考过程而未返回译文，请确认已关闭思考模式。";
        } else {
            err = L"服务端未返回翻译内容。";
        }
        return false;
    }

    out = str::FromUtf8(msg["content"].get<std::string>());

    if (finish == L"length") {
        err = L"译文过长被截断，请缩短选中文本后重试。";
        // 仍返回已获得的部分内容，由调用方决定是否展示
        return !out.empty();
    }
    if (finish == L"content_filter") {
        err = L"内容被服务端安全策略拦截。";
        return false;
    }
    if (finish == L"insufficient_system_resource") {
        err = L"服务端资源不足，请稍后重试。";
        return false;
    }

    if (out.empty()) {
        err = L"模型返回了空译文。";
        return false;
    }
    return true;
}

TranslationResult DeepSeekClient::TranslateText(const std::wstring& text,
                                                const AppConfig&    cfg,
                                                std::atomic<bool>&  cancel) {
    TranslationResult result{};

    if (text.empty()) {
        result.error_message = L"未检测到选中文字";
        return result;
    }
    if (cfg.api_key_plain.empty()) {
        result.error_message = L"尚未配置 API Key，请在设置中填写 DeepSeek API Key。";
        return result;
    }

    result.model_used = cfg.text_model;

    // ---- 构造请求体 ----
    json payload;
    payload["model"] = str::ToUtf8(cfg.text_model);
    payload["stream"] = false;

    // 关键：显式关闭思考模式（官方默认 enabled），否则翻译延迟极高
    payload["thinking"] = { {"type", "disabled"} };

    payload["messages"] = json::array({
        json{ {"role", "system"}, {"content", str::ToUtf8(TextSystemPrompt())} },
        json{ {"role", "user"},   {"content", str::ToUtf8(text)} },
    });

    const std::string body = payload.dump();

    std::vector<std::wstring> headers = {
        L"Content-Type: application/json; charset=utf-8",
        L"Authorization: Bearer " + str::FromUtf8(cfg.api_key_plain),
        L"Accept: application/json",
    };

    LT_INFO(L"发起文本翻译请求，模型=" + cfg.text_model +
            L"，原文长度=" + std::to_wstring(text.size()));

    const HttpResponse resp = WinHttpClient::PostJson(
        cfg.api_url, headers, body, cfg.request_timeout_ms, cancel);

    result.http_status = resp.http_status;
    result.elapsed_ms  = resp.elapsed_ms;

    if (!resp.ok) {
        result.error_message = str::FromUtf8(resp.error);
        LT_ERROR(L"文本翻译请求失败: " + result.error_message);
        return result;
    }

    if (resp.http_status < 200 || resp.http_status >= 300) {
        result.error_message = str::FromUtf8(
            resp.error.empty() ? WinHttpClient::DescribeHttpStatus(resp.http_status, resp.body)
                               : resp.error);
        LT_ERROR(L"文本翻译 HTTP 错误: " + result.error_message);
        return result;
    }

    std::wstring content, err;
    if (!ExtractContent(resp.body, content, err)) {
        result.error_message = err;
        LT_ERROR(L"文本翻译响应解析失败: " + err);
        return result;
    }

    result.success    = true;
    result.translated = str::Trim(content);
    LT_INFO(L"文本翻译完成，耗时 " + std::to_wstring(resp.elapsed_ms) + L" ms");
    return result;
}

TranslationResult DeepSeekClient::TestConnection(const AppConfig& cfg, bool use_vision) {
    TranslationResult result{};
    std::atomic<bool> cancel{false};

    const std::wstring url   = use_vision ? cfg.EffectiveVisionUrl() : cfg.api_url;
    const std::wstring model = use_vision ? cfg.EffectiveVisionModel() : cfg.text_model;
    const std::string  key   = use_vision ? cfg.EffectiveVisionKey() : cfg.api_key_plain;

    result.model_used = model;

    if (key.empty()) {
        result.error_message = L"请先填写 API Key。";
        return result;
    }
    if (url.empty()) {
        result.error_message = L"请先填写 API 地址。";
        return result;
    }

    json payload;
    payload["model"]    = str::ToUtf8(model);
    payload["stream"]   = false;
    payload["thinking"] = { {"type", "disabled"} };
    payload["messages"] = json::array({
        json{ {"role", "user"}, {"content", "ping"} },
    });
    // 限制输出长度，测试连接尽量轻量
    payload["max_tokens"] = 1;

    const std::string body = payload.dump();

    std::vector<std::wstring> headers = {
        L"Content-Type: application/json; charset=utf-8",
        L"Authorization: Bearer " + str::FromUtf8(key),
        L"Accept: application/json",
    };

    const HttpResponse resp = WinHttpClient::PostJson(url, headers, body, 20000, cancel);
    result.http_status = resp.http_status;
    result.elapsed_ms  = resp.elapsed_ms;

    if (!resp.ok) {
        result.error_message = str::FromUtf8(resp.error);
        return result;
    }

    if (resp.http_status < 200 || resp.http_status >= 300) {
        result.error_message = str::FromUtf8(
            resp.error.empty() ? WinHttpClient::DescribeHttpStatus(resp.http_status, resp.body)
                               : resp.error);
        return result;
    }

    // 能拿到 2xx 即认为连通（max_tokens=1 可能触发 length 截断，属正常）
    result.success = true;
    result.translated = L"连接成功";
    return result;
}

} // namespace lt
