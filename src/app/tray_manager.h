#pragma once

#include <windows.h>
#include <shellapi.h>

#include <functional>
#include <string>

namespace lt {

// 托盘管理器。常驻的唯一 UI 元素。
// 菜单项全部为简体中文，勾选状态与配置同步。
class TrayManager {
public:
    using CommandCallback = std::function<void(UINT cmd)>;

    TrayManager() = default;
    ~TrayManager() { Remove(); }

    TrayManager(const TrayManager&) = delete;
    TrayManager& operator=(const TrayManager&) = delete;

    // 创建托盘图标。hwnd 接收 WM_LT_TRAY 回调。
    bool Install(HWND hwnd, const std::wstring& tooltip);

    void Remove();

    void SetOnCommand(CommandCallback cb) { on_command_ = std::move(cb); }

    // 更新菜单勾选状态
    void SetFloatButtonEnabled(bool enabled) { float_button_enabled_ = enabled; }
    void SetTooltip(const std::wstring& tip);

    // 处理托盘回调（由宿主窗口在收到 WM_LT_TRAY 时调用）
    void OnTrayMessage(WPARAM wp, LPARAM lp);

    // 弹出右键菜单
    void ShowContextMenu();

    bool IsInstalled() const { return installed_; }

private:
    HWND                    hwnd_ = nullptr;
    bool                    installed_ = false;
    bool                    float_button_enabled_ = true;
    NOTIFYICONDATAW         nid_{};
    CommandCallback         on_command_;
};

} // namespace lt
