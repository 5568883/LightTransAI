#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include <functional>
#include <string>

namespace lt {

// 翻译结果弹窗。
//
// 特性：
//  - 无系统标题栏，圆角现代风格（Win11 DWMWCP_ROUND）
//  - 先显示加载动画 + "DeepSeek 翻译中……"，完成后只显示中文译文
//  - 「复制译文」与「关闭」按钮，Esc 关闭
//  - 内容过长支持鼠标滚轮滚动
//  - 位置做屏幕边缘防溢出计算
//
// 注意：本窗口带 WS_EX_NOACTIVATE，不会抢走原软件焦点；
// 因此 Esc 键消息不会自动送达，需要由 app_controller 挂低级键盘钩子
// 或在按键时转发（见 app_controller 的处理）。
class PopupWindow {
public:
    using CloseCallback = std::function<void()>;

    PopupWindow() = default;
    ~PopupWindow() { Destroy(); }

    PopupWindow(const PopupWindow&) = delete;
    PopupWindow& operator=(const PopupWindow&) = delete;

    // 在 anchor 附近弹出并进入"加载中"状态
    bool ShowLoading(POINT anchor);

    // 展示最终译文（成功）
    void ShowResult(const std::wstring& text);
    // 展示错误提示
    void ShowError(const std::wstring& message);

    void Destroy();
    bool IsVisible() const { return hwnd_ != nullptr; }
    HWND Handle() const { return hwnd_; }

    void SetOnClose(CloseCallback cb) { on_close_ = std::move(cb); }

    // 供外部（低级键盘钩子）调用：响应 Esc
    void HandleEscape();

    // 请求关闭（会触发 on_close_）
    void RequestClose();

    static bool EnsureClassRegistered();

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);

    void RecalcSize();
    void Paint();
    void ReleaseRenderTarget();
    void ReleaseTextFormat();

    RECT CloseButtonRect() const;
    RECT CopyButtonRect() const;

    enum class State { Loading, Result, Error };

    HWND                   hwnd_ = nullptr;
    ID2D1HwndRenderTarget* rt_   = nullptr;
    IDWriteTextFormat*     fmt_  = nullptr;
    IDWriteTextLayout*     layout_ = nullptr;

    State        state_   = State::Loading;
    std::wstring text_;          // 译文或错误信息
    float        scroll_y_ = 0.0f;
    float        content_h_ = 0.0f;

    int   anim_frame_ = 0;
    POINT anchor_{};

    bool          copy_hover_ = false;
    bool          close_hover_ = false;
    bool          copy_done_  = false;   // 复制后短暂显示"已复制"

    CloseCallback on_close_;
};

} // namespace lt
