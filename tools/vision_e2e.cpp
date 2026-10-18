// 功能二真实 API 端到端测试
//
// 从配置读取真实 API Key，构造一张含英文的图，走完整视觉识别链路。
// 这是功能二最关键的一步验证：真实网络 + 真实模型 + 真实返回。
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

#include "capture/image_encoder.h"
#include "config/config_manager.h"
#include "recognition/ai_vision_provider.h"
#include "utils/logger.h"
#include "utils/string_utils.h"

static FILE* g = nullptr;
static void Say(const char* fmt, ...) {
    if (!g) return;
    va_list ap; va_start(ap, fmt);
    std::vfprintf(g, fmt, ap);
    va_end(ap); std::fflush(g);
}
static void SayW(const char* tag, const std::wstring& w) {
    Say("%s: %s\n", tag, lt::str::ToUtf8(w).c_str());
}

// 生成一张含英文文本的测试图（用 GDI 绘制，避免依赖外部文件）
static bool MakeTestImage(lt::BitmapBuffer& out) {
    const int W = 640, H = 200;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = W;
    bmi.bmiHeader.biHeight      = -H;
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    HDC screen = ::GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP bmp = ::CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HDC dc = ::CreateCompatibleDC(screen);
    ::ReleaseDC(nullptr, screen);
    if (!bmp || !bits || !dc) return false;

    HGDIOBJ old = ::SelectObject(dc, bmp);

    // 白底
    RECT full{ 0, 0, W, H };
    HBRUSH white = ::CreateSolidBrush(RGB(255, 255, 255));
    ::FillRect(dc, &full, white);
    ::DeleteObject(white);

    // 黑字
    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, RGB(0, 0, 0));
    HFONT font = ::CreateFontW(-32, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                               ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Arial");
    HGDIOBJ oldFont = ::SelectObject(dc, font);

    RECT r1{ 20, 30, W - 20, 80 };
    ::DrawTextW(dc, L"This download installs Intel Graphics Driver.",
                -1, &r1, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
    RECT r2{ 20, 90, W - 20, 140 };
    ::DrawTextW(dc, L"Version 32.0.101.9150 WHQL Certified.",
                -1, &r2, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);

    ::SelectObject(dc, oldFont);
    ::DeleteObject(font);
    ::SelectObject(dc, old);
    ::DeleteDC(dc);

    const size_t bytes = static_cast<size_t>(W) * H * 4;
    out.width = W;
    out.height = H;
    out.pixels.assign(static_cast<uint8_t*>(bits),
                      static_cast<uint8_t*>(bits) + bytes);
    for (size_t i = 3; i < out.pixels.size(); i += 4) out.pixels[i] = 0xFF;

    ::DeleteObject(bmp);
    return true;
}

int wmain() {
    g = _wfopen(L"D:\\LightTransAI\\vision_e2e_result.txt", L"wb");
    if (!g) return 2;
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    lt::Logger::Instance().Init(L"D:\\LightTransAI\\vision_e2e.log");

    Say("======== 功能二 真实 API 端到端测试 ========\n\n");

    auto& cm = lt::ConfigManager::Instance();
    cm.Load();
    const lt::AppConfig& cfg = cm.Get();

    Say("[配置]\n");
    Say("  API 地址: %s\n", lt::str::ToUtf8(cfg.EffectiveVisionUrl()).c_str());
    Say("  模型    : %s\n", lt::str::ToUtf8(cfg.EffectiveVisionModel()).c_str());
    Say("  Key     : %s\n", cfg.EffectiveVisionKey().empty() ? "<未配置>" : "<已配置>");
    Say("  detail  : %s\n\n", lt::str::ToUtf8(cfg.vision_image_detail).c_str());

    if (cfg.EffectiveVisionKey().empty()) {
        Say("[跳过] 未配置 API Key，无法测试真实请求。\n");
        Say("       请先在设置中填写 Key，再运行本测试。\n");
        if (g) std::fclose(g);
        return 2;
    }

    // ---- 1. 生成测试图 ----
    Say("[1] 生成测试图\n");
    lt::BitmapBuffer img;
    if (!MakeTestImage(img)) {
        Say("  [失败] 生成测试图失败\n");
        if (g) std::fclose(g);
        return 1;
    }
    Say("  %dx%d, %zu 字节 BGRA\n", img.width, img.height, img.pixels.size());

    // ---- 2. 编码 ----
    Say("\n[2] 编码 JPEG\n");
    std::vector<uint8_t> jpeg;
    if (!lt::ImageEncoder::EncodeJpeg(img, 92, jpeg) || jpeg.empty()) {
        Say("  [失败] JPEG 编码失败\n");
        if (g) std::fclose(g);
        return 1;
    }
    Say("  JPEG %zu 字节, 预估 %d token\n", jpeg.size(),
        lt::ImageEncoder::EstimateImageTokens(img.width, img.height));

    // ---- 3. 功能二：纯译文 ----
    Say("\n[3] 功能二：纯译文模式（require_bounding_boxes=false）\n");
    {
        lt::AiVisionProvider provider(cfg);
        std::atomic<bool> cancel{false};
        const DWORD t0 = ::GetTickCount();
        const lt::RecognitionResult r = provider.ProcessImage(jpeg, false, cancel);
        const DWORD ms = ::GetTickCount() - t0;

        Say("  success=%d  http=%d  耗时=%lu ms\n",
            r.success ? 1 : 0, r.http_status, (unsigned long)ms);
        if (r.success) {
            SayW("  译文", r.full_translation);
            const bool hasChinese = r.full_translation.find(L'这') != std::wstring::npos ||
                                    r.full_translation.find(L'版') != std::wstring::npos ||
                                    r.full_translation.find(L'驱') != std::wstring::npos ||
                                    r.full_translation.find(L'认') != std::wstring::npos ||
                                    r.full_translation.find(L'下') != std::wstring::npos;
            Say("  含中文: %s\n", hasChinese ? "是" : "（未检出常见中文字，请人工确认）");
        } else {
            SayW("  错误", r.error_message);
        }
    }

    // ---- 4. 功能三：分块坐标（提前验证解析链路）----
    Say("\n[4] 功能三：分块坐标模式（require_bounding_boxes=true）\n");
    {
        lt::AiVisionProvider provider(cfg);
        std::atomic<bool> cancel{false};
        const DWORD t0 = ::GetTickCount();
        const lt::RecognitionResult r = provider.ProcessImage(jpeg, true, cancel);
        const DWORD ms = ::GetTickCount() - t0;

        Say("  success=%d  http=%d  耗时=%lu ms\n",
            r.success ? 1 : 0, r.http_status, (unsigned long)ms);
        if (r.success) {
            Say("  文本块数: %zu\n", r.blocks.size());
            for (size_t i = 0; i < r.blocks.size() && i < 6; ++i) {
                const auto& b = r.blocks[i];
                Say("    [%zu] %.30s -> %.30s  box=[%.0f,%.0f,%.0f,%.0f]\n",
                    i,
                    lt::str::ToUtf8(b.original_text).c_str(),
                    lt::str::ToUtf8(b.translated_text).c_str(),
                    b.norm_ymin, b.norm_xmin, b.norm_ymax, b.norm_xmax);
            }
            // 校验坐标合法性
            bool allValid = true;
            for (const auto& b : r.blocks) {
                if (b.norm_xmin < 0 || b.norm_xmax > 1000 ||
                    b.norm_ymin < 0 || b.norm_ymax > 1000 ||
                    b.norm_xmin >= b.norm_xmax || b.norm_ymin >= b.norm_ymax) {
                    allValid = false;
                }
            }
            Say("  所有坐标合法: %s\n", allValid ? "是" : "否");
        } else {
            SayW("  错误", r.error_message);
        }
    }

    Say("\n======== 测试完成 ========\n");
    ::CoUninitialize();
    if (g) std::fclose(g);
    return 0;
}
