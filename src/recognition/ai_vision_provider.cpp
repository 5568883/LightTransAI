#include "recognition/ai_vision_provider.h"

#include <nlohmann/json.hpp>

#include <algorithm>

#include "network/winhttp_client.h"
#include "utils/logger.h"
#include "utils/string_utils.h"

using nlohmann::json;

namespace lt {

namespace {

// 归一化坐标范围（规范规定 0~1000）
constexpr float kNormMax = 1000.0f;

// 把越界坐标 clamp 到 [0,1000]，并校验矩形有效性。
// 无效（零面积或反向）返回 false。
bool NormalizeBox(const float raw[4], TranslatedTextBlock& out) {
    // 输入顺序为 [ymin, xmin, ymax, xmax]
    auto clamp = [](float v) {
        if (v < 0.0f) return 0.0f;
        if (v > kNormMax) return kNormMax;
        return v;
    };

    const float ymin = clamp(raw[0]);
    const float xmin = clamp(raw[1]);
    const float ymax = clamp(raw[2]);
    const float xmax = clamp(raw[3]);

    if (xmin >= xmax || ymin >= ymax) return false;

    out.norm_xmin = xmin;
    out.norm_ymin = ymin;
    out.norm_xmax = xmax;
    out.norm_ymax = ymax;
    return true;
}

// 模型有时会把 JSON 包在 Markdown 代码块围栏里（```json ... ```）。
// 严格解析会失败，这里剥离围栏后再解析。
std::string StripCodeFence(const std::string& s) {
    std::string t = str::TrimA(s);
    if (t.size() < 7) return t;
    if (!str::StartsWith(t, "```")) return t;

    // 去掉起始的 ``` 及其后的语言标记
    const size_t firstNl = t.find('\n');
    if (firstNl == std::string::npos) return t;
    std::string body = t.substr(firstNl + 1);

    // 去掉结尾的 ```
    const size_t lastFence = body.rfind("```");
    if (lastFence != std::string::npos) {
        body = body.substr(0, lastFence);
    }
    return str::TrimA(body);
}

// 从响应体中取出 choices[0].message.content
bool ExtractMessageContent(const std::string& body, std::string& out,
                           std::wstring& err, int& http_hint) {
    out.clear();
    json j;
    try {
        j = json::parse(body);
    } catch (const std::exception& e) {
        err = L"无法解析服务端返回的 JSON：" + str::FromUtf8(e.what());
        return false;
    }

    if (j.contains("error") && j["error"].is_object()) {
        std::string msg;
        if (j["error"].contains("message") && j["error"]["message"].is_string()) {
            msg = j["error"]["message"].get<std::string>();
        }
        err = msg.empty() ? L"服务端返回错误" : L"服务端错误：" + str::FromUtf8(msg);
        return false;
    }

    if (!j.contains("choices") || !j["choices"].is_array() || j["choices"].empty()) {
        err = L"服务端返回内容为空（缺少 choices）";
        return false;
    }

    const json& choice = j["choices"][0];
    if (choice.contains("finish_reason") && choice["finish_reason"].is_string()) {
        const std::wstring fr = str::FromUtf8(choice["finish_reason"].get<std::string>());
        if (fr == L"length") {
            err = L"模型输出被截断（选区内容过多），请缩小框选范围后重试。";
            return false;
        }
        if (fr == L"content_filter") {
            err = L"内容被服务端安全策略拦截。";
            return false;
        }
    }

    if (!choice.contains("message") || !choice["message"].is_object()) {
        err = L"服务端返回结构异常（缺少 message）";
        return false;
    }
    const json& msg = choice["message"];
    if (!msg.contains("content") || !msg["content"].is_string()) {
        if (msg.contains("reasoning_content")) {
            err = L"模型只返回了思考过程，请确认已关闭思考模式。";
        } else {
            err = L"服务端未返回识别内容。";
        }
        return false;
    }

    out = msg["content"].get<std::string>();
    (void)http_hint;
    return true;
}

} // namespace

const wchar_t* AiVisionProvider::PlainPrompt() {
    // 功能二：只要完整译文，不要原文、不要解释
    return L"请识别图片中的所有文字，并将其完整、准确地翻译为自然流畅的简体中文。"
           L"只输出中文译文，不要输出原文，也不要添加任何解释说明。"
           L"如果图片中没有可识别的文字，只输出：未检测到文字。"
           L"请保留原文的段落结构，段落之间用换行分隔。";
}

const wchar_t* AiVisionProvider::BlocksPrompt() {
    // 功能三：需要分块坐标
    return L"请识别图片中的所有独立文本块（按段落、UI 标签或对话气泡分块），"
           L"将其翻译为简体中文，并输出每个文本块在图中的归一化边界框坐标 "
           L"[ymin, xmin, ymax, xmax]（范围 0 到 1000，左上角为原点）。\n"
           L"请严格只输出如下格式的合法 JSON，不要包含 Markdown 代码块标记"
           L"或其他多余文字：\n"
           L"{\"blocks\":[{\"original\":\"原文\",\"translation\":\"中文译文\","
           L"\"box_2d\":[ymin,xmin,ymax,xmax]}]}\n"
           L"注意：\n"
           L"1. box_2d 的四个数字必须落在 0~1000 之间；\n"
           L"2. 每个文本块的坐标要紧密贴合该文本的实际位置；\n"
           L"3. 只翻译非中文文本，已是中文的文本块请勿输出；\n"
           L"4. 不要输出任何 JSON 之外的字符。";
}

// ---------------------------------------------------------------- JSON 解析

bool AiVisionProvider::ParseBlocksJson(const std::string& body,
                                       RecognitionResult& result) {
    result.blocks.clear();
    result.full_translation.clear();

    // 模型可能带 Markdown 围栏，先剥离
    const std::string cleaned = StripCodeFence(body);

    json j;
    try {
        j = json::parse(cleaned);
    } catch (const std::exception& e) {
        result.error_message = L"模型返回的不是合法 JSON：" + str::FromUtf8(e.what());
        return false;
    }

    if (j.contains("error") && j["error"].is_object()) {
        std::string msg;
        if (j["error"].contains("message") && j["error"]["message"].is_string()) {
            msg = j["error"]["message"].get<std::string>();
        }
        result.error_message = msg.empty() ? L"模型返回错误" : str::FromUtf8(msg);
        return false;
    }

    if (!j.contains("blocks") || !j["blocks"].is_array()) {
        result.error_message = L"模型返回的 JSON 缺少 blocks 数组。";
        return false;
    }

    int skippedEmpty = 0, skippedBox = 0, skippedCoords = 0;

    for (const auto& item : j["blocks"]) {
        if (!item.is_object()) { ++skippedCoords; continue; }

        std::wstring original, translation;
        if (item.contains("original") && item["original"].is_string()) {
            original = str::FromUtf8(item["original"].get<std::string>());
        }
        if (item.contains("translation") && item["translation"].is_string()) {
            translation = str::FromUtf8(item["translation"].get<std::string>());
        }

        // 译文为空直接丢弃（规范要求）
        if (str::Trim(translation).empty()) { ++skippedEmpty; continue; }

        if (!item.contains("box_2d") || !item["box_2d"].is_array() ||
            item["box_2d"].size() != 4) {
            ++skippedBox;
            continue;
        }

        float raw[4]{};
        bool numeric = true;
        for (int i = 0; i < 4; ++i) {
            const auto& v = item["box_2d"][static_cast<size_t>(i)];
            if (v.is_number()) {
                raw[i] = v.get<float>();
            } else {
                numeric = false;
                break;
            }
        }
        if (!numeric) { ++skippedCoords; continue; }

        TranslatedTextBlock blk{};
        blk.original_text   = original;
        blk.translated_text = str::Trim(translation);
        if (!NormalizeBox(raw, blk)) { ++skippedCoords; continue; }

        result.blocks.push_back(std::move(blk));
    }

    if (result.blocks.empty()) {
        result.error_message = L"未从模型中解析到有效的文本块"
                               L"（可能图中没有可翻译的外文）。";
        return false;
    }

    // 拼接完整译文（供功能二/兜底显示）
    for (size_t i = 0; i < result.blocks.size(); ++i) {
        if (i > 0) result.full_translation += L"\n";
        result.full_translation += result.blocks[i].translated_text;
    }

    if (skippedEmpty || skippedBox || skippedCoords) {
        LT_INFO(L"文本块解析: 有效 " + std::to_wstring(result.blocks.size()) +
                L"，丢弃 空译文=" + std::to_wstring(skippedEmpty) +
                L" 缺坐标=" + std::to_wstring(skippedBox) +
                L" 坐标非法=" + std::to_wstring(skippedCoords));
    }
    return true;
}

// ---------------------------------------------------------------- 主流程

RecognitionResult AiVisionProvider::ProcessImage(const std::vector<uint8_t>& jpeg_bytes,
                                                 bool require_bounding_boxes,
                                                 std::atomic<bool>& cancel_token) {
    RecognitionResult result{};

    if (jpeg_bytes.empty()) {
        result.error_message = L"图像数据为空，无法识别。";
        return result;
    }

    const std::wstring url   = cfg_.EffectiveVisionUrl();
    const std::wstring model = cfg_.EffectiveVisionModel();
    const std::string  key   = cfg_.EffectiveVisionKey();

    if (key.empty()) {
        result.error_message = L"尚未配置 API Key，请在设置中填写。";
        return result;
    }
    if (url.empty()) {
        result.error_message = L"尚未配置 API 地址，请在设置中填写。";
        return result;
    }

    // ---- 构造请求体（OpenAI 视觉格式）----
    const std::string b64 = str::Base64Encode(jpeg_bytes);
    const std::string dataUrl = "data:image/jpeg;base64," + b64;

    json payload;
    payload["model"]    = str::ToUtf8(model);
    payload["stream"]   = false;
    // 显式关闭思考模式（官方默认 enabled），否则识别延迟极高
    payload["thinking"] = { {"type", "disabled"} };

    // 功能三要求严格 JSON 输出
    if (require_bounding_boxes) {
        payload["response_format"] = { {"type", "json_object"} };
    }

    const std::string prompt = str::ToUtf8(
        require_bounding_boxes ? BlocksPrompt() : PlainPrompt());

    json imageUrl = { {"url", dataUrl} };
    // detail 仅在用户配置了非空值且不是 auto 时下发（auto 是默认行为）
    const std::wstring detail = cfg_.vision_image_detail;
    if (!detail.empty() && detail != L"auto") {
        imageUrl["detail"] = str::ToUtf8(detail);
    }

    payload["messages"] = json::array({
        json{
            {"role", "user"},
            {"content", json::array({
                json{ {"type", "text"}, {"text", prompt} },
                json{ {"type", "image_url"}, {"image_url", imageUrl} },
            })}
        }
    });

    const std::string body = payload.dump();

    std::vector<std::wstring> headers = {
        L"Content-Type: application/json; charset=utf-8",
        L"Authorization: Bearer " + str::FromUtf8(key),
        L"Accept: application/json",
    };

    LT_INFO(L"发起视觉识别请求，模型=" + model +
            L"，模式=" + (require_bounding_boxes ? L"分块坐标" : L"纯译文") +
            L"，图像 " + std::to_wstring(jpeg_bytes.size()) + L" 字节");

    const HttpResponse resp = WinHttpClient::PostJson(
        url, headers, body, cfg_.request_timeout_ms, cancel_token);

    result.http_status = resp.http_status;

    if (!resp.ok) {
        result.error_message = str::FromUtf8(resp.error);
        LT_ERROR(L"视觉识别请求失败: " + result.error_message);
        return result;
    }
    if (resp.http_status < 200 || resp.http_status >= 300) {
        result.error_message = str::FromUtf8(
            resp.error.empty()
                ? WinHttpClient::DescribeHttpStatus(resp.http_status, resp.body)
                : resp.error);
        LT_ERROR(L"视觉识别 HTTP 错误: " + result.error_message);
        return result;
    }

    std::string content;
    std::wstring err;
    int hint = 0;
    if (!ExtractMessageContent(resp.body, content, err, hint)) {
        result.error_message = err;
        LT_ERROR(L"视觉识别响应解析失败: " + err);
        return result;
    }

    if (!require_bounding_boxes) {
        // 功能二：纯译文
        std::wstring text = str::Trim(str::FromUtf8(content));
        if (text.empty()) {
            result.error_message = L"模型未返回译文。";
            return result;
        }
        // 模型有时会加引号包裹，去掉首尾成对的引号
        if (text.size() >= 2 && text.front() == L'"' && text.back() == L'"') {
            text = text.substr(1, text.size() - 2);
        }
        result.success          = true;
        result.full_translation = text;
        LT_INFO(L"视觉识别完成（纯译文），耗时 " + std::to_wstring(resp.elapsed_ms) +
                L" ms，译文长度 " + std::to_wstring(text.size()));
        return result;
    }

    // 功能三：解析 blocks
    if (!ParseBlocksJson(content, result)) {
        LT_ERROR(L"视觉识别 blocks 解析失败: " + result.error_message);
        return result;
    }

    result.success = true;
    LT_INFO(L"视觉识别完成（分块），耗时 " + std::to_wstring(resp.elapsed_ms) +
            L" ms，文本块 " + std::to_wstring(result.blocks.size()) + L" 个");
    return result;
}

} // namespace lt
