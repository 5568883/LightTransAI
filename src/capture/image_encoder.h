#pragma once

#include <windows.h>
#include <wincodec.h>

#include <cstdint>
#include <string>
#include <vector>

namespace lt {

// 一块 32bpp BGRA 位图（自上而下）。
//
// 为什么保留原始 BGRA 而不是只留 JPEG：
// 功能三需要采样文本框边缘的背景色来决定译文颜色与底框，
// 这必须从原始像素取；从有损 JPEG 取会得到块状伪影，导致颜色判断错误。
struct BitmapBuffer {
    int                  width  = 0;
    int                  height = 0;
    std::vector<uint8_t> pixels;   // BGRA，每行 width*4 字节，无填充

    bool Valid() const {
        return width > 0 && height > 0 &&
               pixels.size() >= static_cast<size_t>(width) * height * 4;
    }

    // 取某像素的 BGR 分量（越界返回黑色）
    void GetBgr(int x, int y, uint8_t& b, uint8_t& g, uint8_t& r) const {
        b = g = r = 0;
        if (!Valid() || x < 0 || y < 0 || x >= width || y >= height) return;
        const size_t i = (static_cast<size_t>(y) * width + x) * 4;
        b = pixels[i];
        g = pixels[i + 1];
        r = pixels[i + 2];
    }
};

// 基于 Windows 原生 WIC 的图像处理。
// 全部在内存中完成，不产生临时文件。
class ImageEncoder {
public:
    // 抓取虚拟屏幕（多显示器合并）到 32bpp BGRA 位图。
    // 返回的位图坐标为屏幕物理像素，左上角对应 (SM_XVIRTUALSCREEN, SM_YVIRTUALSCREEN)。
    static bool CaptureVirtualScreen(BitmapBuffer& out);

    // 从源位图裁剪矩形区域。rect 为源位图坐标。
    static bool Crop(const BitmapBuffer& src, const RECT& rect, BitmapBuffer& out);

    // 把位图编码为 JPEG 字节流。
    // quality: 1~100。注意 JPEG 质量**不影响 API token 消耗** ——
    // 官方按图片像素尺寸计费（每图上限 1024 token），与文件体积无关。
    // 因此质量只影响上传带宽与 OCR 准确率，默认取较高值以保证识别质量。
    static bool EncodeJpeg(const BitmapBuffer& bmp, int quality,
                           std::vector<uint8_t>& out);

    // 把位图按最长边缩放到不超过 max_dim（保持宽高比）。
    // 这是控制 token 消耗的**正确手段**（而非降低 JPEG 质量）。
    //
    // 契约：src 按值传入，函数可以直接**接管**它的像素缓冲；
    // 调用方传进来之后就不应再依赖 src 的内容（尺寸字段仍有效）。
    // 这样设计是为了避免"原图已在限制内"时白白复制一整份像素 ——
    // 2560x1440 就是 14.7 MB，而框选区域通常都小于 1600px。
    static bool ResizeToFit(BitmapBuffer src, int max_dim, BitmapBuffer& out);

    // 组合流程：抓屏 → 裁剪 → 缩放 → 编码 JPEG → Base64。
    // 同时返回裁剪后的原始位图（供覆盖层采样背景色使用）。
    static bool CaptureRegionToBase64Jpeg(const RECT& region,
                                          std::vector<uint8_t>& jpeg_out,
                                          std::string& base64_out,
                                          BitmapBuffer& cropped_bgra_out,
                                          int jpeg_quality = 92,
                                          int max_dim = 1600);

    // 估算上传该图会消耗的 token 数（官方规则：按尺寸折算，上限 1024）
    static int EstimateImageTokens(int width, int height);
};

} // namespace lt
