#include "ui/float_button_window.h"

#include <d2d1helper.h>
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM

#include "app/app_messages.h"
#include "ui/ui_common.h"
#include "utils/logger.h"

namespace lt {

namespace {

// ============ 视觉设计 ============
//
// 设计目标：在任意背景（浅色网页 / 深色编辑器 / 图片）上都清晰、精致，
// 并且与程序其它界面（设置窗口、翻译弹窗）的浅色风格保持一致。
//
// 结构（自外向内）：
//   ┌──────────────────────────┐
//   │  柔和投影（9px 内边距）    │
//   │   ┌────────────────────┐ │
//   │   │ ●译  翻译           │ │  ← 白色胶囊 + 品牌蓝圆形图标
//   │   └────────────────────┘ │
//   └──────────────────────────┘
//
// 为什么用白底而不是半透明深色：
//   半透明在不同背景下对比度不稳定；纯白 + 投影在浅色和深色背景上
//   都能稳定分离，观感也更干净。
//
// 位置：贴在**划选内容末尾的右侧**（见 Show()）。

constexpr float kShadowPad   = 9.0f;    // 投影预留边距
// 胶囊宽度按内容精算：左内边距 5 + 图标直径 21 + 间距 6 + 文字「翻译」约 26 + 右内边距 9 ≈ 67
constexpr float kPillWidth   = 70.0f;
constexpr float kPillHeight  = 30.0f;
constexpr float kPillRadius  = 15.0f;   // 胶囊形（= 高度的一半）
constexpr float kIconRadius  = 10.5f;   // 左侧圆形图标半径

// 与选区末尾之间的水平间距（DIP）。
// 取 8：够看出"这是独立的一个按钮"，又不会离选区太远而需要移动视线去找。
constexpr float kAnchorGap   = 8.0f;

// 配色（与设置窗口/弹窗同一套浅色体系）
const D2D1_COLOR_F kPillBg      = D2D1::ColorF(1.00f, 1.00f, 1.00f, 1.0f);
const D2D1_COLOR_F kPillBgHover = D2D1::ColorF(0.95f, 0.965f, 0.99f, 1.0f);
const D2D1_COLOR_F kPillBorder  = D2D1::ColorF(0.83f, 0.845f, 0.88f, 1.0f);
const D2D1_COLOR_F kAccent      = D2D1::ColorF(0.18f, 0.435f, 0.949f, 1.0f);  // 品牌蓝
const D2D1_COLOR_F kTextColor   = D2D1::ColorF(0.11f, 0.12f, 0.15f, 1.0f);
const D2D1_COLOR_F kShadow      = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.16f);

constexpr wchar_t kButtonText[] = L"翻译";
// 图标内用「译」而不是字母 A：与托盘图标、应用图标统一，
// 用户一眼就知道这是同一个产品。
constexpr wchar_t kIconGlyph[]  = L"译";

} // namespace

bool FloatButtonWindow::EnsureClassRegistered() {
    static bool registered = false;
    if (registered) return true;

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc   = &FloatButtonWindow::WndProc;
    wc.hInstance     = ::GetModuleHandleW(nullptr);
    wc.hCursor       = ::LoadCursorW(nullptr, IDC_HAND);
    wc.lpszClassName = kFloatButtonClass;
    wc.hbrBackground = nullptr;   // 自绘，不需要背景刷

    if (::RegisterClassExW(&wc) == 0) {
        const DWORD err = ::GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            LT_ERROR(L"注册悬浮按钮窗口类失败，错误码 " + std::to_wstring(err));
            return false;
        }
    }
    registered = true;
    return true;
}

bool FloatButtonWindow::Show(POINT anchor) {
    if (!EnsureClassRegistered()) return false;

    // 已显示则先销毁，避免残留
    if (hwnd_ != nullptr) Destroy();

    // 用临时窗口取得 DPI 换算比例（按锚点所在显示器）
    const UINT dpi = [] (POINT pt) -> UINT {
        using PFN_GetDpiForWindow = UINT(WINAPI*)(HWND);
        using PFN_GetDpiForMonitor = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
        HMONITOR mon = ::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        HMODULE shcore = ::LoadLibraryW(L"shcore.dll");
        if (shcore != nullptr) {
            auto pfn = reinterpret_cast<PFN_GetDpiForMonitor>(
                ::GetProcAddress(shcore, "GetDpiForMonitor"));
            if (pfn != nullptr) {
                UINT dx = 96, dy = 96;
                if (SUCCEEDED(pfn(mon, 0 /*MDT_EFFECTIVE_DPI*/, &dx, &dy)) && dx >= 48) {
                    ::FreeLibrary(shcore);
                    return dx;
                }
            }
            ::FreeLibrary(shcore);
        }
        (void)sizeof(PFN_GetDpiForWindow);
        return 96;
    }(anchor);

    const float scale = static_cast<float>(dpi) / 96.0f;
    // 窗口尺寸 = 胶囊 + 四周投影留白
    const int w = static_cast<int>((kPillWidth  + kShadowPad * 2) * scale + 0.5f);
    const int h = static_cast<int>((kPillHeight + kShadowPad * 2) * scale + 0.5f);

    // 位置：贴在划选内容末尾的**右侧**，垂直方向与选区末尾对齐。
    //
    // anchor 是鼠标松开点，也就是选区的末尾。放右侧而不是右下方的理由：
    // 用户的视线刚停在选区末尾，右侧是视线自然延伸的方向；
    // 放下方会盖住下一行文字，而且离光标更远、需要移动视线去找。
    //
    // 若右侧放不下（选区贴近屏幕右缘），PlaceRightOfAnchor 会自动翻到左侧，
    // 并把翻转结果写进 flipped_，绘制时据此镜像投影方向。
    const POINT pos = ui::PlaceRightOfAnchor(
        anchor, w, h, static_cast<int>(kAnchorGap * scale + 0.5f), &flipped_);
    const int x = pos.x;
    const int y = pos.y;

    // 关键扩展样式。
    // WS_EX_LAYERED 是必需的：投影与圆角需要逐像素 alpha，
    // 而 ID2D1HwndRenderTarget 无法把逐像素 alpha 合成到屏幕。
    const DWORD exStyle = WS_EX_LAYERED | WS_EX_NOACTIVATE |
                          WS_EX_TOPMOST | WS_EX_TOOLWINDOW;
    const DWORD style   = WS_POPUP;

    hwnd_ = ::CreateWindowExW(exStyle, kFloatButtonClass, L"", style,
                              x, y, w, h,
                              nullptr, nullptr, ::GetModuleHandleW(nullptr), this);
    if (hwnd_ == nullptr) {
        LT_ERROR(L"创建悬浮按钮失败，错误码 " + std::to_wstring(::GetLastError()));
        return false;
    }

    // 不做窗口区域裁剪：投影需要窗口本身是矩形，
    // 圆角由 Direct2D 绘制（胶囊自身是圆角矩形，窗口背景全透明）。
    //
    // 这里**刻意不调用** ui::ApplyDwmRoundCorners。
    // 分层窗口的内容完全由 UpdateLayeredWindow 提交，DWM 的圆角/描边处理
    // 会按窗口矩形再画一层底，于是胶囊外面出现一个白色方框 ——
    // 正好是窗口比胶囊大出的那圈投影留白。两套圆角机制必须只留一套。

    // 记录位置：UpdateLayeredWindow 需要目标屏幕坐标
    pos_ = POINT{ x, y };

    // SWP_NOACTIVATE 确保显示时也不抢焦点
    ::ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    ::SetWindowPos(hwnd_, HWND_TOPMOST, x, y, w, h,
                   SWP_NOACTIVATE | SWP_SHOWWINDOW);

    // 主动绘制一次。
    //
    // 关键：本窗口用 WS_EX_LAYERED + UpdateLayeredWindow 呈现，**不能**只等
    // WM_PAINT —— 分层窗口的内容由 UpdateLayeredWindow 直接提交，系统不保证
    // 在 ShowWindow 后派发 WM_PAINT，结果就是一个位置正确但完全透明的空白窗口。
    Paint();

    LT_INFO(L"已显示 [翻译] 悬浮按钮");
    return true;
}

void FloatButtonWindow::Destroy() {
    ReleaseRenderTarget();
    if (fmt_ != nullptr) {
        fmt_->Release();
        fmt_ = nullptr;
    }
    if (hwnd_ != nullptr) {
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        LT_INFO(L"已销毁 [翻译] 悬浮按钮");
    }
    hover_ = false;
    flipped_ = false;
}

void FloatButtonWindow::ReleaseRenderTarget() {
    canvas_.Release();
}

bool FloatButtonWindow::HitTest(POINT screen_pt) const {
    if (hwnd_ == nullptr) return false;
    RECT rc{};
    ::GetWindowRect(hwnd_, &rc);
    return ::PtInRect(&rc, screen_pt) != FALSE;
}

LRESULT CALLBACK FloatButtonWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        auto* self = reinterpret_cast<FloatButtonWindow*>(cs->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                            reinterpret_cast<LONG_PTR>(self));
        // 创建期间即绑定句柄（详见 main_window.cpp 的说明）。
        if (self != nullptr) self->hwnd_ = hwnd;
        return ::DefWindowProcW(hwnd, msg, wp, lp);
    }

    auto* self = reinterpret_cast<FloatButtonWindow*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self == nullptr) {
        return ::DefWindowProcW(hwnd, msg, wp, lp);
    }
    return self->HandleMessage(msg, wp, lp);
}

LRESULT FloatButtonWindow::HandleMessage(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;  // 由分层画布负责绘制，阻止系统擦除造成的闪烁

        case WM_PAINT: {
            // 分层窗口不用 BeginPaint/EndPaint 绘制，这里只重绘一次内容
            PAINTSTRUCT ps{};
            ::BeginPaint(hwnd_, &ps);
            Paint();
            ::EndPaint(hwnd_, &ps);
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (!hover_) {
                hover_ = true;
                // 进入时启动 TrackMouseEvent 以便收到 WM_MOUSELEAVE
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
            hover_ = false;
            ::InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }

        case WM_LBUTTONUP: {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ::ClientToScreen(hwnd_, &pt);
            // 点击后立即隐藏，避免遮挡即将出现的翻译窗口
            ::ShowWindow(hwnd_, SW_HIDE);
            if (on_click_) on_click_(pt);
            return 0;
        }

        case WM_RBUTTONUP:
        case WM_MBUTTONUP: {
            // 其它键视为放弃
            if (on_expired_) on_expired_();
            return 0;
        }

        case WM_MOUSEWHEEL: {
            // 滚轮说明用户在操作底层窗口，撤销按钮
            if (on_expired_) on_expired_();
            return 0;
        }

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            if (on_expired_) on_expired_();
            return 0;
        }

        case WM_DESTROY: {
            ReleaseRenderTarget();
            hwnd_ = nullptr;
            return 0;
        }

        default:
            break;
    }
    return ::DefWindowProcW(hwnd_, msg, wp, lp);
}

void FloatButtonWindow::Paint() {
    if (hwnd_ == nullptr) return;

    if (!canvas_.BeginDraw(hwnd_)) {
        LT_WARN(L"悬浮按钮绘制失败：分层画布 BeginDraw 返回 false");
        return;
    }

    ID2D1DCRenderTarget* rt = canvas_.Rt();
    if (rt == nullptr) {
        LT_WARN(L"悬浮按钮绘制失败：DCRenderTarget 为空");
        return;
    }

    RECT rc{};
    ::GetClientRect(hwnd_, &rc);
    const float w = static_cast<float>(rc.right);
    const float h = static_cast<float>(rc.bottom);
    if (w <= 0 || h <= 0) {
        LT_WARN(L"悬浮按钮绘制失败：客户区尺寸为 0");
        return;
    }

    const float scale = ui::DpiScale(hwnd_);
    const float pad   = kShadowPad * scale;

    // 胶囊矩形（窗口内缩去投影留白）
    const float pillL = pad;
    const float pillT = pad;
    const float pillR = w - pad;
    const float pillB = h - pad;
    const float radius = kPillRadius * scale;

    // ---- 1) 柔和投影：多层递减透明度的圆角矩形近似高斯模糊 ----
    // Direct2D 无内置投影，用几层偏移+放大的圆角矩形叠加即可得到自然观感。
    // 光从上方来，所以整体向下偏移 1.5px；水平方向按是否翻转做镜像，
    // 让投影始终背离选区（按钮在右则投影偏右，在左则偏左）。
    {
        const float dx = flipped_ ? -1.0f * scale : 1.0f * scale;
        const int kLayers = 5;
        for (int i = kLayers; i >= 1; --i) {
            const float grow = i * 1.1f * scale;
            const float alpha = kShadow.a * (0.34f / i);
            ID2D1SolidColorBrush* sb = nullptr;
            rt->CreateSolidColorBrush(
                D2D1::ColorF(kShadow.r, kShadow.g, kShadow.b, alpha), &sb);
            if (sb != nullptr) {
                const D2D1_ROUNDED_RECT sr = D2D1::RoundedRect(
                    D2D1::RectF(pillL - grow + dx, pillT - grow + 1.5f * scale,
                                pillR + grow + dx, pillB + grow + 1.5f * scale),
                    radius + grow, radius + grow);
                rt->FillRoundedRectangle(sr, sb);
                sb->Release();
            }
        }
    }

    // ---- 2) 胶囊主体 ----
    ID2D1SolidColorBrush* brush = nullptr;
    rt->CreateSolidColorBrush(hover_ ? kPillBgHover : kPillBg, &brush);
    if (brush != nullptr) {
        const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(
            D2D1::RectF(pillL, pillT, pillR, pillB), radius, radius);
        rt->FillRoundedRectangle(rr, brush);

        brush->SetColor(kPillBorder);
        rt->DrawRoundedRectangle(rr, brush, 1.0f * scale);
        brush->Release();
        brush = nullptr;
    }

    // ---- 3) 左侧品牌蓝圆形图标 ----
    // 图标中心：距胶囊左边缘 = 内边距(6) + 半径
    const float iconCx = pillL + (6.0f * scale) + kIconRadius * scale;
    const float iconCy = (pillT + pillB) * 0.5f;
    const float iconR  = kIconRadius * scale;

    ID2D1SolidColorBrush* accent = nullptr;
    rt->CreateSolidColorBrush(kAccent, &accent);
    if (accent != nullptr) {
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(iconCx, iconCy), iconR, iconR),
                        accent);
        accent->Release();
        accent = nullptr;
    }

    // 图标内的白色「译」字。
    // 垂直方向必须显式居中：CreateTextFormat 默认是 NEAR（顶对齐），
    // 汉字在这种小尺寸下会明显偏上。
    IDWriteTextFormat* glyphFmt = ui::CreateTextFormat(
        10.0f, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, nullptr,
        DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    if (glyphFmt != nullptr) {
        ID2D1SolidColorBrush* wb = nullptr;
        rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &wb);
        if (wb != nullptr) {
            // 矩形比圆略大：DWrite 会把文本行高算得比字形高，留出余量才不会被裁
            const D2D1_RECT_F gr = D2D1::RectF(iconCx - iconR, iconCy - iconR,
                                               iconCx + iconR, iconCy + iconR);
            rt->DrawTextW(kIconGlyph, 1, glyphFmt, gr, wb,
                          D2D1_DRAW_TEXT_OPTIONS_CLIP);
            wb->Release();
        }
        glyphFmt->Release();
    }

    // ---- 4) 「翻译」文字 ----
    if (fmt_ == nullptr) {
        fmt_ = ui::CreateTextFormat(13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                    DWRITE_TEXT_ALIGNMENT_LEADING, nullptr,
                                    DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    if (fmt_ != nullptr) {
        ID2D1SolidColorBrush* tb = nullptr;
        rt->CreateSolidColorBrush(kTextColor, &tb);
        if (tb != nullptr) {
            const float textL = iconCx + iconR + 7.0f * scale;
            const D2D1_RECT_F tr = D2D1::RectF(textL, pillT, pillR - 6.0f * scale, pillB);
            rt->DrawTextW(kButtonText, 2, fmt_, tr, tb,
                          D2D1_DRAW_TEXT_OPTIONS_CLIP);
            tb->Release();
        }
    }

    canvas_.EndDrawAndPresent(hwnd_, pos_.x, pos_.y);
}

} // namespace lt
