#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace lt {

struct HotkeySpec {
    int          id      = 0;
    unsigned int mods    = 0;   // MOD_ALT / MOD_CONTROL / MOD_SHIFT / MOD_WIN
    unsigned int vk      = 0;   // 虚拟键码
    std::wstring name;          // 简体中文名称，用于提示
};

// 基于 RegisterHotKey 的全局热键管理器。
//
// 注意：RegisterHotKey 注册的组合是全局优先的，会抢占目标程序自身的快捷键。
// 例如注册 Alt+E 会导致许多软件的"编辑(E)"菜单失效。默认值已按此权衡选取，
// 并在注册失败时向用户给出明确的中文提示。
class HotkeyManager {
public:
    using Callback = std::function<void(int id)>;

    explicit HotkeyManager(HWND hwnd) : hwnd_(hwnd) {}
    ~HotkeyManager() { UnregisterAll(); }

    void SetCallback(Callback cb) { callback_ = std::move(cb); }

    // 注册一个热键；失败返回 false 并填充 error（简体中文）
    bool Register(const HotkeySpec& spec, std::wstring& error);

    // 批量注册；返回失败的名称列表
    std::vector<std::wstring> RegisterAll(const std::vector<HotkeySpec>& specs);

    void Unregister(int id);
    void UnregisterAll();

    // 供窗口过程转发 WM_HOTKEY
    void OnHotkey(int id);

    // 把 mods/vk 格式化成 "Alt+E" 这样的可读文本
    static std::wstring Format(const HotkeySpec& spec);
    static std::wstring Format(unsigned int mods, unsigned int vk);

    // Parse 的反向操作：解析用户在设置窗口里输入的 "Alt+E" 这类文本。
    // 成功返回 true 并填写 mods 与 vk（mods 不含 MOD_NOREPEAT）。
    // 失败返回 false 并在 error 中给出简体中文原因。
    //
    // 出于安全考虑，纯字母/数字（无修饰键）会被拒绝：注册单个按键为全局热键
    // 会抢走所有程序里该键的输入。功能键（F1~F24）允许单独使用。
    static bool Parse(const std::wstring& text, unsigned int& mods,
                      unsigned int& vk, std::wstring& error);

    // 检查组合是否与系统保留键冲突（如 Alt+Tab、Alt+F4、Ctrl+Alt+Del）
    static bool IsSystemReserved(unsigned int mods, unsigned int vk);

private:
    HWND                  hwnd_ = nullptr;
    Callback              callback_;
    std::vector<HotkeySpec> registered_;
};

} // namespace lt
