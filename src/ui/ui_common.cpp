#include "ui/ui_common.h"

#include <d2d1helper.h>
#include <dwmapi.h>

#include "utils/logger.h"
#include "utils/string_utils.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dwmapi.lib")

namespace lt {
namespace ui {

namespace {

ID2D1Factory*   g_d2d   = nullptr;
IDWriteFactory* g_dwrite = nullptr;

// DWM 属性常量（避免依赖较新的 SDK 头文件宏）
constexpr DWORD kDwmWindowCornerPreference = 33;   // DWMWA_WINDOW_CORNER_PREFERENCE
constexpr DWORD kDwmBorderColor            = 34;   // DWMWA_BORDER_COLOR
constexpr int   kDwmRoundDefault           = 2;    // DWMWCP_ROUND
constexpr DWORD kDwmColorNone              = 0xFFFFFFFE;  // DWMWA_COLOR_NONE

} // namespace

ID2D1Factory* D2DFactory() {
    if (g_d2d == nullptr) {
        ::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2d);
    }
    return g_d2d;
}

IDWriteFactory* DWriteFactory() {
    if (g_dwrite == nullptr) {
        ::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                              reinterpret_cast<IUnknown**>(&g_dwrite));
    }
    return g_dwrite;
}

void ShutdownGraphics() {
    if (g_dwrite != nullptr) {
        g_dwrite->Release();
        g_dwrite = nullptr;
    }
    if (g_d2d != nullptr) {
        g_d2d->Release();
        g_d2d = nullptr;
    }
}

float DpiScale(HWND hwnd) {
    // GetDpiForWindow 需要 Win10 1607+；失败时回退到系统 DPI
    using PFN_GetDpiForWindow = UINT(WINAPI*)(HWND);
    static PFN_GetDpiForWindow pfn = []() -> PFN_GetDpiForWindow {
        HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
        if (user32 == nullptr) return nullptr;
        return reinterpret_cast<PFN_GetDpiForWindow>(
            ::GetProcAddress(user32, "GetDpiForWindow"));
    }();

    UINT dpi = 96;
    if (pfn != nullptr && hwnd != nullptr) {
        const UINT v = pfn(hwnd);
        if (v >= 48 && v <= 480) dpi = v;
    } else {
        HDC hdc = ::GetDC(nullptr);
        if (hdc != nullptr) {
            dpi = static_cast<UINT>(::GetDeviceCaps(hdc, LOGPIXELSX));
            ::ReleaseDC(nullptr, hdc);
        }
    }
    if (dpi == 0) dpi = 96;
    return static_cast<float>(dpi) / 96.0f;
}

int ScaleForDpi(int value, HWND hwnd) {
    return static_cast<int>(value * DpiScale(hwnd) + 0.5f);
}

void ApplyRoundedRegion(HWND hwnd, int radius_px) {
    if (hwnd == nullptr) return;
    RECT rc{};
    ::GetWindowRect(hwnd, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    // CreateRoundRectRgn 的右/下边界是开区间，需 +1
    HRGN rgn = ::CreateRoundRectRgn(0, 0, w + 1, h + 1, radius_px * 2, radius_px * 2);
    if (rgn != nullptr) {
        if (::SetWindowRgn(hwnd, rgn, TRUE) == 0) {
            ::DeleteObject(rgn);
        }
        // 成功时系统接管 rgn，无需释放
    }
}

void ApplyDwmRoundCorners(HWND hwnd) {
    if (hwnd == nullptr) return;
    const int pref = kDwmRoundDefault;
    ::DwmSetWindowAttribute(hwnd, kDwmWindowCornerPreference, &pref, sizeof(pref));

    // 去掉 Win11 默认的彩色描边，保持极简
    const DWORD border = kDwmColorNone;
    ::DwmSetWindowAttribute(hwnd, kDwmBorderColor, &border, sizeof(border));
}

void ApplyDwmBorderlessShadow(HWND hwnd) {
    if (hwnd == nullptr) return;
    // 扩展边框到客户区，让 DWM 为无边框窗口绘制投影
    MARGINS margins{ 1, 1, 1, 1 };
    ::DwmExtendFrameIntoClientArea(hwnd, &margins);
}

ID2D1HwndRenderTarget* CreateHwndRenderTarget(HWND hwnd) {
    ID2D1Factory* factory = D2DFactory();
    if (factory == nullptr || hwnd == nullptr) return nullptr;

    RECT rc{};
    ::GetClientRect(hwnd, &rc);
    const D2D1_SIZE_U size = D2D1::SizeU(
        static_cast<UINT32>(rc.right > 0 ? rc.right : 1),
        static_cast<UINT32>(rc.bottom > 0 ? rc.bottom : 1));

    ID2D1HwndRenderTarget* rt = nullptr;
    const HRESULT hr = factory->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_UNKNOWN),
            static_cast<float>(96 * DpiScale(hwnd)),
            static_cast<float>(96 * DpiScale(hwnd))),
        D2D1::HwndRenderTargetProperties(hwnd, size,
                                         D2D1_PRESENT_OPTIONS_IMMEDIATELY),
        &rt);
    if (FAILED(hr)) return nullptr;
    return rt;
}

ID2D1HwndRenderTarget* ResizeRenderTarget(ID2D1HwndRenderTarget* rt, HWND hwnd,
                                          UINT width, UINT height) {
    if (rt == nullptr) return CreateHwndRenderTarget(hwnd);
    if (width == 0 || height == 0) return rt;
    rt->Resize(D2D1::SizeU(width, height));
    return rt;
}

IDWriteTextFormat* CreateTextFormat(float size_dip,
                                    DWRITE_FONT_WEIGHT weight,
                                    DWRITE_TEXT_ALIGNMENT align,
                                    const wchar_t* family,
                                    DWRITE_PARAGRAPH_ALIGNMENT valign) {
    IDWriteFactory* factory = DWriteFactory();
    if (factory == nullptr) return nullptr;

    // 中文界面优先使用微软雅黑，缺失时回退 Segoe UI
    const wchar_t* fam = (family != nullptr) ? family : L"Microsoft YaHei UI";

    IDWriteTextFormat* fmt = nullptr;
    HRESULT hr = factory->CreateTextFormat(
        fam, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        size_dip, L"zh-CN", &fmt);

    if (FAILED(hr) && family == nullptr) {
        hr = factory->CreateTextFormat(
            L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size_dip, L"zh-CN", &fmt);
    }
    if (FAILED(hr) || fmt == nullptr) return nullptr;

    fmt->SetTextAlignment(align);
    fmt->SetParagraphAlignment(valign);
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    return fmt;
}

POINT ClampToWorkArea(int x, int y, int w, int h) {
    // 取距目标点最近的显示器工作区（自动排除任务栏）
    POINT probe{ x, y };
    HMONITOR mon = ::MonitorFromPoint(probe, MONITOR_DEFAULTTONEAREST);

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    RECT work{ 0, 0, ::GetSystemMetrics(SM_CXSCREEN), ::GetSystemMetrics(SM_CYSCREEN) };
    if (::GetMonitorInfoW(mon, &mi)) {
        work = mi.rcWork;
    }

    int nx = x;
    int ny = y;

    if (nx + w > work.right)  nx = work.right - w;
    if (ny + h > work.bottom) ny = work.bottom - h;
    if (nx < work.left)       nx = work.left;
    if (ny < work.top)        ny = work.top;

    return POINT{ nx, ny };
}

POINT PlaceRightOfAnchor(POINT anchor, int w, int h, int gap, bool* flipped_x) {
    if (flipped_x != nullptr) *flipped_x = false;

    HMONITOR mon = ::MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    RECT work{ 0, 0, ::GetSystemMetrics(SM_CXSCREEN), ::GetSystemMetrics(SM_CYSCREEN) };
    if (::GetMonitorInfoW(mon, &mi)) {
        work = mi.rcWork;
    }

    // 垂直方向：以锚点为中心对齐，再夹到工作区内。
    // 这样按钮看起来是"挂在选区末尾的右边"，而不是浮在下方。
    int y = anchor.y - h / 2;
    if (y + h > work.bottom) y = work.bottom - h;
    if (y < work.top)        y = work.top;

    // 水平方向：优先放右侧
    int x = anchor.x + gap;
    if (x + w > work.right) {
        // 右侧放不下 —— 翻到左侧。翻转后同样要做边界保护。
        x = anchor.x - gap - w;
        if (flipped_x != nullptr) *flipped_x = true;
        if (x < work.left) x = work.left;
    }

    return POINT{ x, y };
}

// ---------------------------------------------------------------- 分层画布

void LayeredCanvas::Release() {
    if (rt_ != nullptr) { rt_->Release(); rt_ = nullptr; }
    if (memDc_ != nullptr) {
        if (memOld_ != nullptr) ::SelectObject(memDc_, memOld_);
        ::DeleteDC(memDc_);
        memDc_ = nullptr;
    }
    if (memBmp_ != nullptr) { ::DeleteObject(memBmp_); memBmp_ = nullptr; }
    memOld_ = nullptr;
    bits_   = nullptr;
    width_  = 0;
    height_ = 0;
}

bool LayeredCanvas::Ensure(HWND hwnd) {
    if (hwnd == nullptr) return false;

    RECT rc{};
    ::GetClientRect(hwnd, &rc);
    const int w = rc.right;
    const int h = rc.bottom;
    if (w <= 0 || h <= 0) return false;

    if (memDc_ != nullptr && w == width_ && h == height_ && rt_ != nullptr) {
        return true;   // 已就绪且尺寸未变
    }
    return Resize(hwnd, static_cast<UINT>(w), static_cast<UINT>(h));
}

bool LayeredCanvas::Resize(HWND hwnd, UINT width, UINT height) {
    Release();
    if (width == 0 || height == 0) return false;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = static_cast<LONG>(width);
    bmi.bmiHeader.biHeight      = -static_cast<LONG>(height);   // 自上而下
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    HDC screen = ::GetDC(nullptr);
    memBmp_ = ::CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits_, nullptr, 0);
    memDc_  = ::CreateCompatibleDC(screen);
    ::ReleaseDC(nullptr, screen);

    if (memBmp_ == nullptr || memDc_ == nullptr || bits_ == nullptr) {
        Release();
        return false;
    }
    memOld_ = ::SelectObject(memDc_, memBmp_);
    width_  = static_cast<int>(width);
    height_ = static_cast<int>(height);

    ID2D1Factory* factory = D2DFactory();
    if (factory == nullptr) { Release(); return false; }

    // 像素格式必须是预乘 alpha，UpdateLayeredWindow 才能正确合成
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        static_cast<float>(96 * DpiScale(hwnd)),
        static_cast<float>(96 * DpiScale(hwnd)));

    if (FAILED(factory->CreateDCRenderTarget(&props, &rt_)) || rt_ == nullptr) {
        Release();
        return false;
    }
    return true;
}

bool LayeredCanvas::BeginDraw(HWND hwnd) {
    if (!Ensure(hwnd) || rt_ == nullptr) return false;

    RECT rc{ 0, 0, width_, height_ };
    if (FAILED(rt_->BindDC(memDc_, &rc))) return false;

    rt_->BeginDraw();
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
    rt_->Clear(D2D1::ColorF(0, 0.0f));   // 全透明
    return true;
}

bool LayeredCanvas::EndDrawAndPresent(HWND hwnd, int x, int y) {
    if (rt_ == nullptr || hwnd == nullptr) return false;

    const HRESULT hr = rt_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        Release();
        return false;
    }
    if (FAILED(hr)) return false;

    POINT srcPos{ 0, 0 };
    SIZE  size{ width_, height_ };
    POINT dstPos{ x, y };
    BLENDFUNCTION bf{};
    bf.BlendOp             = AC_SRC_OVER;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat         = AC_SRC_ALPHA;

    ::SetLastError(0);
    const BOOL ok = ::UpdateLayeredWindow(hwnd, nullptr, &dstPos, &size,
                                          memDc_, &srcPos, 0, &bf, ULW_ALPHA);
    if (!ok) {
        LT_ERROR(L"UpdateLayeredWindow 失败，错误码 " + std::to_wstring(::GetLastError()) +
                 L"（窗口必须带 WS_EX_LAYERED）");
    }
    return ok != FALSE;
}

} // namespace ui
} // namespace lt
