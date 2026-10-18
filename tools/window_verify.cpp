// 窗口样式运行时诊断
//
// 目的：确认主窗口与设置窗口**实际**拿到了什么样式。
// 我代码里写的是 WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME，
// 但用户反馈没有标题栏按钮 —— 必须用运行时数据判断是代码问题还是别的原因。
#include <windows.h>
#include <wincodec.h>
#include <cstdio>
#include <memory>
#include <string>

#include "app/settings_window.h"
#include "config/config_manager.h"
#include "ui/main_window.h"
#include "utils/logger.h"
#include "utils/string_utils.h"

#pragma comment(lib, "windowscodecs.lib")

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

static void DumpStyle(const char* tag, HWND h) {
    if (!h) { Say("  %s: <无窗口>\n", tag); return; }
    const LONG_PTR st = ::GetWindowLongPtrW(h, GWL_STYLE);
    const LONG_PTR ex = ::GetWindowLongPtrW(h, GWL_EXSTYLE);
    RECT rc{};
    ::GetWindowRect(h, &rc);

    Say("  %s\n", tag);
    Say("    句柄    : %p\n", (void*)h);
    Say("    STYLE   : 0x%08lX\n", (unsigned long)st);
    Say("    EXSTYLE : 0x%08lX\n", (unsigned long)ex);
    Say("    尺寸    : %dx%d @ (%d,%d)\n",
        rc.right - rc.left, rc.bottom - rc.top, rc.left, rc.top);
    Say("    WS_CAPTION     : %s\n", (st & WS_CAPTION) ? "有" : "【无】");
    Say("    WS_SYSMENU     : %s\n", (st & WS_SYSMENU) ? "有" : "【无】");
    Say("    WS_MINIMIZEBOX : %s\n", (st & WS_MINIMIZEBOX) ? "有" : "【无】");
    Say("    WS_THICKFRAME  : %s\n", (st & WS_THICKFRAME) ? "有" : "【无】");
    Say("    WS_POPUP       : %s\n", (st & WS_POPUP) ? "是（无标题栏）" : "否");
    Say("    可见           : %s\n", ::IsWindowVisible(h) ? "是" : "否");

    // 客户区 vs 窗口区：有标题栏时两者高度差应大于 20px
    RECT cr{};
    ::GetClientRect(h, &cr);
    const int frameH = (rc.bottom - rc.top) - (cr.bottom - cr.top);
    Say("    边框总高度     : %d px  %s\n", frameH,
        frameH > 20 ? "（含标题栏）" : "【无标题栏】");
}

static bool ShotWindow(HWND h, const wchar_t* path) {
    if (!h) return false;
    RECT rc{};
    ::GetWindowRect(h, &rc);
    const int w = rc.right - rc.left, hh = rc.bottom - rc.top;
    if (w <= 0 || hh <= 0) return false;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -hh;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    HDC screen = ::GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP dib = ::CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&bmi),
                                     DIB_RGB_COLORS, &bits, nullptr, 0);
    HDC dc = ::CreateCompatibleDC(screen);
    HGDIOBJ old = ::SelectObject(dc, dib);

    // 灰底，便于分辨窗口边界
    RECT full{ 0, 0, w, hh };
    HBRUSH bg = ::CreateSolidBrush(RGB(90, 95, 105));
    ::FillRect(dc, &full, bg);
    ::DeleteObject(bg);

    ::SetLastError(0);
    const BOOL ok = ::PrintWindow(h, dc, 2);
    Say("    PrintWindow=%d err=%lu\n", ok ? 1 : 0, (unsigned long)::GetLastError());

    ::SelectObject(dc, old);
    ::DeleteDC(dc);
    ::ReleaseDC(nullptr, screen);

    bool saved = false;
    IWICImagingFactory* f = nullptr;
    if (SUCCEEDED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                     CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) {
        IWICStream* st = nullptr; IWICBitmapEncoder* enc = nullptr;
        if (SUCCEEDED(f->CreateStream(&st)) &&
            SUCCEEDED(st->InitializeFromFilename(path, GENERIC_WRITE)) &&
            SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
            SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache))) {
            IWICBitmapFrameEncode* fr = nullptr; IPropertyBag2* pr = nullptr;
            if (SUCCEEDED(enc->CreateNewFrame(&fr, &pr))) {
                if (SUCCEEDED(fr->Initialize(pr))) {
                    fr->SetSize(w, hh);
                    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
                    fr->SetPixelFormat(&fmt);
                    if (SUCCEEDED(fr->WritePixels(hh, w * 4, w * 4 * hh,
                                                  static_cast<BYTE*>(bits))) &&
                        SUCCEEDED(fr->Commit())) {
                        saved = SUCCEEDED(enc->Commit());
                    }
                }
                if (pr) pr->Release();
                fr->Release();
            }
            enc->Release();
        }
        if (st) st->Release();
        f->Release();
    }
    ::DeleteObject(dib);
    return saved;
}

int wmain() {
    g = _wfopen(L"D:\\LightTransAI\\window_result.txt", L"wb");
    if (!g) return 2;
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    lt::Logger::Instance().Init(L"D:\\LightTransAI\\window_probe.log");
    lt::ConfigManager::Instance().Load();

    Say("======== 窗口样式运行时诊断 ========\n\n");

    // ---- 主窗口 ----
    Say("[1] 主窗口\n");
    auto mw = std::make_unique<lt::MainWindow>();
    const bool mok = mw->Show();
    Say("    Show: %s\n", mok ? "成功" : "失败");
    Pump(700);
    DumpStyle("主窗口实测", mw->Handle());
    if (ShotWindow(mw->Handle(), L"D:\\LightTransAI\\shot_main.png")) {
        Say("    截图: 已保存 shot_main.png\n");
    }

    // ---- 设置窗口 ----
    Say("\n[2] 设置窗口\n");
    auto sw = std::make_unique<lt::SettingsWindow>();
    const bool sok = sw->Show(nullptr);
    Say("    Show: %s\n", sok ? "成功" : "失败");
    Pump(700);
    DumpStyle("设置窗口实测", sw->Handle());
    if (ShotWindow(sw->Handle(), L"D:\\LightTransAI\\shot_settings.png")) {
        Say("    截图: 已保存 shot_settings.png\n");
    }

    // ---- 前台窗口是谁 ----
    Say("\n[3] 当前前台窗口\n");
    {
        HWND fg = ::GetForegroundWindow();
        wchar_t cls[128]{}, title[256]{};
        if (fg) {
            ::GetClassNameW(fg, cls, 127);
            ::GetWindowTextW(fg, title, 255);
        }
        Say("    类名: %ls\n", fg ? cls : L"<无>");
        Say("    标题: %ls\n", fg ? title : L"<无>");
    }

    Say("\n======== 完成 ========\n");

    if (sw) sw->Destroy();
    if (mw) mw->Destroy();
    ::CoUninitialize();
    if (g) std::fclose(g);
    return 0;
}
