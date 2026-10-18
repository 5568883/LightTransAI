#pragma once

#include <windows.h>

#include <functional>
#include <string>

namespace lt {

class MainWindow {
public:
    // 命令回调：参数为 CMD_TRAY_* 系列常量，由 app_controller 统一处理
    using CommandCallback = std::function<void(UINT cmd)>;
    // 关闭请求回调：由 app_controller 决定是"收起"还是"退出"
    using CloseCallback = std::function<void()>;

    MainWindow() = default;
    ~MainWindow() { Destroy(); }

    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    // 创建并显示（已存在则前置激活）
    bool Show();
    // 隐藏但保留窗口（收到托盘时使用）
    void Hide();
    // 彻底销毁并释放控件与字体
    void Destroy();

    bool IsVisible() const;
    HWND Handle() const { return hwnd_; }

    void SetOnCommand(CommandCallback cb) { on_command_ = std::move(cb); }
    void SetOnCloseRequest(CloseCallback cb) { on_close_request_ = std::move(cb); }

    // ---- 状态同步 ----
    void SetTrayAvailable(bool available) { tray_available_ = available; }
    void SetFloatButtonEnabled(bool enabled);
    void SetStatus(const std::wstring& text);
    // 刷新内存显示（不轮询：仅在显示、激活或用户点击时更新）
    void RefreshMemoryText();

    static bool EnsureClassRegistered();

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);

    void CreateControls();
    void Layout();
    void OnCommand(int id);

    HWND hwnd_ = nullptr;

    HWND hTitle_    = nullptr;
    HWND hSubtitle_ = nullptr;
    HWND hFloat_    = nullptr;   // 划词悬浮开关
    HWND hSettings_ = nullptr;   // 打开设置
    HWND hFeature2_ = nullptr;   // 功能二
    HWND hFeature3_ = nullptr;   // 功能三
    HWND hMem_      = nullptr;   // 内存状态
    HWND hStatus_   = nullptr;   // 状态提示
    HWND hQuit_     = nullptr;   // 退出

    HFONT fontNormal_ = nullptr;
    HFONT fontBold_   = nullptr;
    HFONT fontSmall_  = nullptr;

    CommandCallback on_command_;
    CloseCallback   on_close_request_;

    bool tray_available_ = false;
    bool float_enabled_  = true;
};

} // namespace lt
