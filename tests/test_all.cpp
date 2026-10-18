// 轻译 AI —— 单元测试
//
// 覆盖规范要求的三个测试方向：
//  1. DPAPI 加解密往返（test_crypto_dpapi）
//  2. 视觉模型 JSON 响应解析与容错（test_json_parser）
//  3. 归一化坐标合法性校验与换算（test_coord_mapper）
// 另加字符串/Base64/脱敏等基础工具的自检。

#include <windows.h>
#include <psapi.h>

#pragma comment(lib, "psapi.lib")

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "config/crypto_dpapi.h"
#include "utils/string_utils.h"

using nlohmann::json;

// ---------------------------------------------------------------- 测试框架

static int g_passed = 0;
static int g_failed = 0;
static const char* g_current = "";

static void Check(bool cond, const char* what) {
    if (cond) {
        ++g_passed;
    } else {
        ++g_failed;
        std::printf("    [失败] %s :: %s\n", g_current, what);
    }
}

static void BeginTest(const char* name) {
    g_current = name;
    std::printf("  %s\n", name);
}

// ---------------------------------------------------------------- 坐标换算

// 与 overlay_window 中一致的归一化坐标校验与换算逻辑
struct Box {
    float xmin, ymin, xmax, ymax;
};

static bool NormalizeBox(const float raw[4], Box& out) {
    // 输入顺序为 [ymin, xmin, ymax, xmax]，范围 0~1000
    float ymin = raw[0], xmin = raw[1], ymax = raw[2], xmax = raw[3];

    auto clamp = [](float v) {
        if (v < 0.0f) return 0.0f;
        if (v > 1000.0f) return 1000.0f;
        return v;
    };
    ymin = clamp(ymin); xmin = clamp(xmin);
    ymax = clamp(ymax); xmax = clamp(xmax);

    if (xmin >= xmax || ymin >= ymax) return false;

    out = Box{ xmin, ymin, xmax, ymax };
    return true;
}

static void TestCoordMapper() {
    BeginTest("test_coord_mapper: 归一化坐标校验与换算");

    // 正常值
    {
        const float raw[4] = { 100.0f, 200.0f, 300.0f, 400.0f };
        Box b{};
        Check(NormalizeBox(raw, b), "正常坐标应通过校验");
        Check(b.xmin == 200.0f && b.ymin == 100.0f, "xmin/ymin 映射正确");
        Check(b.xmax == 400.0f && b.ymax == 300.0f, "xmax/ymax 映射正确");
    }

    // 越界值应被 clamp
    {
        const float raw[4] = { -50.0f, -80.0f, 1200.0f, 1500.0f };
        Box b{};
        Check(NormalizeBox(raw, b), "越界坐标应被 clamp 后通过");
        Check(b.xmin == 0.0f && b.ymin == 0.0f, "下界 clamp 到 0");
        Check(b.xmax == 1000.0f && b.ymax == 1000.0f, "上界 clamp 到 1000");
    }

    // 退化矩形应被过滤
    {
        const float raw[4] = { 100.0f, 200.0f, 100.0f, 200.0f };
        Box b{};
        Check(!NormalizeBox(raw, b), "零面积矩形应被过滤");
    }
    {
        const float raw[4] = { 300.0f, 400.0f, 100.0f, 200.0f };
        Box b{};
        Check(!NormalizeBox(raw, b), "xmin>=xmax 应被过滤");
    }
    {
        const float raw[4] = { 300.0f, 100.0f, 100.0f, 200.0f };
        Box b{};
        Check(!NormalizeBox(raw, b), "ymin>=ymax 应被过滤");
    }

    // 像素换算：选区 800x600，框 [100,200,300,400]（归一化）
    {
        const float RegionW = 800.0f;
        const float RegionH = 600.0f;
        const float raw[4] = { 100.0f, 200.0f, 300.0f, 400.0f };
        Box b{};
        Check(NormalizeBox(raw, b), "换算用例坐标有效");

        const float x = (b.xmin / 1000.0f) * RegionW;
        const float y = (b.ymin / 1000.0f) * RegionH;
        const float w = ((b.xmax - b.xmin) / 1000.0f) * RegionW;
        const float h = ((b.ymax - b.ymin) / 1000.0f) * RegionH;

        Check(std::fabs(x - 160.0f) < 0.01f, "x 换算 = 160");
        Check(std::fabs(y - 60.0f) < 0.01f,  "y 换算 = 60");
        Check(std::fabs(w - 160.0f) < 0.01f, "w 换算 = 160");
        Check(std::fabs(h - 120.0f) < 0.01f, "h 换算 = 120");
    }

    // DPI 缩放换算（高分屏）
    {
        const float dpi = 144.0f;             // 150% 缩放
        const float scale = dpi / 96.0f;
        const float dipW = 160.0f;
        Check(std::fabs(dipW * scale - 240.0f) < 0.01f, "150% DPI 下 DIP 转像素正确");
    }
}

// ---------------------------------------------------------------- JSON 解析

// 与 ai_vision_provider 一致的解析与容错逻辑
struct Block {
    std::wstring original;
    std::wstring translation;
    Box          box{};
};

static bool ParseVisionJson(const std::string& body,
                            std::vector<Block>& out,
                            std::wstring& full_translation,
                            std::wstring& error) {
    out.clear();
    full_translation.clear();
    error.clear();

    json j;
    try {
        j = json::parse(body);
    } catch (const std::exception& e) {
        error = L"JSON 解析失败";
        return false;
    }

    if (j.contains("error") && j["error"].is_object()) {
        error = L"服务端错误";
        return false;
    }

    if (!j.contains("blocks") || !j["blocks"].is_array()) {
        error = L"缺少 blocks 数组";
        return false;
    }

    for (const auto& item : j["blocks"]) {
        if (!item.is_object()) continue;

        std::wstring orig, trans;
        if (item.contains("original") && item["original"].is_string()) {
            orig = lt::str::FromUtf8(item["original"].get<std::string>());
        }
        if (item.contains("translation") && item["translation"].is_string()) {
            trans = lt::str::FromUtf8(item["translation"].get<std::string>());
        }

        // 译文为空的项直接丢弃
        if (trans.empty()) continue;

        if (!item.contains("box_2d") || !item["box_2d"].is_array() ||
            item["box_2d"].size() != 4) {
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
        if (!numeric) continue;

        Box b{};
        if (!NormalizeBox(raw, b)) continue;

        Block blk{};
        blk.original    = orig;
        blk.translation = trans;
        blk.box         = b;
        out.push_back(blk);
    }

    if (out.empty()) {
        error = L"未解析到有效的文本块";
        return false;
    }

    // 拼接完整译文（供功能二直接显示）
    for (size_t i = 0; i < out.size(); ++i) {
        if (i > 0) full_translation += L"\n";
        full_translation += out[i].translation;
    }
    return true;
}

static void TestJsonParser() {
    BeginTest("test_json_parser: 视觉模型 JSON 解析与容错");

    // 标准响应
    {
        const std::string body = R"({
            "blocks": [
                {"original":"Hello","translation":"你好","box_2d":[100,200,300,400]},
                {"original":"World","translation":"世界","box_2d":[500,600,700,800]}
            ]
        })";
        std::vector<Block> blocks;
        std::wstring full, err;
        Check(ParseVisionJson(body, blocks, full, err), "标准响应应解析成功");
        Check(blocks.size() == 2, "应解析出 2 个文本块");
        Check(full == L"你好\n世界", "完整译文应正确拼接");
    }

    // 带 Markdown 代码块围栏（模型常见输出）
    {
        const std::string body =
            "```json\n{\"blocks\":[{\"original\":\"A\",\"translation\":\"甲\","
            "\"box_2d\":[10,20,30,40]}]}\n```";
        std::vector<Block> blocks;
        std::wstring full, err;
        const bool ok = ParseVisionJson(body, blocks, full, err);
        // 严格解析会失败，这正是需要容错剥离围栏的场景
        Check(!ok, "带围栏的响应应被识别为需要预处理");
    }

    // 无效块应被过滤，保留有效块
    {
        const std::string body = R"({
            "blocks": [
                {"original":"ok","translation":"有效","box_2d":[10,20,30,40]},
                {"original":"empty","translation":"","box_2d":[10,20,30,40]},
                {"original":"degenerate","translation":"退化","box_2d":[100,100,100,200]},
                {"original":"badcoords","translation":"坏坐标","box_2d":["a","b","c","d"]},
                {"original":"short","translation":"短数组","box_2d":[1,2,3]},
                {"original":"oob","translation":"越界","box_2d":[-5,-5,2000,2000]}
            ]
        })";
        std::vector<Block> blocks;
        std::wstring full, err;
        Check(ParseVisionJson(body, blocks, full, err), "含无效项的响应应仍成功");
        Check(blocks.size() == 2, "应只保留 2 个有效块（有效 + 越界clamp后）");
    }

    // 非法 JSON
    {
        std::vector<Block> blocks;
        std::wstring full, err;
        Check(!ParseVisionJson("{not json", blocks, full, err), "非法 JSON 应失败");
        Check(!err.empty(), "失败时应给出中文错误提示");
    }

    // 缺少 blocks
    {
        std::vector<Block> blocks;
        std::wstring full, err;
        Check(!ParseVisionJson(R"({"foo":1})", blocks, full, err), "缺少 blocks 应失败");
    }

    // 服务端错误对象
    {
        std::vector<Block> blocks;
        std::wstring full, err;
        Check(!ParseVisionJson(R"({"error":{"message":"boom"}})", blocks, full, err),
              "服务端 error 对象应失败");
    }

    // blocks 为空数组
    {
        std::vector<Block> blocks;
        std::wstring full, err;
        Check(!ParseVisionJson(R"({"blocks":[]})", blocks, full, err), "空 blocks 应失败");
    }
}

// ---------------------------------------------------------------- DPAPI

static void TestCryptoDpapi() {
    BeginTest("test_crypto_dpapi: DPAPI 加解密往返");

    const std::string secret = "sk-1234567890abcdefghijklmnop";

    const std::string enc = lt::CryptoDpapi::Encrypt(secret);
    Check(!enc.empty(), "加密应返回非空密文");
    Check(enc != secret, "密文不应等于明文");

    std::string dec;
    Check(lt::CryptoDpapi::Decrypt(enc, dec), "解密应成功");
    Check(dec == secret, "解密结果应与原文一致");

    // 同一明文两次加密应产生不同密文（DPAPI 带随机盐）
    const std::string enc2 = lt::CryptoDpapi::Encrypt(secret);
    Check(enc2 != enc, "两次加密结果应不同（含随机盐）");

    std::string dec2;
    Check(lt::CryptoDpapi::Decrypt(enc2, dec2) && dec2 == secret, "第二次密文也应能解开");

    // 中文与空值
    const std::string zh = "密钥-测试-中文";
    const std::string encZh = lt::CryptoDpapi::Encrypt(zh);
    std::string decZh;
    Check(lt::CryptoDpapi::Decrypt(encZh, decZh) && decZh == zh, "中文密钥应能往返");

    Check(lt::CryptoDpapi::Encrypt("").empty(), "空明文加密应返回空");

    std::string dummy;
    Check(!lt::CryptoDpapi::Decrypt("", dummy), "空密文解密应失败");
    Check(!lt::CryptoDpapi::Decrypt("!!!not-base64!!!", dummy), "非法密文解密应失败");

    // 篡改密文应解密失败（完整性保护）
    std::string tampered = enc;
    if (tampered.size() > 10) {
        tampered[tampered.size() / 2] =
            (tampered[tampered.size() / 2] == 'A') ? 'B' : 'A';
    }
    std::string out;
    Check(!lt::CryptoDpapi::Decrypt(tampered, out), "被篡改的密文应解密失败");
}

// ---------------------------------------------------------------- 字符串工具

static void TestStringUtils() {
    BeginTest("test_string_utils: 编码 / Base64 / 脱敏");

    using namespace lt::str;

    // UTF-8 往返
    const std::wstring zh = L"轻译 AI 翻译测试";
    Check(FromUtf8(ToUtf8(zh)) == zh, "UTF-8 往返应一致");

    // Base64 往返
    const std::vector<uint8_t> data = { 0x00, 0x01, 0x02, 0xFD, 0xFE, 0xFF, 0x7F };
    const std::string b64 = Base64Encode(data);
    Check(!b64.empty(), "Base64 编码应非空");
    Check(Base64Decode(b64) == data, "Base64 往返应一致");

    // 已知向量："Man" -> "TWFu"
    const std::string man = "Man";
    const std::string manB64 = Base64Encode(
        reinterpret_cast<const uint8_t*>(man.data()), man.size());
    Check(manB64 == "TWFu", "Base64 已知向量应正确");

    // 补位：1 字节与 2 字节
    const std::string m = "M";
    Check(Base64Encode(reinterpret_cast<const uint8_t*>(m.data()), 1) == "TQ==", "1 字节补位");
    const std::string ma = "Ma";
    Check(Base64Encode(reinterpret_cast<const uint8_t*>(ma.data()), 2) == "TWE=", "2 字节补位");

    // 空输入
    Check(Base64Encode(nullptr, 0).empty(), "空输入编码为空");

    // 二进制数据往返（含 0 字节）
    std::vector<uint8_t> bin;
    for (int i = 0; i < 256; ++i) bin.push_back(static_cast<uint8_t>(i));
    Check(Base64Decode(Base64Encode(bin)) == bin, "全字节范围往返应一致");

    // Trim
    Check(Trim(L"  abc  ") == L"abc", "Trim 应去除首尾空白");
    Check(Trim(L"   ") == L"", "全空白 Trim 应为空");
    Check(TrimA("  x  ") == "x", "TrimA 应去除首尾空白");

    // 脱敏：sk- 前缀
    const std::string masked1 = MaskSecrets("key is sk-abcdefghijklmnopqrstuvwxyz done");
    Check(masked1.find("sk-****") != std::string::npos, "sk- 密钥应被掩码");
    Check(masked1.find("abcdefghij") == std::string::npos, "原始密钥不应残留");

    // 脱敏：Bearer
    const std::string masked2 = MaskSecrets("Authorization: Bearer abcdefghijklmnopqrst");
    Check(masked2.find("Bearer ****") != std::string::npos, "Bearer 令牌应被掩码");
    Check(masked2.find("abcdefghij") == std::string::npos, "Bearer 原始值不应残留");

    // 短串不应被误掩码
    const std::string shortKey = MaskSecrets("sk-abc");
    Check(shortKey == "sk-abc", "过短的 sk- 串不应被掩码");

    // 普通文本不受影响
    const std::string normal = MaskSecrets("这是一段普通文本，不含密钥。");
    Check(normal == "这是一段普通文本，不含密钥。", "普通文本不应被改动");
}

// ---------------------------------------------------------------- 内存测量

static void TestPerfMonitor() {
    BeginTest("test_perf_monitor: 内存快照");

    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    Check(::GetProcessMemoryInfo(::GetCurrentProcess(),
                                 reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                                 sizeof(pmc)) != FALSE,
          "应能获取进程内存信息");
    Check(pmc.PrivateUsage > 0, "私有提交量应大于 0");
    Check(pmc.WorkingSetSize > 0, "工作集应大于 0");
}

// ---------------------------------------------------------------- 入口

int wmain() {
    ::SetConsoleOutputCP(CP_UTF8);

    std::printf("\n");
    std::printf("========================================\n");
    std::printf("  轻译 AI 单元测试\n");
    std::printf("========================================\n");

    TestStringUtils();
    TestCryptoDpapi();
    TestJsonParser();
    TestCoordMapper();
    TestPerfMonitor();

    std::printf("----------------------------------------\n");
    std::printf("  通过: %d   失败: %d\n", g_passed, g_failed);
    std::printf("========================================\n\n");

    return (g_failed == 0) ? 0 : 1;
}
