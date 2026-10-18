#pragma once

#include <windows.h>

#include <atomic>
#include <functional>

namespace lt {

// 划词手势候选信息
struct SelectionGesture {
    POINT pt{};                    // 鼠标松开时的屏幕坐标
    bool  is_drag        = false;  // 拖拽距离 > 15px
    bool  is_double_click = false; // 快速双击
    bool  likely_selection = false; // is_drag || is_double_click
};

// 基于 WH_MOUSE_LL 的零轮询划词检测。
//
// 设计要点（严格遵守低级钩子的时限约束）：
//  - 只处理 WM_LBUTTONDOWN / WM_LBUTTONUP，绝不处理 WM_MOUSEMOVE（规范要求零轮询）。
//  - 钩子回调内只做坐标与时间记录，然后 PostMessage 投递给宿主窗口，
//    绝不在回调里做 UIA 调用、剪贴板操作或任何可能阻塞的工作。
//    低级钩子回调超过 LowLevelHooksTimeout（默认 300ms）会被 Windows 静默摘除。
//  - 真正的"是否弹出按钮"判定（工字光标校验 + UIA 选区探测）在宿主窗口线程完成。
class MouseSelectionHook {
public:
    using Callback = std::function<void(const SelectionGesture&)>;

    MouseSelectionHook() = default;
    ~MouseSelectionHook() { Uninstall(); }

    MouseSelectionHook(const MouseSelectionHook&) = delete;
    MouseSelectionHook& operator=(const MouseSelectionHook&) = delete;

    // hwndNotify: 接收 WM_LT_SELECTION_CAND 的窗口
    bool Install(HWND hwndNotify);
    void Uninstall();
    bool IsInstalled() const { return hook_ != nullptr; }

    void SetCallback(Callback cb) { callback_ = std::move(cb); }

    // 是否投递滚轮活动通知。
    // 仅在划词悬浮按钮可见时开启，避免用户每次滚动滚轮都做一次
    // 内存分配 + PostMessage（那是无谓的常态开销）。
    void SetActivityNotifyEnabled(bool enabled) {
        activity_notify_.store(enabled);
    }

    // 拖拽判定阈值（像素）
    static int DragThreshold() { return 15; }

private:
    static LRESULT CALLBACK HookProc(int nCode, WPARAM wParam, LPARAM lParam);
    void OnButtonDown(POINT pt);
    void OnButtonUp(POINT pt);

    static MouseSelectionHook* s_instance_;

    HHOOK    hook_       = nullptr;
    HWND     hwndNotify_ = nullptr;
    Callback callback_;

    // 由 UI 线程设置、钩子回调读取
    std::atomic<bool> activity_notify_{false};

    POINT down_pt_{};
    DWORD down_time_ = 0;
    bool  button_down_ = false;

    DWORD last_click_time_ = 0;
    POINT last_click_pt_{};
};

} // namespace lt
