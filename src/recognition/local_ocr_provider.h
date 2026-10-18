#pragma once

#include "recognition/i_recognizer.h"

namespace lt {

class LocalOcrProvider : public IRecognitionProvider {
public:
    LocalOcrProvider() = default;
    ~LocalOcrProvider() override = default;

    RecognitionResult ProcessImage(const std::vector<uint8_t>& jpeg_bytes,
                                   bool                        require_bounding_boxes,
                                   std::atomic<bool>&          cancel_token) override;

    const wchar_t* Name() const override { return L"本地 OCR（预留）"; }
};

} // namespace lt
