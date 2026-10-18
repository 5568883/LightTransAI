#include "config/config_manager.h"

#include <windows.h>
#include <shlobj.h>

#include <fstream>
#include <nlohmann/json.hpp>

#include "config/crypto_dpapi.h"
#include "utils/logger.h"
#include "utils/string_utils.h"

using nlohmann::json;

namespace lt {

namespace {
// 安全读取 JSON 字段的小助手：类型不符时保留默认值
template <typename T>
void ReadField(const json& j, const char* key, T& out) {
    const auto it = j.find(key);
    if (it == j.end() || it->is_null()) return;
    if constexpr (std::is_same_v<T, std::wstring>) {
        if (it->is_string()) out = str::FromUtf8(it->get<std::string>());
    } else if constexpr (std::is_same_v<T, std::string>) {
        if (it->is_string()) out = it->get<std::string>();
    } else if constexpr (std::is_same_v<T, bool>) {
        if (it->is_boolean()) out = it->get<bool>();
    } else if constexpr (std::is_same_v<T, int>) {
        if (it->is_number_integer()) out = it->get<int>();
    } else if constexpr (std::is_same_v<T, unsigned int>) {
        if (it->is_number_unsigned() || it->is_number_integer()) {
            out = static_cast<unsigned int>(it->get<long long>());
        }
    }
}

std::string Utf8(const std::wstring& w) { return str::ToUtf8(w); }

// 确认目录存在且确实可写（仅创建成功不代表能写，某些受限环境会拒绝写入）
bool EnsureWritableDir(const std::wstring& dir) {
    if (dir.empty()) return false;
    ::CreateDirectoryW(dir.c_str(), nullptr);   // 已存在时返回 0，无害

    const DWORD attr = ::GetFileAttributesW(dir.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return false;
    }

    // 真实写一个探测文件，避免"目录可建但不可写"的情况
    const std::wstring probe = dir + L"\\.write_probe";
    HANDLE h = ::CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    ::CloseHandle(h);
    ::DeleteFileW(probe.c_str());
    return true;
}

// 取本程序可执行文件所在目录
std::wstring ExecutableDir() {
    wchar_t buf[MAX_PATH * 2]{};
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0) return {};
    std::wstring path(buf, n);
    const size_t slash = path.find_last_of(L'\\');
    return (slash == std::wstring::npos) ? std::wstring{} : path.substr(0, slash);
}
} // namespace

ConfigManager& ConfigManager::Instance() {
    static ConfigManager inst;
    return inst;
}

void ConfigManager::ResolvePaths() {
    // 依次尝试若干可写位置：
    //   1) %APPDATA%\LightTransAI         （标准位置）
    //   2) 程序所在目录\LightTransAI_data （便携模式 / %APPDATA% 受限时）
    //   3) %TEMP%\LightTransAI            （最后的兜底）
    // 某些受限环境（只读配置、容器、沙箱）下 %APPDATA% 不可写，
    // 若不做回退会导致配置与日志静默丢失，用户无从察觉。
    //
    // 注意：目录名仍然沿用 LightTransAI —— 产品显示名已改为「鲤桥翻译」，
    // 但改目录名会让用户已保存的 API Key（DPAPI 密文）找不到，
    // 因此保持目录名不变，只改界面显示用的名称。
    std::vector<std::wstring> candidates;

    wchar_t* appdata = nullptr;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata)) &&
        appdata != nullptr) {
        candidates.push_back(std::wstring(appdata) + L"\\LightTransAI");
        ::CoTaskMemFree(appdata);
    }

    const std::wstring exeDir = ExecutableDir();
    if (!exeDir.empty()) {
        candidates.push_back(exeDir + L"\\LightTransAI_data");
    }

    wchar_t tmp[MAX_PATH]{};
    if (::GetTempPathW(MAX_PATH, tmp) > 0) {
        candidates.push_back(std::wstring(tmp) + L"LightTransAI");
    }

    for (size_t i = 0; i < candidates.size(); ++i) {
        if (EnsureWritableDir(candidates[i])) {
            dir_ = candidates[i];
            if (i != 0) {
                path_warning_ = L"标准配置目录不可写，配置已改存到：" + dir_;
            }
            break;
        }
    }

    if (dir_.empty()) {
        // 全部失败：仍指向第一个候选，让上层拿到明确的失败而不是空路径
        dir_ = candidates.empty() ? L"." : candidates.front();
        path_warning_ = L"找不到可写的配置目录，设置将无法保存。";
    }

    path_ = dir_ + L"\\config.json";
}

bool ConfigManager::EnsureDefaultExists() {
    if (path_.empty()) ResolvePaths();
    const DWORD attr = ::GetFileAttributesW(path_.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES) return false;  // 已存在
    return Save();
}

bool ConfigManager::Load() {
    if (path_.empty()) ResolvePaths();
    decrypt_warning_.clear();

    std::ifstream f(path_, std::ios::binary);
    if (!f) {
        LT_WARN(L"配置文件不存在，使用默认配置");
        return false;
    }

    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    if (content.empty()) {
        LT_WARN(L"配置文件为空，使用默认配置");
        return false;
    }

    json j;
    try {
        j = json::parse(content, nullptr, true, /*ignore_comments=*/true);
    } catch (const std::exception& e) {
        LT_ERROR(L"配置文件解析失败，回退默认配置: " + str::FromUtf8(e.what()));
        return false;
    }
    if (!j.is_object()) {
        LT_ERROR(L"配置文件根节点不是对象，回退默认配置");
        return false;
    }

    AppConfig c{};  // 从默认值出发

    ReadField(j, "api_url", c.api_url);
    ReadField(j, "text_model", c.text_model);
    ReadField(j, "api_key_enc", c.api_key_enc);

    ReadField(j, "vision_api_url", c.vision_api_url);
    ReadField(j, "vision_model", c.vision_model);
    ReadField(j, "vision_api_key_enc", c.vision_api_key_enc);
    ReadField(j, "vision_supports_image", c.vision_supports_image);
    ReadField(j, "vision_image_detail", c.vision_image_detail);

    ReadField(j, "enable_feature1", c.enable_feature1);
    ReadField(j, "enable_feature2", c.enable_feature2);
    ReadField(j, "enable_feature3", c.enable_feature3);

    // 功能一的触发方式。
    //
    // 旧版本只有一个 enable_float_button 布尔开关，含义是"划词后是否弹悬浮按钮"，
    // 而快捷键始终可用。为了不让老配置失效，缺省时按它换算：
    //     enable_float_button = false  →  仅快捷键
    //     enable_float_button = true   →  两者都用
    if (j.contains("feature1_trigger")) {
        int trig = static_cast<int>(Feature1Trigger::Both);
        ReadField(j, "feature1_trigger", trig);
        c.feature1_trigger = (trig == 1) ? Feature1Trigger::HotkeyOnly
                           : (trig == 2) ? Feature1Trigger::FloatOnly
                                         : Feature1Trigger::Both;
    } else {
        bool legacyFloat = true;
        ReadField(j, "enable_float_button", legacyFloat);
        c.feature1_trigger = legacyFloat ? Feature1Trigger::Both
                                         : Feature1Trigger::HotkeyOnly;
    }

    ReadField(j, "enable_log", c.enable_log);
    ReadField(j, "request_timeout_ms", c.request_timeout_ms);

    ReadField(j, "hk_translate_mods", c.hk_translate_mods);
    ReadField(j, "hk_translate_vk", c.hk_translate_vk);
    ReadField(j, "hk_capture_mods", c.hk_capture_mods);
    ReadField(j, "hk_capture_vk", c.hk_capture_vk);
    ReadField(j, "hk_overlay_mods", c.hk_overlay_mods);
    ReadField(j, "hk_overlay_vk", c.hk_overlay_vk);

    int mode = static_cast<int>(c.recognition_mode);
    ReadField(j, "recognition_mode", mode);
    c.recognition_mode = (mode == 1) ? RecognitionModeConfig::LocalOcrReserve
                                      : RecognitionModeConfig::AiVisionDirect;

    // ---- 解密 API Key ----
    if (!c.api_key_enc.empty()) {
        if (!CryptoDpapi::Decrypt(c.api_key_enc, c.api_key_plain)) {
            c.api_key_plain.clear();
            decrypt_warning_ = L"已保存的 API Key 无法解密（可能更换了 Windows 账户或电脑），请重新输入。";
            LT_WARN(L"主 API Key 解密失败");
        }
    }
    if (!c.vision_api_key_enc.empty()) {
        if (!CryptoDpapi::Decrypt(c.vision_api_key_enc, c.vision_api_key_plain)) {
            c.vision_api_key_plain.clear();
            LT_WARN(L"视觉 API Key 解密失败");
        }
    }

    cfg_ = std::move(c);
    LT_INFO(L"配置加载成功: " + path_);
    return true;
}

bool ConfigManager::Save() const {
    if (path_.empty()) {
        const_cast<ConfigManager*>(this)->ResolvePaths();
    }

    json j;
    j["_说明"] = "鲤桥翻译 配置文件。api_key_enc 为 Windows DPAPI 加密后的密文，请勿手动修改。";
    j["_配置版本"] = 1;

    j["api_url"]    = Utf8(cfg_.api_url);
    j["text_model"] = Utf8(cfg_.text_model);
    j["api_key_enc"] = cfg_.api_key_enc;

    j["vision_api_url"]         = Utf8(cfg_.vision_api_url);
    j["vision_model"]           = Utf8(cfg_.vision_model);
    j["vision_api_key_enc"]     = cfg_.vision_api_key_enc;
    j["vision_supports_image"]  = cfg_.vision_supports_image;
    j["vision_image_detail"]    = Utf8(cfg_.vision_image_detail);

    j["enable_feature1"]     = cfg_.enable_feature1;
    j["enable_feature2"]     = cfg_.enable_feature2;
    j["enable_feature3"]     = cfg_.enable_feature3;
    j["feature1_trigger"]    = static_cast<int>(cfg_.feature1_trigger);
    // 兼容旧版本读取：由触发方式反推旧字段，保持两种表示一致
    j["enable_float_button"] = cfg_.FloatButtonEnabled();
    j["enable_log"]          = cfg_.enable_log;
    j["request_timeout_ms"]  = cfg_.request_timeout_ms;
    j["recognition_mode"]    = static_cast<int>(cfg_.recognition_mode);

    j["hk_translate_mods"] = cfg_.hk_translate_mods;
    j["hk_translate_vk"]   = cfg_.hk_translate_vk;
    j["hk_capture_mods"]   = cfg_.hk_capture_mods;
    j["hk_capture_vk"]     = cfg_.hk_capture_vk;
    j["hk_overlay_mods"]   = cfg_.hk_overlay_mods;
    j["hk_overlay_vk"]     = cfg_.hk_overlay_vk;

    // 先写临时文件再原子替换，避免写入中断导致配置损坏
    const std::wstring tmpPath = path_ + L".tmp";
    {
        std::ofstream f(tmpPath, std::ios::binary | std::ios::trunc);
        if (!f) {
            LT_ERROR(L"无法写入配置文件: " + tmpPath);
            return false;
        }
        f << j.dump(2);
    }

    if (!::MoveFileExW(tmpPath.c_str(), path_.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        LT_ERROR(L"配置原子替换失败，错误码 " + std::to_wstring(::GetLastError()));
        ::DeleteFileW(tmpPath.c_str());
        return false;
    }

    LT_INFO(L"配置已保存: " + path_);
    return true;
}

} // namespace lt
