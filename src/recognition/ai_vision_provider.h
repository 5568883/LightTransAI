#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "config/config_manager.h"
#include "recognition/i_recognizer.h"

namespace lt {

// 通过兼容 OpenAI 视觉接口的多模态模型直接识图并翻译。
//
// 默认复用 DeepSeek 主配置（deepseek-flash 原生支持视觉），
// 也可在设置中指定独立的视觉模型（Qwen-VL / Doubao / GPT-4o 等）。
//
// require_bounding_boxes = false 时（功能二）：
//   只要求返回完整中文译文，输出简洁。
// require_bounding_boxes = true 时（功能三）：
//   要求返回 blocks 数组，每项含 original / translation / box_2d，
//   并启用 response_format=json_object 保证输出合法 JSON。
class AiVisionProvider : public IRecognitionProvider {
public:
    explicit AiVisionProvider(const AppConfig& cfg) : cfg_(cfg) {}
    ~AiVisionProvider() override = default;

    RecognitionResult ProcessImage(const std::vector<uint8_t>& jpeg_bytes,
                                   bool                        require_bounding_boxes,
                                   std::atomic<bool>&          cancel_token) override;

    const wchar_t* Name() const override { return L"AI 直接识图"; }

    // 内置 Prompt（供单元测试与诊断引用）
    static const wchar_t* PlainPrompt();
    static const wchar_t* BlocksPrompt();

    // 解析视觉模型的 JSON 响应。独立成静态函数以便单元测试。
    // 返回是否解析成功；成功时填充 result。
    static bool ParseBlocksJson(const std::string& body,
                                RecognitionResult& result);

private:
    AppConfig cfg_;
};

} // namespace lt
