// 功能二（框选截图翻译）链路验证
//
// 分三步验证，每步都独立可判定：
//   1. 抓屏 + 裁剪 + 缩放 + JPEG 编码 + Base64
//   2. 视觉模型 JSON 解析（含各种畸形输入）
//   3. 坐标校验与 token 估算
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

#include "capture/image_encoder.h"
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

static int g_pass = 0, g_fail = 0;
static void Check(bool ok, const char* what) {
    if (ok) { ++g_pass; }
    else { ++g_fail; Say("    [失败] %s\n", what); }
}

int wmain() {
    g = _wfopen(L"D:\\LightTransAI\\phase3_result.txt", L"wb");
    if (!g) return 2;
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    lt::Logger::Instance().Init(L"D:\\LightTransAI\\phase3_probe.log");

    Say("======== 功能二链路验证 ========\n\n");

    // ============ 1. 图像编码 ============
    Say("--- [1] 抓屏 / 裁剪 / 缩放 / JPEG / Base64 ---\n");
    {
        lt::BitmapBuffer full;
        const bool capOk = lt::ImageEncoder::CaptureVirtualScreen(full);
        Say("  抓取虚拟屏幕: %s", capOk ? "成功" : "失败");
        if (capOk) Say("  (%dx%d)", full.width, full.height);
        Say("\n");
        Check(capOk, "抓屏应成功");
        Check(full.Valid(), "位图应有效");

        if (capOk) {
            // 检查像素不透明（BitBlt 不写 alpha，必须手工补 0xFF）
            int opaque = 0, transparent = 0;
            for (size_t i = 3; i < full.pixels.size(); i += 4) {
                if (full.pixels[i] == 0xFF) ++opaque; else ++transparent;
            }
            Say("  不透明像素 %d / 透明像素 %d\n", opaque, transparent);
            Check(transparent == 0, "截图像素必须全部不透明（alpha=0xFF）");

            // 裁一块 400x300
            RECT r{ 100, 100, 500, 400 };
            lt::BitmapBuffer cropped;
            const bool cropOk = lt::ImageEncoder::Crop(full, r, cropped);
            Say("  裁剪 400x300: %s  (%dx%d)\n", cropOk ? "成功" : "失败",
                cropped.width, cropped.height);
            Check(cropOk && cropped.width == 400 && cropped.height == 300,
                  "裁剪尺寸应正确");

            // 缩放
            lt::BitmapBuffer scaled;
            const bool scaleOk = lt::ImageEncoder::ResizeToFit(cropped, 200, scaled);
            Say("  缩放到最长边 200: %s  (%dx%d)\n", scaleOk ? "成功" : "失败",
                scaled.width, scaled.height);
            Check(scaleOk && scaled.width <= 200 && scaled.height <= 200,
                  "缩放后最长边应不超过限制");

            // JPEG 编码
            std::vector<uint8_t> jpeg;
            const bool encOk = lt::ImageEncoder::EncodeJpeg(cropped, 92, jpeg);
            Say("  JPEG 编码: %s  (%zu 字节)\n", encOk ? "成功" : "失败", jpeg.size());
            Check(encOk && !jpeg.empty(), "JPEG 编码应成功且非空");
            // JPEG 魔数 FF D8 FF
            if (jpeg.size() >= 3) {
                const bool magic = (jpeg[0] == 0xFF && jpeg[1] == 0xD8 && jpeg[2] == 0xFF);
                Say("  JPEG 魔数: %s\n", magic ? "正确 (FF D8 FF)" : "错误");
                Check(magic, "JPEG 应以 FF D8 FF 开头");
            }

            // Base64
            const std::string b64 = lt::str::Base64Encode(jpeg);
            Say("  Base64: %zu 字符\n", b64.size());
            Check(!b64.empty(), "Base64 应非空");
            // 往返校验
            const std::vector<uint8_t> back = lt::str::Base64Decode(b64);
            Check(back == jpeg, "Base64 往返应一致");

            // 质量对比：质量应影响体积
            std::vector<uint8_t> lowQ;
            lt::ImageEncoder::EncodeJpeg(cropped, 40, lowQ);
            Say("  质量 92: %zu 字节   质量 40: %zu 字节\n", jpeg.size(), lowQ.size());
            Check(lowQ.size() < jpeg.size(), "低质量应产生更小体积");
        }
    }

    // ============ 2. token 估算 ============
    Say("\n--- [2] token 估算 ---\n");
    {
        const int t1 = lt::ImageEncoder::EstimateImageTokens(400, 300);
        const int t2 = lt::ImageEncoder::EstimateImageTokens(1600, 1200);
        const int t3 = lt::ImageEncoder::EstimateImageTokens(4000, 4000);
        Say("  400x300   -> %d token\n", t1);
        Say("  1600x1200 -> %d token\n", t2);
        Say("  4000x4000 -> %d token (上限 1024)\n", t3);
        Check(t1 > 0 && t1 <= 1024, "小图 token 应在范围内");
        Check(t3 <= 1024, "大图 token 不应超过 1024 上限");
        Check(t2 >= t1, "更大尺寸应消耗不少于小图");
    }

    // ============ 3. JSON 解析 ============
    Say("\n--- [3] 视觉模型 JSON 解析 ---\n");
    {
        // 3.1 标准响应
        {
            const std::string body = R"({"blocks":[
                {"original":"Hello","translation":"你好","box_2d":[100,200,300,400]},
                {"original":"World","translation":"世界","box_2d":[500,600,700,800]}]})";
            lt::RecognitionResult r;
            const bool ok = lt::AiVisionProvider::ParseBlocksJson(body, r);
            Say("  标准响应: %s  块数=%zu\n", ok ? "成功" : "失败", r.blocks.size());
            Check(ok && r.blocks.size() == 2, "应解析出 2 个块");
            Check(r.full_translation == L"你好\n世界", "完整译文应正确拼接");
            if (r.blocks.size() == 2) {
                Check(r.blocks[0].norm_xmin == 200.0f && r.blocks[0].norm_ymin == 100.0f,
                      "坐标映射应为 [ymin,xmin,ymax,xmax]");
            }
        }

        // 3.2 带 Markdown 围栏
        {
            const std::string body =
                "```json\n{\"blocks\":[{\"original\":\"A\",\"translation\":\"甲\","
                "\"box_2d\":[10,20,30,40]}]}\n```";
            lt::RecognitionResult r;
            const bool ok = lt::AiVisionProvider::ParseBlocksJson(body, r);
            Say("  带 Markdown 围栏: %s  块数=%zu\n", ok ? "成功" : "失败", r.blocks.size());
            Check(ok && r.blocks.size() == 1, "应能剥离围栏后解析");
        }

        // 3.3 各种无效项过滤
        {
            const std::string body = R"({"blocks":[
                {"original":"ok","translation":"有效","box_2d":[10,20,30,40]},
                {"original":"e","translation":"","box_2d":[10,20,30,40]},
                {"original":"d","translation":"退化","box_2d":[100,100,100,200]},
                {"original":"c","translation":"坏坐标","box_2d":["a","b","c","d"]},
                {"original":"s","translation":"短","box_2d":[1,2,3]},
                {"original":"o","translation":"越界","box_2d":[-5,-5,2000,2000]}]})";
            lt::RecognitionResult r;
            const bool ok = lt::AiVisionProvider::ParseBlocksJson(body, r);
            Say("  混合无效项: %s  有效块=%zu (期望 2)\n",
                ok ? "成功" : "失败", r.blocks.size());
            Check(ok && r.blocks.size() == 2, "应只保留有效块 + 越界clamp后的块");
        }

        // 3.4 非法 JSON
        {
            lt::RecognitionResult r;
            const bool ok = lt::AiVisionProvider::ParseBlocksJson("{not json", r);
            Say("  非法 JSON: %s (应失败)  提示=%s\n", ok ? "误判成功" : "正确失败",
                lt::str::ToUtf8(r.error_message).c_str());
            Check(!ok && !r.error_message.empty(), "非法 JSON 应失败并给出中文提示");
        }

        // 3.5 缺少 blocks
        {
            lt::RecognitionResult r;
            Check(!lt::AiVisionProvider::ParseBlocksJson(R"({"foo":1})", r),
                  "缺少 blocks 应失败");
            Check(!lt::AiVisionProvider::ParseBlocksJson(R"({"blocks":[]})", r),
                  "空 blocks 应失败");
            Check(!lt::AiVisionProvider::ParseBlocksJson(R"({"error":{"message":"x"}})", r),
                  "服务端 error 应失败");
            Say("  缺字段/空数组/error 对象: 均正确失败\n");
        }

        // 3.6 已是中文的块（模型可能误输出）
        {
            const std::string body = R"({"blocks":[
                {"original":"中文","translation":"中文","box_2d":[1,2,3,4]}]})";
            lt::RecognitionResult r;
            const bool ok = lt::AiVisionProvider::ParseBlocksJson(body, r);
            Say("  中文块: %s (解析器不负责过滤，交由 Prompt 约束)\n",
                ok ? "接受" : "拒绝");
            Check(ok, "解析器应接受结构合法的中文块");
        }
    }

    // ============ 4. Prompt 存在性 ============
    Say("\n--- [4] Prompt 模板 ---\n");
    {
        const wchar_t* plain = lt::AiVisionProvider::PlainPrompt();
        const wchar_t* blocks = lt::AiVisionProvider::BlocksPrompt();
        Check(plain && wcslen(plain) > 20, "功能二 Prompt 应非空");
        Check(blocks && wcschr(blocks, L'{') != nullptr, "功能三 Prompt 应含 JSON 示例");
        Check(blocks && wcsstr(blocks, L"box_2d") != nullptr, "功能三 Prompt 应提及 box_2d");
        Say("  功能二 Prompt: %d 字符\n", (int)wcslen(plain));
        Say("  功能三 Prompt: %d 字符\n", (int)wcslen(blocks));
    }

    Say("\n========================================\n");
    Say("  通过: %d   失败: %d\n", g_pass, g_fail);
    Say("========================================\n");

    ::CoUninitialize();
    if (g) std::fclose(g);
    return g_fail == 0 ? 0 : 1;
}
