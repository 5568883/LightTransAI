#include "recognition/local_ocr_provider.h"

namespace lt {

RecognitionResult LocalOcrProvider::ProcessImage(
    const std::vector<uint8_t>& jpeg_bytes,
    bool                        require_bounding_boxes,
    std::atomic<bool>&          cancel_token) {
    (void)jpeg_bytes;
    (void)require_bounding_boxes;
    (void)cancel_token;

    RecognitionResult result{};
    result.success = false;
    result.error_message =
        L"当前版本仅启用 AI 直接识图，本地 OCR 模块接口已预留。\n"
        L"请在设置中把识别模式改回「AI 直接识图（默认）」。";
    return result;
}

} // namespace lt
