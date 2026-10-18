#pragma once

#include <windows.h>

#include <functional>
#include <string>

namespace lt {

// 设置窗口。
//
// 严格遵守"零常驻界面"原则：打开时 CreateWindowEx，关闭时立即 DestroyWindow
// 并释放全部控件与字体资源，让内存回落到闲置水平。
class SettingsWindow {
public:
    // 测试连接结果回调：ok 为是否成功，message 为简体中文提示
    using TestResultCallback = std::function<void(bool ok, const std::wstring& message)>;
    // 保存成功后的回调：由 app_controller 重新应用配置（重新注册热键等）。
    // 返回非空的简体中文警告表示"已保存，但应用时有问题"，由设置窗口展示给用户；
    // 返回空串表示一切正常。
    using SavedCallback = std::function<std::wstring()>;

    SettingsWindow() = default;
    ~SettingsWindow() { Destroy(); }

    SettingsWindow(const SettingsWindow&) = delete;
    SettingsWindow& operator=(const SettingsWindow&) = delete;

    bool Show(HWND owner);
    void Destroy();
    bool IsVisible() const { return hwnd_ != nullptr; }
    HWND Handle() const { return hwnd_; }

    void SetOnSaved(SavedCallback cb) { on_saved_ = std::move(cb); }

    // 由 app_controller 在测试连接完成后调用，更新状态栏
    void ReportTestResult(bool ok, const std::wstring& message);

    static bool EnsureClassRegistered();

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);

    void CreateControls();
    void LoadFromConfig();
    bool SaveToConfig();
    void SetStatus(const std::wstring& text, bool is_error);
    void OnCommand(int id);

    // CreateControls 按实际内容算出的客户区高度（含状态栏）。
    // 窗口高度据此设定，避免控件落到客户区之外导致用户看不到。
    int content_height_ = 0;

    HWND hwnd_       = nullptr;
    HWND hwndOwner_  = nullptr;

    // 控件句柄
    HWND hUrl_          = nullptr;
    HWND hKey_          = nullptr;
    HWND hModel_        = nullptr;
    HWND hVisionUrl_    = nullptr;
    HWND hVisionKey_    = nullptr;
    HWND hVisionModel_  = nullptr;
    HWND hVisionDetail_ = nullptr;
    HWND hVisionEnable_ = nullptr;

    // ---- 功能开关 ----
    HWND hEnableF1_     = nullptr;   // 功能一：划词翻译
    HWND hEnableF2_     = nullptr;   // 功能二：框选截图翻译
    HWND hEnableF3_     = nullptr;   // 功能三：原位置覆盖（待开发）
    HWND hF1Trigger_    = nullptr;   // 功能一触发方式：快捷键 / 悬浮按钮 / 两者
    HWND hRecogMode_    = nullptr;
    HWND hHkTranslate_  = nullptr;
    HWND hHkCapture_    = nullptr;
    HWND hHkOverlay_    = nullptr;

    HWND hTestText_     = nullptr;
    HWND hTestVision_   = nullptr;
    HWND hSave_         = nullptr;
    HWND hStatus_       = nullptr;
    HWND hNote_         = nullptr;

    HFONT fontNormal_ = nullptr;
    HFONT fontBold_   = nullptr;

    SavedCallback on_saved_;
    bool          test_in_progress_ = false;
};

} // namespace lt
