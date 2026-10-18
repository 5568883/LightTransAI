// 框选遮罩可见性验证（受控背景版）
//
// 上一版测试的缺陷：直接测量真实屏幕区域，而那些区域的内容
// （控制台窗口、其它程序）不受控制，导致"选区内外亮度对比"失去意义。
//
// 本版创建一个**纯白全屏背景窗口**作为受控基准，于是亮度可以精确预期：
//   遮罩前          : ~255（纯白）
//   遮罩显示（未拖）: ~255 * (1 - 150/255) ≈ 105（压暗）
//   拖拽中 选区内   : ~255（保持原亮度）
//   拖拽中 选区外   : ~105（压暗）
//   关闭后          : ~255（恢复）
#include <windows.h>
#include <cstdio>
#include <string>

#include "capture/screen_capturer.h"
#include "utils/logger.h"
#include "utils/string_utils.h"

static FILE* g = nullptr;
static void Say(const char* fmt, ...) {
    if (!g) return;
    va_list ap; va_start(ap, fmt);
    std::vfprintf(g, fmt, ap);
    va_end(ap); std::fflush(g);
}

static void Pump(int ms) {
    const DWORD end = ::GetTickCount() + ms;
    MSG m{};
    while (::GetTickCount() < end) {
        while (::PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&m);
            ::DispatchMessageW(&m);
        }
        ::Sleep(5);
    }
}

static double ShotLuma(const RECT& r) {
    const int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return -1.0;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    HDC screen = ::GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP bmp = ::CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HDC dc = ::CreateCompatibleDC(screen);
    HGDIOBJ old = ::SelectObject(dc, bmp);
    ::BitBlt(dc, 0, 0, w, h, screen, r.left, r.top, SRCCOPY | CAPTUREBLT);

    double sum = 0.0;
    if (bits) {
        auto* px = static_cast<uint32_t*>(bits);
        for (int i = 0; i < w * h; ++i) {
            const double b = px[i] & 0xFF;
            const double gg = (px[i] >> 8) & 0xFF;
            const double rr = (px[i] >> 16) & 0xFF;
            sum += rr * 0.299 + gg * 0.587 + b * 0.114;
        }
        sum /= (w * h);
    }
    ::SelectObject(dc, old);
    ::DeleteDC(dc);
    ::DeleteObject(bmp);
    ::ReleaseDC(nullptr, screen);
    return sum;
}

// 受控背景：纯白全屏窗口
static HWND g_backdrop = nullptr;

static LRESULT CALLBACK BackdropProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_ERASEBKGND) {
        RECT rc{};
        ::GetClientRect(h, &rc);
        HBRUSH b = ::CreateSolidBrush(RGB(255, 255, 255));
        ::FillRect(reinterpret_cast<HDC>(w), &rc, b);
        ::DeleteObject(b);
        return 1;
    }
    if (m == WM_PAINT) {
        PAINTSTRUCT ps{};
        ::BeginPaint(h, &ps);
        RECT rc{};
        ::GetClientRect(h, &rc);
        HBRUSH b = ::CreateSolidBrush(RGB(255, 255, 255));
        ::FillRect(ps.hdc, &rc, b);
        ::DeleteObject(b);
        ::EndPaint(h, &ps);
        return 0;
    }
    return ::DefWindowProcW(h, m, w, l);
}

int wmain() {
    g = _wfopen(L"D:\\LightTransAI\\visible_result.txt", L"wb");
    if (!g) return 2;
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    lt::Logger::Instance().Init(L"D:\\LightTransAI\\visible_probe.log");

    Say("======== 框选遮罩可见性验证（受控白底）========\n\n");
    int pass = 0, fail = 0;
    auto check = [&](bool ok, const char* what) {
        if (ok) ++pass; else { fail++; Say("  [失败] %s\n", what); }
    };

    // ---- 建立纯白全屏背景 ----
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = BackdropProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = L"LTWhiteBackdrop";
    wc.hbrBackground = reinterpret_cast<HBRUSH>(::GetStockObject(WHITE_BRUSH));
    ::RegisterClassExW(&wc);

    const int vx = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vh = ::GetSystemMetrics(SM_CYVIRTUALSCREEN);

    g_backdrop = ::CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"", WS_POPUP,
                                   vx, vy, vw, vh, nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_backdrop) { Say("白底窗口创建失败\n"); return 1; }
    ::ShowWindow(g_backdrop, SW_SHOW);
    ::UpdateWindow(g_backdrop);
    Pump(500);
    Say("[0] 白底窗口已建立 %dx%d\n", vw, vh);

    // 采样点（都在白底范围内）
    const RECT rInside{ 300, 250, 500, 450 };    // 将落在选区内
    const RECT rOutside{ 800, 250, 1000, 450 };  // 将落在选区外

    const double lumaBeforeIn = ShotLuma(rInside);
    const double lumaBeforeOut = ShotLuma(rOutside);
    Say("    遮罩前: 选区位置 %.1f   选区外位置 %.1f\n",
        lumaBeforeIn, lumaBeforeOut);
    check(lumaBeforeIn > 200.0, "白底应接近 255");

    // ---- 显示遮罩（未拖拽）----
    bool called = false;
    lt::CaptureSelection got{};
    lt::ScreenCapturer cap;
    const bool begun = cap.Begin([&](const lt::CaptureSelection& sel) {
        got = sel; called = true;
    });
    check(begun, "Begin 应成功");
    if (!begun) { ::DestroyWindow(g_backdrop); if (g) std::fclose(g); return 1; }
    Pump(400);

    HWND ov = ::FindWindowW(L"LightTransAI_CaptureOverlay", nullptr);
    check(ov != nullptr, "遮罩窗口应存在");
    if (!ov) { cap.Abort(); ::DestroyWindow(g_backdrop); if (g) std::fclose(g); return 1; }

    const LONG_PTR ex = ::GetWindowLongPtrW(ov, GWL_EXSTYLE);
    Say("\n[1] 扩展样式: 0x%08lX\n", (unsigned long)ex);
    check((ex & WS_EX_LAYERED) != 0, "必须有 WS_EX_LAYERED");

    const double lumaDimIn = ShotLuma(rInside);
    const double lumaDimOut = ShotLuma(rOutside);
    Say("\n[2] 遮罩显示（未拖拽）: %.1f / %.1f\n", lumaDimIn, lumaDimOut);
    Say("    相比遮罩前: %.1f -> %.1f  (预期压暗到约 105)\n",
        lumaBeforeIn, lumaDimIn);
    check(lumaDimIn < lumaBeforeIn - 30.0, "遮罩应把白底压暗");
    check(lumaDimIn > 20.0, "压暗后不应变成全黑");

    // ---- 拖拽：选区覆盖 rInside ----
    ::SendMessageW(ov, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(200 - vx, 200 - vy));
    Pump(100);
    ::SendMessageW(ov, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(600 - vx, 500 - vy));
    Pump(300);

    check(::IsWindowVisible(ov) != FALSE, "截图时遮罩应可见");

    const double lumaSelIn = ShotLuma(rInside);
    const double lumaSelOut = ShotLuma(rOutside);
    Say("\n[3] 拖拽中（选区 200,200-600,500）\n");
    Say("    选区内 (300-500,250-450): %.1f  (预期恢复到约 %.1f)\n",
        lumaSelIn, lumaBeforeIn);
    Say("    选区外 (800-1000,250-450): %.1f  (预期保持压暗)\n", lumaSelOut);

    const bool insideRestored = lumaSelIn > lumaDimIn + 30.0;
    const bool outsideDimmed  = lumaSelOut < lumaBeforeOut - 30.0;
    Say("    选区内已恢复亮度: %s\n", insideRestored ? "是" : "【否】");
    Say("    选区外保持压暗  : %s\n", outsideDimmed ? "是" : "【否】");
    check(insideRestored, "选区内部应恢复原亮度");
    check(outsideDimmed, "选区外部应保持压暗");
    check(lumaSelIn > lumaSelOut + 30.0, "选区内应明显比选区外亮");

    // 白色边框检测：沿选区上边缘采样一条窄带
    const double lumaEdge = ShotLuma(RECT{ 200, 199, 600, 202 });
    Say("    上边缘窄带亮度: %.1f (白线应拉高亮度)\n", lumaEdge);
    check(lumaEdge > lumaSelOut - 10.0, "边缘白线应比压暗区更亮");

    // ---- 完成 ----
    ::SendMessageW(ov, WM_LBUTTONUP, 0, MAKELPARAM(600 - vx, 500 - vy));
    Pump(600);

    Say("\n[4] 回调: called=%d accepted=%d 选区 %dx%d\n",
        called ? 1 : 0, got.accepted ? 1 : 0,
        got.screen_rect.right - got.screen_rect.left,
        got.screen_rect.bottom - got.screen_rect.top);
    check(called && got.accepted, "拖拽应被接受");
    check(got.screen_rect.left == 200 && got.screen_rect.top == 200,
          "选区起点应为 (200,200)");

    // ---- 关闭后恢复 ----
    ::ShowWindow(g_backdrop, SW_SHOW);
    ::UpdateWindow(g_backdrop);
    Pump(400);

    const double lumaAfter = ShotLuma(rInside);
    Say("\n[5] 遮罩关闭后: %.1f  (遮罩前 %.1f, 压暗时 %.1f)\n",
        lumaAfter, lumaBeforeIn, lumaDimIn);
    check(lumaAfter > lumaDimIn + 30.0, "关闭后应恢复亮度");

    Say("\n========================================\n");
    Say("  通过: %d   失败: %d\n", pass, fail);
    Say("========================================\n");

    if (g_backdrop) ::DestroyWindow(g_backdrop);
    ::CoUninitialize();
    if (g) std::fclose(g);
    return fail == 0 ? 0 : 1;
}
