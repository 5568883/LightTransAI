#pragma once

#include <string>
#include <vector>

namespace lt {

// 识别模式（对应规范中的 RecognitionMode，配置层用整数存储以便前向兼容）
enum class RecognitionModeConfig : int {
    AiVisionDirect = 0,   // AI 直接识图（第一版实现）
    LocalOcrReserve = 1,  // 本地 OCR（扩展预留，当前仅提示）
};

// 功能一的触发方式。
// 功能一（划词翻译）有两条触发通道：全局快捷键 Alt+E，以及划词后弹出的悬浮按钮。
// 两条通道互相独立，用户可以只要其中一条 —— 例如担心全局钩子影响游戏时，
// 可以切到"仅快捷键"，程序就不会安装低级鼠标钩子。
enum class Feature1Trigger : int {
    Both       = 0,  // 快捷键 + 悬浮按钮（默认）
    HotkeyOnly = 1,  // 仅快捷键（不装鼠标钩子）
    FloatOnly  = 2,  // 仅悬浮按钮
};

// 应用配置。API Key 以 DPAPI 密文（Base64）落盘，内存中保存明文。
struct AppConfig {
    // ---- DeepSeek 主 API（功能一文本翻译，同时作为功能二/三的默认视觉后端）----
    std::wstring api_url     = L"https://api.deepseek.com/chat/completions";
    std::wstring text_model  = L"deepseek-flash";   // deepseek-chat 已于 2026-07-24 下线
    std::string  api_key_plain;                     // 内存明文
    std::string  api_key_enc;                       // 落盘密文（Base64）

    // ---- 视觉模型覆盖项（留空则复用上面的主 API）----
    std::wstring vision_api_url;
    std::wstring vision_model = L"deepseek-flash";
    std::string  vision_api_key_plain;
    std::string  vision_api_key_enc;
    bool         vision_supports_image = true;
    std::wstring vision_image_detail   = L"auto";   // auto / low / high

    // ---- 功能开关 ----
    // 三个功能各自的启用状态。总开关（app_controller 的 kFeature1Available 等）
    // 为编译期常量，这里只表达用户的运行时选择。
    //
    // 功能三（原位置覆盖）尚未实现，默认关闭；即使打开也只会走框选+识别，
    // 不会在屏幕上画出覆盖层。
    bool enable_feature1 = true;    // 功能一：划词翻译
    bool enable_feature2 = true;    // 功能二：框选截图翻译
    bool enable_feature3 = false;   // 功能三：原位置覆盖（待开发）

    // 功能一的触发方式（快捷键 / 悬浮按钮 / 两者）
    Feature1Trigger feature1_trigger = Feature1Trigger::Both;

    RecognitionModeConfig recognition_mode = RecognitionModeConfig::AiVisionDirect;

    // ---- 热键（RegisterHotKey 的 modifiers + vk，见 hotkey_manager）----
    unsigned int hk_translate_mods = 0x0001;  // MOD_ALT
    unsigned int hk_translate_vk   = 'E';
    unsigned int hk_capture_mods   = 0x0001;  // MOD_ALT
    unsigned int hk_capture_vk     = 'Q';
    unsigned int hk_overlay_mods   = 0x0001;  // MOD_ALT
    unsigned int hk_overlay_vk     = 'W';

    // ---- 其它 ----
    bool         enable_log   = true;
    int          request_timeout_ms = 30000;

    // 视觉能力实际生效的取值（未配置覆盖项时回落到主 API）
    std::wstring EffectiveVisionUrl() const {
        return vision_api_url.empty() ? api_url : vision_api_url;
    }
    std::string EffectiveVisionKey() const {
        return vision_api_key_plain.empty() ? api_key_plain : vision_api_key_plain;
    }
    std::wstring EffectiveVisionModel() const {
        return vision_model.empty() ? text_model : vision_model;
    }

    // ---- 由开关派生的"某条通道是否生效"判断 ----
    // 集中在这里，避免各处重复写 && 判断导致行为不一致。
    //
    // 功能一是否可用。触发方式只决定走哪条通道，只要开关打开功能就可用，
    // 因此这里就是开关本身。
    bool Feature1Usable() const { return enable_feature1; }
    // 是否注册功能一的全局快捷键 Alt+E
    bool TranslateHotkeyEnabled() const {
        return enable_feature1 && feature1_trigger != Feature1Trigger::FloatOnly;
    }
    // 是否显示划词悬浮按钮（同时决定要不要装全局鼠标钩子）
    bool FloatButtonEnabled() const {
        return enable_feature1 && feature1_trigger != Feature1Trigger::HotkeyOnly;
    }
    // 是否注册功能二的全局快捷键 Alt+Q
    bool CaptureHotkeyEnabled() const { return enable_feature2; }
    // 是否注册功能三的全局快捷键 Alt+W
    bool OverlayHotkeyEnabled() const { return enable_feature3; }
};

// JSON 配置读写。路径默认 %APPDATA%\LightTransAI\config.json
class ConfigManager {
public:
    static ConfigManager& Instance();

    bool Load();
    bool Save() const;

    AppConfig&       Get() { return cfg_; }
    const AppConfig& Get() const { return cfg_; }

    // 首次运行写出带注释说明的默认配置
    bool EnsureDefaultExists();

    std::wstring ConfigPath() const { return path_; }
    std::wstring DataDir() const { return dir_; }
    // 上次 Load 时若密文无法解密（换了账户/机器），这里给出提示文案
    const std::wstring& LastDecryptWarning() const { return decrypt_warning_; }
    // 若标准配置目录不可写而改用了回退目录，这里给出提示文案
    const std::wstring& PathWarning() const { return path_warning_; }

private:
    ConfigManager() = default;
    ConfigManager(const ConfigManager&) = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;

    void ResolvePaths();

    AppConfig    cfg_;
    std::wstring path_;
    std::wstring dir_;
    std::wstring decrypt_warning_;
    std::wstring path_warning_;
};

} // namespace lt
