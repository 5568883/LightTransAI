#pragma once

#include <windows.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "app/app_messages.h"   // TriggerSource / ProbePayload / TestConnPayload
#include "app/settings_window.h"
#include "app/tray_manager.h"
#include "capture/image_encoder.h"
#include "capture/screen_capturer.h"
#include "input/hotkey_manager.h"
#include "input/mouse_selection_hook.h"
#include "recognition/i_recognizer.h"
#include "translation/deepseek_client.h"
#include "ui/float_button_window.h"
#include "ui/main_window.h"
#include "ui/popup_window.h"

namespace lt {

// 应用控制器：协调热键、划词检测、取词、识别与 UI。
//
// 线程模型：
//  - UI 与窗口消息全部在主线程（也是宿主消息循环所在线程）
//  - 翻译请求在独立工作线程执行，完成后 PostMessage 回主线程更新 UI
//  - 低级键盘钩子（Esc 捕获）在主线程安装，回调内只做投递
class AppController {
public:
    AppController() = default;
    ~AppController();

    AppController(const AppController&) = delete;
    AppController& operator=(const AppController&) = delete;

    bool Initialize(HINSTANCE hinst);
    void Shutdown();

    // 宿主窗口消息分发（由 main.cpp 的窗口过程调用）
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    HWND HostWindow() const { return hwnd_; }

private:
    // ---- 初始化步骤 ----
    bool CreateHostWindow(HINSTANCE hinst);
    // 按当前配置注册热键；返回注册失败的中文原因列表（空表示全部成功）
    std::vector<std::wstring> RegisterHotkeys();
    // 设置窗口保存成功后调用：重新注册热键、按需装拆鼠标钩子与悬浮按钮。
    // 返回非空表示有需要提示给用户的警告。
    std::wstring ApplyConfigAfterSave();
    // 按配置创建/销毁悬浮按钮对象（触发方式可变，故需双向）
    void EnsureFloatButton();
    void InstallMouseHook();
    void InstallEscHook();
    void UninstallEscHook();

    // ---- 功能一流程 ----
    void OnSelectionCandidate(const SelectionGesture& gesture);
    void OnFloatButtonClicked(POINT screen_pt);
    void OnHotkeyPressed(int id);
    void StartTranslation(TriggerSource source, const std::wstring& preset_text);
    void OnTranslationFinished(TranslationResult* result);

    // ---- 测试连接 ----
    void OnTestConnectionRequested(bool use_vision);
    void OnTestConnectionFinished(TestConnPayload* payload);

    // ---- 功能二：框选截图翻译 ----
    // require_boxes=false 走功能二（弹窗显示译文）
    // require_boxes=true  走功能三（原位置覆盖）
    void StartCaptureFlow(bool require_boxes);
    void StartVisionRequest(bool require_boxes);
    void OnVisionFinished(VisionPayload* payload);

    // ---- 悬浮按钮生命周期 ----
    void DismissFloatButton();
    // 没有任何需要键盘响应的窗口时卸载钩子
    void ReleaseEscHookIfIdle();

    // ---- Esc 低级键盘钩子 ----
    static LRESULT CALLBACK EscHookProc(int nCode, WPARAM wp, LPARAM lp);

    // ---- 托盘 ----
    void OnTrayCommand(UINT cmd);
    void ShowMemoryStatus();
    // 托盘不可用时的可见提示（延后到消息循环启动后执行）
    void ShowTrayFailureNotice();

    // ---- 主窗口（任务栏可见入口）----
    // 主窗口关闭请求：托盘可用则收起，不可用则退出
    void OnMainWindowCloseRequest();
    // 同步主窗口的开关与状态显示
    void SyncMainWindow();

    // ---- 工作线程 ----
    void StartWorker(std::thread& t, std::function<void()> fn);

    static AppController* s_instance_;

    HWND hwnd_ = nullptr;

    std::unique_ptr<TrayManager>        tray_;
    std::unique_ptr<SettingsWindow>     settings_;
    std::unique_ptr<HotkeyManager>      hotkeys_;
    std::unique_ptr<MouseSelectionHook> mouse_hook_;
    std::unique_ptr<FloatButtonWindow>  float_btn_;
    std::unique_ptr<PopupWindow>        popup_;
    std::unique_ptr<MainWindow>         main_win_;
    std::unique_ptr<ScreenCapturer>     capturer_;

    // 取消令牌：与正在进行的请求共享
    std::shared_ptr<std::atomic<bool>> cancel_token_;
    std::atomic<bool>                  busy_{false};
    // 防止划词探测线程堆积
    std::atomic<bool>                  probe_in_flight_{false};

    std::thread translation_thread_;
    std::thread test_thread_;
    std::thread vision_thread_;

    // 当前视觉任务是否为"原位置覆盖"模式
    bool vision_require_boxes_ = false;
    // 当前视觉任务的选区（用于覆盖层定位）
    RECT vision_region_{};
    // 选区原始 BGRA 位图（覆盖层采样背景色必需；JPEG 有损不能用于取色）
    BitmapBuffer vision_cropped_;
    // 选区所在显示器的 DPI 缩放
    float vision_dpi_scale_ = 1.0f;

    HHOOK esc_hook_ = nullptr;

    // 托盘是否创建失败（用于给出可见提示）
    bool tray_failed_ = false;

    // 待翻译文本（悬浮按钮点击时从 UIA 现场获取，或热键触发时获取）
    std::wstring pending_text_;
    TriggerSource pending_source_ = TriggerSource::Hotkey;

    bool initialized_ = false;
};

} // namespace lt
