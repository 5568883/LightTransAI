#include "ui/popup_window.h"

#include <d2d1helper.h>
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM

#include "app/app_messages.h"
#include "ui/ui_common.h"
#include "utils/logger.h"
#include "utils/string_utils.h"

namespace lt {

namespace {

// 布局常量（DIP）
//
// 垂直方向的空间分配（自下而上、自上而下各占一部分，必须不重叠）：
//   ┌─────────────────────────┐
//   │ kPadY      上边距 14     │
//   │ kHeaderH   标题行 30     │
//   │ ─── 内容区（译文）───    │  ← contentTop    = kPadY + kHeaderH
//   │ kContentGap 间隔 8       │
//   │ kBtnH      按钮高 28     │
//   │ kPadY      下边距 14     │
//   └─────────────────────────┘  ← contentBottom = h - (kContentGap + kBtnH + kPadY)
//
// 曾经的错误：内容区底部按 (kPadY*2 + kBtnH + 8) 预留，把上边距也算了一遍，
// 导致内容区高度被压成负数（kMinHeightDip=96 时：top=44 > bottom=32），
// 译文被完全裁剪，弹窗只剩标题和按钮 —— 表现为"出错了但正文空白"。
constexpr float kPadX         = 16.0f;
constexpr float kPadY         = 14.0f;
constexpr float kBtnH         = 28.0f;
constexpr float kBtnW         = 88.0f;
constexpr float kCloseBtnSize = 26.0f;
constexpr float kRadius       = 10.0f;
constexpr float kHeaderH      = 30.0f;
constexpr float kContentGap   = 8.0f;   // 内容区与按钮之间的间隔

// 内容区下沿的预留量（不含上边距，避免与 contentTop 重复计算）
constexpr float kBottomReserve = kContentGap + kBtnH + kPadY;   // 8 + 28 + 14 = 50

constexpr float kMinWidthDip  = 280.0f;
constexpr float kMaxWidthDip  = 520.0f;
// 最小高度必须容纳：上边距 + 标题 + 至少一行译文 + 间隔 + 按钮 + 下边距
// = 14 + 30 + (14*1.45≈20) + 8 + 28 + 14 ≈ 114，取 124 留一点余量
constexpr float kMinHeightDip = 124.0f;
constexpr float kMaxHeightDip = 420.0f;

constexpr float kFontSizeDip  = 14.0f;
constexpr float kLineSpacing  = 1.45f;

// 复制完成提示的恢复定时器（与 app_messages.h 中的定时器 ID 不冲突）
constexpr UINT_PTR kCopyDoneTimer = 0x1100;

// 配色
const D2D1_COLOR_F kBg         = D2D1::ColorF(0.99f, 0.99f, 1.00f, 1.0f);
const D2D1_COLOR_F kBorder     = D2D1::ColorF(0.85f, 0.86f, 0.89f, 1.0f);
const D2D1_COLOR_F kText       = D2D1::ColorF(0.12f, 0.13f, 0.16f, 1.0f);
const D2D1_COLOR_F kTextDim    = D2D1::ColorF(0.45f, 0.47f, 0.52f, 1.0f);
const D2D1_COLOR_F kErrText    = D2D1::ColorF(0.78f, 0.20f, 0.18f, 1.0f);
const D2D1_COLOR_F kAccent     = D2D1::ColorF(0.20f, 0.45f, 0.95f, 1.0f);
const D2D1_COLOR_F kBtnBg      = D2D1::ColorF(0.94f, 0.95f, 0.97f, 1.0f);
const D2D1_COLOR_F kBtnBgHover = D2D1::ColorF(0.88f, 0.90f, 0.94f, 1.0f);
const D2D1_COLOR_F kCloseHover = D2D1::ColorF(0.93f, 0.35f, 0.32f, 1.0f);

// 复制到剪贴板
bool CopyToClipboard(HWND owner, const std::wstring& text) {
    if (text.empty()) return false;

    bool opened = false;
    for (int i = 0; i < 10; ++i) {
        if (::OpenClipboard(owner)) { opened = true; break; }
        ::Sleep(10);
    }
    if (!opened) return false;

    ::EmptyClipboard();

    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    bool ok = false;
    if (HGLOBAL h = ::GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void* p = ::GlobalLock(h)) {
            memcpy(p, text.c_str(), bytes);
            ::GlobalUnlock(h);
            if (::SetClipboardData(CF_UNICODETEXT, h) != nullptr) {
                ok = true;
            } else {
                ::GlobalFree(h);
            }
        } else {
            ::GlobalFree(h);
        }
    }

    ::CloseClipboard();
    return ok;
}

} // namespace

bool PopupWindow::EnsureClassRegistered() {
    static bool registered = false;
    if (registered) return true;

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc   = &PopupWindow::WndProc;
    wc.hInstance     = ::GetModuleHandleW(nullptr);
    wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kPopupWindowClass;
    wc.hbrBackground = nullptr;

    if (::RegisterClassExW(&wc) == 0) {
        const DWORD err = ::GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            LT_ERROR(L"注册翻译弹窗类失败，错误码 " + std::to_wstring(err));
            return false;
        }
    }
    registered = true;
    return true;
}

bool PopupWindow::ShowLoading(POINT anchor) {
    if (!EnsureClassRegistered()) return false;

    if (hwnd_ != nullptr) Destroy();

    anchor_       = anchor;
    state_        = State::Loading;
    text_.clear();
    scroll_y_     = 0.0f;
    content_h_    = 0.0f;
    anim_frame_   = 0;
    copy_done_    = false;
    copy_hover_   = false;
    close_hover_  = false;

    // 先按加载态的最小尺寸创建，内容确定后再调整
    const UINT dpi = ::GetDpiForWindow(::GetDesktopWindow());
    const float scale = (dpi >= 48) ? static_cast<float>(dpi) / 96.0f : 1.0f;
    const int w = static_cast<int>(kMinWidthDip * scale + 0.5f);
    const int h = static_cast<int>(kMinHeightDip * scale + 0.5f);

    const POINT pos = ui::ClampToWorkArea(anchor.x, anchor.y, w, h);

    // WS_EX_NOACTIVATE：不抢焦点，避免原软件选区高亮消失
    const DWORD exStyle = WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TOOLWINDOW;
    hwnd_ = ::CreateWindowExW(exStyle, kPopupWindowClass, L"鲤桥翻译", WS_POPUP,
                              pos.x, pos.y, w, h,
                              nullptr, nullptr, ::GetModuleHandleW(nullptr), this);
    if (hwnd_ == nullptr) {
        LT_ERROR(L"创建翻译弹窗失败，错误码 " + std::to_wstring(::GetLastError()));
        return false;
    }

    ui::ApplyRoundedRegion(hwnd_, static_cast<int>(kRadius * scale));
    ui::ApplyDwmRoundCorners(hwnd_);

    ::ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    ::SetWindowPos(hwnd_, HWND_TOPMOST, pos.x, pos.y, w, h,
                   SWP_NOACTIVATE | SWP_SHOWWINDOW);

    // 启动加载动画定时器（约 12 fps，开销极低）
    ::SetTimer(hwnd_, TIMER_POPUP_ANIM, 80, nullptr);

    return true;
}

void PopupWindow::ShowResult(const std::wstring& text) {
    if (hwnd_ == nullptr) return;
    state_ = State::Result;
    text_  = text;
    scroll_y_ = 0.0f;
    if (::IsWindow(hwnd_)) {
        ::KillTimer(hwnd_, TIMER_POPUP_ANIM);
        RecalcSize();
        ::InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void PopupWindow::ShowError(const std::wstring& message) {
    if (hwnd_ == nullptr) return;
    state_ = State::Error;
    text_  = message;
    scroll_y_ = 0.0f;
    if (::IsWindow(hwnd_)) {
        ::KillTimer(hwnd_, TIMER_POPUP_ANIM);
        RecalcSize();
        ::InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void PopupWindow::RecalcSize() {
    if (hwnd_ == nullptr) return;

    const float scale = ui::DpiScale(hwnd_);

    // 依据文本量估算合适的窗口尺寸
    const size_t len = text_.size();
    float widthDip = kMinWidthDip + static_cast<float>(len) * 1.6f;
    if (widthDip > kMaxWidthDip) widthDip = kMaxWidthDip;

    const float textWidthDip = widthDip - kPadX * 2.0f;

    // 估算行数：按每行可容纳字符数粗略计算（中文字符宽度约为字号）
    const float charsPerLine = (textWidthDip > 1.0f)
                                   ? (textWidthDip / (kFontSizeDip * 0.95f))
                                   : 20.0f;
    float lines = 1.0f;
    if (charsPerLine > 1.0f && len > 0) {
        // 同时考虑显式换行
        lines = 0.0f;
        size_t current = 0;
        for (const auto& line : str::SplitLines(text_)) {
            const size_t n = line.size();
            lines += (n == 0) ? 1.0f
                              : static_cast<float>((n + static_cast<size_t>(charsPerLine) - 1) /
                                                   static_cast<size_t>(charsPerLine));
            current += n;
        }
        if (lines < 1.0f) lines = 1.0f;
        (void)current;
    }

    float textHeightDip = lines * kFontSizeDip * kLineSpacing;
    content_h_ = textHeightDip * scale;

    // 窗口高度 = 上边距 + 标题 + 译文高度 + 底部预留（间隔 + 按钮 + 下边距）
    float heightDip = kPadY + kHeaderH + textHeightDip + kBottomReserve;
    if (heightDip < kMinHeightDip) heightDip = kMinHeightDip;
    if (heightDip > kMaxHeightDip) heightDip = kMaxHeightDip;

    const int w = static_cast<int>(widthDip * scale + 0.5f);
    const int h = static_cast<int>(heightDip * scale + 0.5f);

    RECT rc{};
    ::GetWindowRect(hwnd_, &rc);
    const POINT pos = ui::ClampToWorkArea(rc.left, rc.top, w, h);

    ::SetWindowPos(hwnd_, HWND_TOPMOST, pos.x, pos.y, w, h,
                   SWP_NOACTIVATE | SWP_SHOWWINDOW);

    ui::ApplyRoundedRegion(hwnd_, static_cast<int>(kRadius * scale));

    // 尺寸变化后渲染目标需要重建
    ReleaseRenderTarget();
    ReleaseTextFormat();
}

void PopupWindow::ReleaseRenderTarget() {
    if (layout_ != nullptr) { layout_->Release(); layout_ = nullptr; }
    if (rt_ != nullptr) { rt_->Release(); rt_ = nullptr; }
}

void PopupWindow::ReleaseTextFormat() {
    if (fmt_ != nullptr) { fmt_->Release(); fmt_ = nullptr; }
}

void PopupWindow::Destroy() {
    if (hwnd_ != nullptr) {
        ::KillTimer(hwnd_, TIMER_POPUP_ANIM);
    }
    ReleaseRenderTarget();
    ReleaseTextFormat();
    if (hwnd_ != nullptr) {
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    text_.clear();
}

void PopupWindow::RequestClose() {
    if (hwnd_ == nullptr) return;
    Destroy();
    if (on_close_) on_close_();
}

void PopupWindow::HandleEscape() {
    RequestClose();
}

LRESULT CALLBACK PopupWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        auto* self = reinterpret_cast<PopupWindow*>(cs->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                            reinterpret_cast<LONG_PTR>(self));
        // 创建期间即绑定句柄，否则默认分支会把消息交给
        // DefWindowProcW(nullptr, ...)（详见 main_window.cpp 的说明）。
        if (self != nullptr) self->hwnd_ = hwnd;
        return ::DefWindowProcW(hwnd, msg, wp, lp);
    }

    auto* self = reinterpret_cast<PopupWindow*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self == nullptr) {
        return ::DefWindowProcW(hwnd, msg, wp, lp);
    }
    return self->HandleMessage(msg, wp, lp);
}

RECT PopupWindow::CloseButtonRect() const {
    if (hwnd_ == nullptr) return RECT{};
    RECT rc{};
    ::GetClientRect(hwnd_, &rc);
    const float scale = ui::DpiScale(hwnd_);
    const int size = static_cast<int>(kCloseBtnSize * scale);
    const int margin = static_cast<int>(8 * scale);
    RECT r{};
    r.right  = rc.right - margin;
    r.left   = r.right - size;
    r.top    = margin;
    r.bottom = r.top + size;
    return r;
}

RECT PopupWindow::CopyButtonRect() const {
    if (hwnd_ == nullptr) return RECT{};
    RECT rc{};
    ::GetClientRect(hwnd_, &rc);
    const float scale = ui::DpiScale(hwnd_);
    const int w = static_cast<int>(kBtnW * scale);
    const int h = static_cast<int>(kBtnH * scale);
    const int padX = static_cast<int>(kPadX * scale);
    const int padY = static_cast<int>(kPadY * scale);

    RECT r{};
    r.left   = padX;
    r.right  = r.left + w;
    r.bottom = rc.bottom - padY;
    r.top    = r.bottom - h;
    return r;
}

LRESULT PopupWindow::HandleMessage(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            ::BeginPaint(hwnd_, &ps);
            Paint();
            ::EndPaint(hwnd_, &ps);
            return 0;
        }

        case WM_TIMER: {
            if (wp == TIMER_POPUP_ANIM && state_ == State::Loading) {
                anim_frame_ = (anim_frame_ + 1) % 12;
                ::InvalidateRect(hwnd_, nullptr, FALSE);
            } else if (wp == kCopyDoneTimer) {
                ::KillTimer(hwnd_, kCopyDoneTimer);
                copy_done_ = false;
                ::InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }

        case WM_MOUSEMOVE: {
            const POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            const RECT cr = CloseButtonRect();
            const RECT pr = CopyButtonRect();

            const bool overClose = ::PtInRect(&cr, pt) != FALSE;
            const bool overCopy  = (state_ != State::Loading) &&
                                   (::PtInRect(&pr, pt) != FALSE);

            if (overClose != close_hover_ || overCopy != copy_hover_) {
                close_hover_ = overClose;
                copy_hover_  = overCopy;

                TRACKMOUSEEVENT tme{};
                tme.cbSize    = sizeof(tme);
                tme.dwFlags   = TME_LEAVE;
                tme.hwndTrack = hwnd_;
                ::TrackMouseEvent(&tme);

                ::InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }

        case WM_MOUSELEAVE: {
            if (copy_hover_ || close_hover_) {
                copy_hover_  = false;
                close_hover_ = false;
                ::InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            const POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            const RECT cr = CloseButtonRect();
            const RECT pr = CopyButtonRect();

            if (::PtInRect(&cr, pt)) {
                RequestClose();
                return 0;
            }
            if (state_ != State::Loading && ::PtInRect(&pr, pt)) {
                if (CopyToClipboard(hwnd_, text_)) {
                    copy_done_ = true;
                    ::InvalidateRect(hwnd_, nullptr, FALSE);
                    // 1.5 秒后恢复按钮文字
                    ::SetTimer(hwnd_, kCopyDoneTimer, 1500, nullptr);
                    LT_INFO(L"译文已复制到剪贴板");
                }
                return 0;
            }
            return 0;
        }

        case WM_MOUSEWHEEL: {
            if (state_ == State::Loading) return 0;
            RECT rc{};
            ::GetClientRect(hwnd_, &rc);
            // 可视高度 = 客户区高度 - 上边距 - 标题 - 底部预留
            const float scaleW = ui::DpiScale(hwnd_);
            const float viewH = static_cast<float>(rc.bottom - rc.top) -
                                (kPadY + kHeaderH + kBottomReserve) * scaleW;
            const float maxScroll = content_h_ - viewH;
            if (maxScroll <= 0.0f) return 0;

            const int delta = GET_WHEEL_DELTA_WPARAM(wp);
            scroll_y_ -= static_cast<float>(delta) / 120.0f * 40.0f * ui::DpiScale(hwnd_);
            if (scroll_y_ < 0.0f) scroll_y_ = 0.0f;
            if (scroll_y_ > maxScroll) scroll_y_ = maxScroll;
            ::InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }

        case WM_KEYDOWN: {
            if (wp == VK_ESCAPE) {
                RequestClose();
                return 0;
            }
            return 0;
        }

        case WM_DESTROY: {
            ReleaseRenderTarget();
            ReleaseTextFormat();
            hwnd_ = nullptr;
            return 0;
        }

        default:
            break;
    }
    return ::DefWindowProcW(hwnd_, msg, wp, lp);
}

void PopupWindow::Paint() {
    if (hwnd_ == nullptr) return;

    if (rt_ == nullptr) {
        rt_ = ui::CreateHwndRenderTarget(hwnd_);
        if (rt_ == nullptr) return;
    }

    RECT rc{};
    ::GetClientRect(hwnd_, &rc);
    const float w = static_cast<float>(rc.right);
    const float h = static_cast<float>(rc.bottom);
    if (w <= 0 || h <= 0) return;

    ui::ResizeRenderTarget(rt_, hwnd_, rc.right, rc.bottom);

    const float scale = ui::DpiScale(hwnd_);

    rt_->BeginDraw();
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());

    ID2D1SolidColorBrush* brush = nullptr;
    rt_->CreateSolidColorBrush(kBg, &brush);
    if (brush != nullptr) {
        // 背景与边框
        const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(
            D2D1::RectF(0.5f, 0.5f, w - 0.5f, h - 0.5f),
            kRadius * scale, kRadius * scale);
        rt_->FillRoundedRectangle(rr, brush);

        brush->SetColor(kBorder);
        rt_->DrawRoundedRectangle(rr, brush, 1.0f);
        brush->Release();
        brush = nullptr;
    }

    // ---- 顶部：标题 + 关闭按钮 ----
    IDWriteTextFormat* titleFmt = ui::CreateTextFormat(
        12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING);
    ID2D1SolidColorBrush* textBrush = nullptr;
    rt_->CreateSolidColorBrush(kTextDim, &textBrush);

    if (titleFmt != nullptr && textBrush != nullptr) {
        const D2D1_RECT_F titleRect = D2D1::RectF(
            kPadX * scale, kPadY * scale,
            w - kPadX * scale - kCloseBtnSize * scale, (kPadY + 16.0f) * scale);
        const wchar_t* title = (state_ == State::Loading) ? L"鲤桥翻译"
                             : (state_ == State::Error)   ? L"鲤桥翻译 · 出错了"
                                                          : L"鲤桥翻译 · 译文";
        rt_->DrawTextW(title, static_cast<UINT32>(wcslen(title)), titleFmt,
                       titleRect, textBrush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    if (titleFmt != nullptr) titleFmt->Release();

    // 关闭按钮
    {
        const RECT cr = CloseButtonRect();
        ID2D1SolidColorBrush* cb = nullptr;
        if (close_hover_) {
            rt_->CreateSolidColorBrush(kCloseHover, &cb);
        } else {
            rt_->CreateSolidColorBrush(kTextDim, &cb);
        }
        if (cb != nullptr) {
            const float cx = static_cast<float>(cr.left + cr.right) * 0.5f;
            const float cy = static_cast<float>(cr.top + cr.bottom) * 0.5f;
            const float r  = 5.0f * scale;
            if (close_hover_) {
                rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r * 2.0f, r * 2.0f), cb);
                cb->SetColor(D2D1::ColorF(1, 1, 1, 1));
            }
            const float t = 1.4f * scale;
            rt_->DrawLine(D2D1::Point2F(cx - r, cy - r), D2D1::Point2F(cx + r, cy + r), cb, t);
            rt_->DrawLine(D2D1::Point2F(cx - r, cy + r), D2D1::Point2F(cx + r, cy - r), cb, t);
            cb->Release();
        }
    }

    // ---- 内容区 ----
    // 上下边界必须与 RecalcSize 的高度公式一致，否则内容区可能被压成负数
    // （曾导致译文被完全裁剪，弹窗只剩标题和按钮）。
    const float contentTop    = (kPadY + kHeaderH) * scale;
    const float contentBottom = h - kBottomReserve * scale;
    const float contentLeft   = kPadX * scale;
    const float contentRight  = w - kPadX * scale;
    const float contentWidth  = contentRight - contentLeft;
    const float contentHeight = contentBottom - contentTop;

    // 极端情况下（窗口被外部强制改小）保护一下，避免负高度绘制
    if (contentHeight <= 0.0f || contentWidth <= 0.0f) {
        rt_->EndDraw();
        return;
    }

    if (state_ == State::Loading) {
        // 加载动画：三个跳动的圆点
        if (textBrush != nullptr) {
            textBrush->SetColor(kAccent);
            const float dotR = 3.2f * scale;
            const float gap  = 12.0f * scale;
            const float baseX = contentLeft + 6.0f * scale;
            const float baseY = contentTop + contentHeight * 0.5f - 12.0f * scale;

            for (int i = 0; i < 3; ++i) {
                // 相位错开，形成波浪
                const int phase = (anim_frame_ - i * 4 + 12) % 12;
                float lift = 0.0f;
                if (phase < 6) {
                    lift = (phase <= 3) ? static_cast<float>(phase)
                                        : static_cast<float>(6 - phase);
                }
                lift *= 1.6f * scale;
                rt_->FillEllipse(
                    D2D1::Ellipse(D2D1::Point2F(baseX + i * gap, baseY - lift),
                                  dotR, dotR),
                    textBrush);
            }
        }

        if (textBrush != nullptr) {
            textBrush->SetColor(kText);
        }
        if (fmt_ == nullptr) {
            fmt_ = ui::CreateTextFormat(kFontSizeDip, DWRITE_FONT_WEIGHT_NORMAL);
        }
        if (fmt_ != nullptr && textBrush != nullptr) {
            const wchar_t* msg = L"DeepSeek 翻译中……";
            const D2D1_RECT_F r = D2D1::RectF(
                contentLeft, contentTop + contentHeight * 0.5f,
                contentRight, contentTop + contentHeight * 0.5f + 24.0f * scale);
            rt_->DrawTextW(msg, static_cast<UINT32>(wcslen(msg)), fmt_, r, textBrush,
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    } else {
        // 译文 / 错误文本
        if (textBrush != nullptr) {
            textBrush->SetColor(state_ == State::Error ? kErrText : kText);
        }
        if (fmt_ == nullptr) {
            fmt_ = ui::CreateTextFormat(kFontSizeDip, DWRITE_FONT_WEIGHT_NORMAL);
        }

        if (fmt_ != nullptr && textBrush != nullptr) {
            if (layout_ == nullptr) {
                ui::DWriteFactory()->CreateTextLayout(
                    text_.c_str(), static_cast<UINT32>(text_.size()),
                    fmt_, contentWidth, 100000.0f, &layout_);
                if (layout_ != nullptr) {
                    layout_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
                }
            }

            if (layout_ != nullptr) {
                // 裁剪到内容区，避免滚动时溢出到按钮区
                rt_->PushAxisAlignedClip(
                    D2D1::RectF(contentLeft, contentTop, contentRight, contentBottom),
                    D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                rt_->DrawTextLayout(
                    D2D1::Point2F(contentLeft, contentTop - scroll_y_),
                    layout_, textBrush);
                rt_->PopAxisAlignedClip();
            } else {
                // 回退：直接绘制
                const D2D1_RECT_F r = D2D1::RectF(contentLeft, contentTop - scroll_y_,
                                                  contentRight, contentBottom);
                rt_->DrawTextW(text_.c_str(), static_cast<UINT32>(text_.size()),
                               fmt_, r, textBrush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        }
    }

    if (textBrush != nullptr) textBrush->Release();

    // ---- 底部：复制按钮 ----
    if (state_ != State::Loading) {
        const RECT pr = CopyButtonRect();
        ID2D1SolidColorBrush* bb = nullptr;
        rt_->CreateSolidColorBrush(copy_hover_ ? kBtnBgHover : kBtnBg, &bb);
        if (bb != nullptr) {
            const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(
                D2D1::RectF(static_cast<float>(pr.left), static_cast<float>(pr.top),
                            static_cast<float>(pr.right), static_cast<float>(pr.bottom)),
                6.0f * scale, 6.0f * scale);
            rt_->FillRoundedRectangle(rr, bb);
            bb->Release();
        }

        IDWriteTextFormat* btnFmt = ui::CreateTextFormat(
            12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
        ID2D1SolidColorBrush* bt = nullptr;
        rt_->CreateSolidColorBrush(copy_done_ ? kAccent : kText, &bt);
        if (btnFmt != nullptr && bt != nullptr) {
            const wchar_t* label = copy_done_ ? L"已复制" : L"复制译文";
            const D2D1_RECT_F r = D2D1::RectF(
                static_cast<float>(pr.left), static_cast<float>(pr.top),
                static_cast<float>(pr.right), static_cast<float>(pr.bottom));
            rt_->DrawTextW(label, static_cast<UINT32>(wcslen(label)), btnFmt, r, bt,
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        if (btnFmt != nullptr) btnFmt->Release();
        if (bt != nullptr) bt->Release();
    }

    const HRESULT hr = rt_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        ReleaseRenderTarget();
        ReleaseTextFormat();
    }
}

} // namespace lt
