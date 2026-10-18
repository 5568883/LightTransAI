#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include <functional>

#include "ui/ui_common.h"

namespace lt {

// 极简胶囊形悬浮按钮。
//
// 关键扩展样式（缺一不可）：
//   WS_EX_NOACTIVATE —— 点击时不抢走原软件焦点，保证选中的蓝色高亮文字不消失
//   WS_EX_TOPMOST    —— 始终浮于其它窗口之上
//   WS_EX_TOOLWINDOW —— 不出现在 Alt+Tab 与任务栏
//
// 生命周期：Show 后由调用方通过定时器在 3 秒后或用户交互时 Destroy。
class FloatButtonWindow {
public:
    using ClickCallback   = std::function<void(POINT screen_pt)>;
    using ExpiredCallback = std::function<void()>;

    FloatButtonWindow() = default;
    ~FloatButtonWindow() { Destroy(); }

    FloatButtonWindow(const FloatButtonWindow&) = delete;
    FloatButtonWindow& operator=(const FloatButtonWindow&) = delete;

    // 在 anchor 附近显示。anchor 通常是划词结束时鼠标所在位置。
    bool Show(POINT anchor);

    void Destroy();
    bool IsVisible() const { return hwnd_ != nullptr; }
    HWND Handle() const { return hwnd_; }

    void SetOnClick(ClickCallback cb) { on_click_ = std::move(cb); }
    void SetOnExpired(ExpiredCallback cb) { on_expired_ = std::move(cb); }

    // 命中测试：判断屏幕坐标是否落在按钮上
    bool HitTest(POINT screen_pt) const;

    static bool EnsureClassRegistered();

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);

    void Paint();
    void ReleaseRenderTarget();

    HWND                   hwnd_ = nullptr;
    // 逐像素 alpha 需要分层画布（ID2D1HwndRenderTarget 做不到）
    ui::LayeredCanvas      canvas_;
    IDWriteTextFormat*     fmt_  = nullptr;
    bool                   hover_ = false;
    // 按钮是否被翻到了锚点左侧（选区贴近屏幕右缘时发生）。
    // 绘制时据此镜像投影方向，否则投影会指向文字一侧，看着像凹陷。
    bool                   flipped_ = false;
    // 按钮在屏幕上的位置（UpdateLayeredWindow 需要）
    POINT                  pos_{};

    ClickCallback   on_click_;
    ExpiredCallback on_expired_;
};

} // namespace lt
