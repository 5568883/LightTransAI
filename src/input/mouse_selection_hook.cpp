#include "input/mouse_selection_hook.h"

#include <cmath>

#include "app/app_messages.h"
#include "utils/logger.h"

namespace lt {

MouseSelectionHook* MouseSelectionHook::s_instance_ = nullptr;

bool MouseSelectionHook::Install(HWND hwndNotify) {
    if (hook_ != nullptr) return true;
    if (hwndNotify == nullptr) return false;

    hwndNotify_ = hwndNotify;
    s_instance_ = this;

    // WH_MOUSE_LL 不需要 DLL 注入，回调在本进程主线程执行
    hook_ = ::SetWindowsHookExW(WH_MOUSE_LL, &MouseSelectionHook::HookProc,
                                ::GetModuleHandleW(nullptr), 0);
    if (hook_ == nullptr) {
        const DWORD err = ::GetLastError();
        s_instance_ = nullptr;
        LT_ERROR(L"安装鼠标钩子失败，错误码 " + std::to_wstring(err));
        return false;
    }

    LT_INFO(L"划词检测已启用（零轮询，仅监听左键按下/松开）");
    return true;
}

void MouseSelectionHook::Uninstall() {
    if (hook_ != nullptr) {
        ::UnhookWindowsHookEx(hook_);
        hook_ = nullptr;
        LT_INFO(L"划词检测已停用");
    }
    if (s_instance_ == this) s_instance_ = nullptr;
}

LRESULT CALLBACK MouseSelectionHook::HookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    // 钩子链约定：nCode < 0 时必须直接透传，不得处理
    if (nCode < 0 || s_instance_ == nullptr) {
        return ::CallNextHookEx(nullptr, nCode, wParam, lParam);
    }

    // 只关心左键按下/松开；其余消息（尤其 WM_MOUSEMOVE）立即透传，保证零轮询
    if (wParam == WM_LBUTTONDOWN || wParam == WM_LBUTTONUP) {
        auto* info = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
        if (info != nullptr) {
            // 悬浮按钮可见时，额外把"左键按下"投递给 UI 线程。
            //
            // 为什么必须靠钩子而不是轮询：按钮"点到别处就消失"原先依赖一个
            // 120ms 定时器轮询 GetAsyncKeyState，而一次快速点击（按下+抬起）
            // 完全可能落在两次轮询之间，于是按钮留在屏幕上直到 3 秒超时 ——
            // 用户看到的就是"点了别处按钮还赖着不走"。
            // 钩子能拿到每一个按键事件，是事件驱动、不会漏。
            //
            // 回调内只做一次 new + PostMessage，不做任何可能阻塞的工作，
            // 满足低级钩子 300ms 的时限约束。
            if (wParam == WM_LBUTTONDOWN && s_instance_->activity_notify_.load() &&
                s_instance_->hwndNotify_ != nullptr) {
                auto* pt = new POINT(info->pt);
                if (!::PostMessageW(s_instance_->hwndNotify_, WM_LT_MOUSE_DOWN, 0,
                                    reinterpret_cast<LPARAM>(pt))) {
                    delete pt;
                }
            }

            if (wParam == WM_LBUTTONDOWN) {
                s_instance_->OnButtonDown(info->pt);
            } else {
                s_instance_->OnButtonUp(info->pt);
            }
        }
    } else if (wParam == WM_MOUSEWHEEL) {
        // 滚轮活动：用于撤销划词悬浮按钮（规范要求"滚动滚轮"时自动销毁）。
        // 仅在按钮可见时才投递（由 UI 线程开关），
        // 否则用户每次滚动都会产生一次分配 + 消息，属无谓的常态开销。
        if (s_instance_->activity_notify_.load()) {
            auto* info = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
            if (info != nullptr && s_instance_->hwndNotify_ != nullptr) {
                auto* pt = new POINT(info->pt);
                if (!::PostMessageW(s_instance_->hwndNotify_, WM_LT_MOUSE_ACTIVITY, 0,
                                    reinterpret_cast<LPARAM>(pt))) {
                    delete pt;
                }
            }
        }
    }

    // 绝不吞掉消息，保证鼠标行为对其它程序完全透明
    return ::CallNextHookEx(nullptr, nCode, wParam, lParam);
}

void MouseSelectionHook::OnButtonDown(POINT pt) {
    // 这里只记录状态，成本极低，满足钩子时限要求
    down_pt_     = pt;
    down_time_   = ::GetTickCount();
    button_down_ = true;
}

void MouseSelectionHook::OnButtonUp(POINT pt) {
    if (!button_down_) return;
    button_down_ = false;

    const DWORD now = ::GetTickCount();

    // ---- 拖拽判定 ----
    const int dx = std::abs(pt.x - down_pt_.x);
    const int dy = std::abs(pt.y - down_pt_.y);
    const bool is_drag = (dx > DragThreshold() || dy > DragThreshold());

    // ---- 双击判定 ----
    // 用系统双击时间与双击矩形判定，与用户控制面板设置保持一致
    const DWORD dblTime = ::GetDoubleClickTime();
    const int   dblCx   = ::GetSystemMetrics(SM_CXDOUBLECLK) / 2;
    const int   dblCy   = ::GetSystemMetrics(SM_CYDOUBLECLK) / 2;

    const bool withinTime = (last_click_time_ != 0) && ((now - last_click_time_) <= dblTime);
    const bool withinRect = std::abs(pt.x - last_click_pt_.x) <= dblCx &&
                            std::abs(pt.y - last_click_pt_.y) <= dblCy;
    const bool is_double_click = withinTime && withinRect;

    last_click_time_ = now;
    last_click_pt_   = pt;

    SelectionGesture g{};
    g.pt                = pt;
    g.is_drag           = is_drag;
    g.is_double_click   = is_double_click;
    g.likely_selection  = is_drag || is_double_click;

    if (!g.likely_selection) {
        // 普通单击不产生候选，避免频繁打扰
        return;
    }

    // ---- 只投递消息，重活交给宿主窗口线程 ----
    // 低级钩子回调有 LowLevelHooksTimeout（默认 300ms）限制，超时会被系统静默摘除。
    // 因此这里只做一次 PostMessage，绝不在回调内创建线程或调用 UIA/剪贴板。
    if (hwndNotify_ != nullptr) {
        auto* payload = new SelectionGesture(g);
        if (!::PostMessageW(hwndNotify_, WM_LT_SELECTION_CAND, 0,
                            reinterpret_cast<LPARAM>(payload))) {
            delete payload;  // 投递失败必须自行释放，否则泄漏
        }
    } else if (callback_) {
        // 仅在没有宿主窗口时才走回调（单元测试等场景）
        callback_(g);
    }
}

} // namespace lt
