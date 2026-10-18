// 端到端取词验证（在正常完整性环境下运行）
//
// 本程序在新位置（D:\LightTransAI）编译，具备正常的中完整性，
// 因此 SendInput 不会被 UIPI 拦截，可以真实验证：
//   UIA 读取 / Ctrl+C 注入 → 剪贴板 → 读取 → 还原
#include <windows.h>
#include <cstdio>
#include <string>

#include "input/text_extractor.h"
#include "utils/logger.h"
#include "utils/string_utils.h"

static FILE* g_log = nullptr;
static void Say(const char* fmt, ...) {
    if (!g_log) return;
    va_list ap; va_start(ap, fmt);
    std::vfprintf(g_log, fmt, ap);
    va_end(ap); std::fflush(g_log);
}
static void SayW(const char* tag, const std::wstring& w) {
    Say("%s: %s\n", tag, lt::str::ToUtf8(w).c_str());
}

static HWND g_edit = nullptr;
static HWND g_main = nullptr;

const wchar_t* kSample = L"This download installs Intel Graphics Driver WHQL Certified.";

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CREATE) {
        g_edit = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", kSample,
                                   WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                                   ES_MULTILINE | ES_AUTOVSCROLL,
                                   10, 10, 520, 150, h, nullptr,
                                   ::GetModuleHandleW(nullptr), nullptr);
        return 0;
    }
    if (m == WM_DESTROY) { ::PostQuitMessage(0); return 0; }
    return ::DefWindowProcW(h, m, w, l);
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

static std::wstring GetClip() {
    std::wstring out;
    for (int i = 0; i < 10; ++i) {
        if (::OpenClipboard(nullptr)) {
            if (HANDLE h = ::GetClipboardData(CF_UNICODETEXT)) {
                if (auto* p = static_cast<const wchar_t*>(::GlobalLock(h))) {
                    out = p; ::GlobalUnlock(h);
                }
            }
            ::CloseClipboard();
            break;
        }
        ::Sleep(10);
    }
    return out;
}

static void SetClip(const std::wstring& s) {
    for (int i = 0; i < 10; ++i) {
        if (::OpenClipboard(nullptr)) {
            ::EmptyClipboard();
            const size_t bytes = (s.size() + 1) * sizeof(wchar_t);
            if (HGLOBAL h = ::GlobalAlloc(GMEM_MOVEABLE, bytes)) {
                if (void* p = ::GlobalLock(h)) {
                    memcpy(p, s.c_str(), bytes); ::GlobalUnlock(h);
                    ::SetClipboardData(CF_UNICODETEXT, h);
                }
            }
            ::CloseClipboard();
            break;
        }
        ::Sleep(10);
    }
}

int wmain() {
    g_log = _wfopen(L"D:\\LightTransAI\\e2e_result.txt", L"wb");
    if (!g_log) return 2;
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    lt::Logger::Instance().Init(L"D:\\LightTransAI\\e2e_probe.log");

    Say("======== 端到端取词验证（正常完整性）========\n\n");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = L"E2EHostCls2";
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    ::RegisterClassExW(&wc);

    g_main = ::CreateWindowExW(0, wc.lpszClassName, L"E2E 取词测试",
                               WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                               80, 80, 560, 240, nullptr, nullptr,
                               wc.hInstance, nullptr);
    if (!g_main) { Say("窗口创建失败\n"); return 1; }
    Pump(400);
    Say("[1] 宿主窗口已创建\n");

    const std::wstring kOrig = L"__ORIGINAL_CLIP__";
    SetClip(kOrig);
    Pump(100);
    SayW("[2] 剪贴板初始内容", GetClip());

    // ---- 通道 A：UIA ----
    Say("\n--- 通道 A：UIA ---\n");
    ::SetForegroundWindow(g_main);
    ::SetFocus(g_edit);
    ::SendMessageW(g_edit, EM_SETSEL, 0, -1);
    Pump(200);
    {
        std::wstring t;
        const DWORD t0 = ::GetTickCount();
        const bool ok = lt::TextExtractor::TryGetViaUia(t, 800);
        Say("  结果=%s  耗时 %lu ms\n", ok ? "成功" : "失败",
            (unsigned long)(::GetTickCount() - t0));
        if (ok) SayW("  文本", t);
    }

    // ---- 通道 B：剪贴板 ----
    Say("\n--- 通道 B：剪贴板借用 ---\n");
    ::SetForegroundWindow(g_main);
    ::SetFocus(g_edit);
    ::SendMessageW(g_edit, EM_SETSEL, 0, -1);
    Pump(150);
    {
        std::wstring t, err;
        const DWORD t0 = ::GetTickCount();
        const bool ok = lt::TextExtractor::TryGetViaClipboard(t, err);
        Say("  结果=%s  耗时 %lu ms\n", ok ? "成功" : "失败",
            (unsigned long)(::GetTickCount() - t0));
        if (ok) SayW("  取到文本", t);
        else    SayW("  错误", err);
        const std::wstring after = GetClip();
        SayW("  还原后剪贴板", after);
        Say("  还原成功: %s\n", (after == kOrig) ? "是" : "否");
    }

    // ---- 完整链路 ----
    Say("\n--- 完整链路 GetSelectedText() ---\n");
    ::SetForegroundWindow(g_main);
    ::SetFocus(g_edit);
    ::SendMessageW(g_edit, EM_SETSEL, 0, -1);
    Pump(150);
    {
        const DWORD t0 = ::GetTickCount();
        const lt::SelectionText sel = lt::TextExtractor::GetSelectedText();
        Say("  耗时 %lu ms\n", (unsigned long)(::GetTickCount() - t0));
        Say("  found=%d  source=%s\n", sel.found ? 1 : 0,
            lt::str::ToUtf8(sel.source).c_str());
        if (sel.found) {
            SayW("  文本", sel.text);
            Say("  与预期一致: %s\n", (sel.text == kSample) ? "是" : "否");
        } else {
            SayW("  错误", sel.error);
        }
    }

    Say("\n======== 完成 ========\n");
    if (g_main) ::DestroyWindow(g_main);
    ::CoUninitialize();
    if (g_log) std::fclose(g_log);
    return 0;
}
