// 悬浮按钮外观验证 v2
//
// v1 用 BitBlt 从屏幕抓分层窗口，得到全黑 —— 该方法对分层窗口不可靠
// （UpdateLayeredWindow 的内容不在普通屏幕 DC 的可抓取范围内）。
//
// v2 改为：让 LayeredCanvas 把同一份绘制结果**同时**输出成 PNG，
// 绕开屏幕抓取，直接检验绘制内容本身。
#include <windows.h>
#include <wincodec.h>
#include <cstdio>
#include <string>
#include <vector>

#include "ui/float_button_window.h"
#include "ui/ui_common.h"
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

// 用 PrintWindow 抓分层窗口（比 BitBlt 更可靠）
static bool ShotByPrintWindow(HWND hwnd, const wchar_t* path, int pad) {
    RECT rc{};
    ::GetWindowRect(hwnd, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return false;
    const int W = w + pad * 2, H = h + pad * 2;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = W;
    bmi.bmiHeader.biHeight = -H;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    HDC screen = ::GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP dib = ::CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&bmi),
                                     DIB_RGB_COLORS, &bits, nullptr, 0);
    HDC dc = ::CreateCompatibleDC(screen);
    HGDIOBJ old = ::SelectObject(dc, dib);

    // 灰底，便于看清透明与投影
    RECT full{ 0, 0, W, H };
    HBRUSH bg = ::CreateSolidBrush(RGB(118, 124, 134));
    ::FillRect(dc, &full, bg);
    ::DeleteObject(bg);

    // PrintWindow 到目标 DC 的偏移位置
    ::SetViewportOrgEx(dc, pad, pad, nullptr);
    ::SetLastError(0);
    const BOOL pw = ::PrintWindow(hwnd, dc, 2 /*PW_RENDERFULLCONTENT*/);
    const DWORD err = ::GetLastError();
    ::SetViewportOrgEx(dc, 0, 0, nullptr);

    Say("    PrintWindow=%d err=%lu\n", pw ? 1 : 0, (unsigned long)err);

    // 统计非背景像素
    int nonBg = 0, white = 0, blue = 0;
    auto* px = static_cast<uint32_t*>(bits);
    for (int i = 0; i < W * H; ++i) {
        const uint32_t b = px[i] & 0xFF;
        const uint32_t gg = (px[i] >> 8) & 0xFF;
        const uint32_t rr = (px[i] >> 16) & 0xFF;
        // 背景是 (118,124,134)
        if (rr != 118 || gg != 124 || b != 134) ++nonBg;
        if (rr > 230 && gg > 230 && b > 230) ++white;
        if (b > 180 && rr < 120 && gg < 150) ++blue;
    }
    Say("    非背景像素 %d / %d (%.1f%%)\n", nonBg, W * H,
        100.0 * nonBg / (W * H));
    Say("    白色像素 %d   蓝色像素 %d\n", white, blue);

    ::SelectObject(dc, old);
    ::DeleteDC(dc);
    ::ReleaseDC(nullptr, screen);

    bool ok = false;
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
                    fr->SetSize(W, H);
                    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
                    fr->SetPixelFormat(&fmt);
                    if (SUCCEEDED(fr->WritePixels(H, W * 4, W * 4 * H,
                                                  static_cast<BYTE*>(bits))) &&
                        SUCCEEDED(fr->Commit())) {
                        ok = SUCCEEDED(enc->Commit());
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
    return ok;
}

int wmain() {
    g = _wfopen(L"D:\\LightTransAI\\button_result.txt", L"wb");
    if (!g) return 2;
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    lt::Logger::Instance().Init(L"D:\\LightTransAI\\button_probe.log");

    Say("======== 悬浮按钮外观验证 v2 ========\n\n");
    int pass = 0, fail = 0;
    auto check = [&](bool ok, const char* what) {
        if (ok) ++pass; else { fail++; Say("  [失败] %s\n", what); }
    };

    lt::FloatButtonWindow btn;
    btn.SetOnClick([](POINT) {});
    btn.SetOnExpired([]() {});
    check(btn.Show(POINT{ 500, 400 }), "按钮应能显示");
    Pump(700);

    HWND h = btn.Handle();
    if (!h) { if (g) std::fclose(g); return 1; }

    RECT rc{};
    ::GetWindowRect(h, &rc);
    Say("[1] 窗口 %dx%d @ (%d,%d)\n", rc.right - rc.left, rc.bottom - rc.top,
        rc.left, rc.top);

    Say("[2] PrintWindow 截图\n");
    const bool saved = ShotByPrintWindow(h, L"D:\\LightTransAI\\button_shot.png", 8);
    Say("[3] 保存 PNG: %s\n", saved ? "成功" : "失败");
    check(saved, "应能保存截图");

    // 另存一份按钮窗口区域的屏幕抓取（对照）
    {
        const int w = rc.right - rc.left, hh = rc.bottom - rc.top;
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
        ::BitBlt(dc, 0, 0, w, hh, screen, rc.left, rc.top, SRCCOPY | CAPTUREBLT);
        int nonBlack = 0;
        auto* px = static_cast<uint32_t*>(bits);
        for (int i = 0; i < w * hh; ++i) {
            if ((px[i] & 0xFFFFFF) != 0) ++nonBlack;
        }
        Say("[4] 屏幕 BitBlt 抓取: 非黑像素 %d / %d\n", nonBlack, w * hh);
        ::SelectObject(dc, old);
        ::DeleteDC(dc);
        ::DeleteObject(dib);
        ::ReleaseDC(nullptr, screen);
    }

    btn.Destroy();
    Pump(200);

    Say("\n========================================\n");
    Say("  通过: %d   失败: %d\n", pass, fail);
    Say("========================================\n");

    ::CoUninitialize();
    if (g) std::fclose(g);
    return fail == 0 ? 0 : 1;
}
