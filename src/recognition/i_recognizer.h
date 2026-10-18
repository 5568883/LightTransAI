#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace lt {

// 单个文本块：原文 + 中文译文 + 归一化坐标
struct TranslatedTextBlock {
    std::wstring original_text;    // 识别出的原文
    std::wstring translated_text;  // 中文译文
    float norm_xmin = 0.0f;        // 归一化坐标 (0.0 ~ 1000.0)
    float norm_ymin = 0.0f;
    float norm_xmax = 0.0f;
    float norm_ymax = 0.0f;
};

struct RecognitionResult {
    bool         success = false;
    int          http_status = 0;
    std::wstring error_message;               // 简体中文错误提示
    std::wstring full_translation;            // 供功能二直接显示的完整中文译文
    std::vector<TranslatedTextBlock> blocks;  // 供功能三使用的分块坐标与译文列表
};

enum class RecognitionMode {
    AiVisionDirect = 0,  // 第一版实现：AI 视觉大模型直接识图并翻译
    LocalOcrReserve = 1  // 预留扩展：本地 OCR 识图 + DeepSeek 文本翻译
};

class IRecognitionProvider {
public:
    virtual ~IRecognitionProvider() = default;

    // jpeg_bytes: 已编码的选区图像
    // require_bounding_boxes: true 时要求返回分块坐标（功能三），
    //                         false 时只需完整译文（功能二）
    // cancel_token: 置位后应尽快中断并返回
    virtual RecognitionResult ProcessImage(
        const std::vector<uint8_t>& jpeg_bytes,
        bool                        require_bounding_boxes,
        std::atomic<bool>&          cancel_token) = 0;

    // 提供者名称（用于日志与界面显示）
    virtual const wchar_t* Name() const = 0;
};

} // namespace lt
