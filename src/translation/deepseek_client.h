#pragma once

#include <atomic>
#include <string>

#include "config/config_manager.h"

namespace lt {

struct TranslationResult {
    bool         success = false;
    int          http_status = 0;
    std::wstring translated;       // 中文译文（成功时）
    std::wstring error_message;    // 简体中文错误提示（失败时）
    std::wstring model_used;
    int          elapsed_ms = 0;
};

// DeepSeek Chat Completions 客户端。
//
// 与原始规范的重要差异（依据 2026-09 DeepSeek 官方文档校正）：
//  1. 默认模型为 deepseek-flash —— 旧名 deepseek-chat 已于 2026-07-24 下线。
//  2. 显式关闭思考模式（thinking.type = "disabled"）。
//     官方默认 thinking.type = "enabled"，不关掉会让每次翻译都先"想"很久，
//     对翻译这种确定性任务毫无收益，且显著增加延迟与 token 消耗。
//  3. 支持多模态 image_url（deepseek-flash 原生支持视觉），
//     功能二/三复用同一套 API Key。
class DeepSeekClient {
public:
    DeepSeekClient() = default;

    // 纯文本翻译（功能一）
    static TranslationResult TranslateText(const std::wstring& text,
                                           const AppConfig&    cfg,
                                           std::atomic<bool>&  cancel);

    // 连通性测试：发一个极小的请求验证 Key 与地址是否可用
    static TranslationResult TestConnection(const AppConfig& cfg, bool use_vision);

    // 内置 System Prompt（功能一）
    static const wchar_t* TextSystemPrompt();

private:
    // 从 Chat Completions 响应 JSON 中抽取 choices[0].message.content
    static bool ExtractContent(const std::string& body,
                               std::wstring&      out,
                               std::wstring&      err);
};

} // namespace lt
