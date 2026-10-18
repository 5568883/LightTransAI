// 决定性实验 2：复刻 main.cpp 的初始化序列，找出哪一步导致标题栏消失
//
// 前面已排除：样式位、manifest、窗口类、owner、HWND_MESSAGE、子控件。
// main.cpp 里还有三步是复刻实验没做的：
//   1. SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)
//   2. CoInitializeEx
//   3. InitCommonControlsEx
// 本实验逐个加上，看哪个让边框高度变 0。
#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <shellscalingapi.h>
#include <cstdio>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shcore.lib")

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
            ::TranslateMessage(&m); ::DispatchMessageW(&m);
        }
        ::Sleep(5);
    }
}
static int FrameH(HWND h) {
    if (!h) return -1;
    RECT wr{}, cr{};
    ::GetWindowRect(h, &wr);
    ::GetClientRect(h, &cr);
    return (wr.bottom - wr.top) - (cr.bottom - cr.top);
}
static LRESULT CALLBACK WP(HWND h, UINT m, WPARAM w, LPARAM l) {
    return ::DefWindowProcW(h, m, w, l);
}

static HWND MakeWin(const wchar_t* cls) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WP;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = cls;
    wc.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
    ::RegisterClassExW(&wc);

    RECT work{};
    ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int x = work.left + ((work.right - work.left) - 460) / 2;
    const int y = work.top + ((work.bottom - work.top) - 400) / 2;

    HWND h = ::CreateWindowExW(WS_EX_APPWINDOW, cls, L"轻译 AI",
                               WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX,
                               x, y, 460, 400, nullptr, nullptr,
                               ::GetModuleHandleW(nullptr), nullptr);
    Pump(200);
    if (h) { ::ShowWindow(h, SW_SHOW); ::UpdateWindow(h); Pump(300); }
    return h;
}

int wmain() {
    g = _wfopen(L"D:\\LightTransAI\\init_result.txt", L"wb");
    if (!g) return 2;

    Say("======== 初始化序列影响实验 ========\n\n");

    // ---- 基准（什么都不做）----
    Say("[0] 基准（无任何初始化）\n");
    HWND h0 = MakeWin(L"InitCls0");
    Say("    边框高度 %d px  %s\n", FrameH(h0),
        FrameH(h0) > 20 ? "（有标题栏）" : "【无标题栏】");

    // ---- 加 CoInitializeEx ----
    Say("\n[1] + CoInitializeEx(APARTMENTTHREADED)\n");
    const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Say("    CoInitializeEx hr=0x%08lX\n", (unsigned long)hr);
    HWND h1 = MakeWin(L"InitCls1");
    Say("    边框高度 %d px  %s\n", FrameH(h1),
        FrameH(h1) > 20 ? "（有标题栏）" : "【无标题栏】");

    // ---- 加 InitCommonControlsEx ----
    Say("\n[2] + InitCommonControlsEx\n");
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES | ICC_BAR_CLASSES;
    const BOOL ic = ::InitCommonControlsEx(&icc);
    Say("    InitCommonControlsEx=%d\n", ic ? 1 : 0);
    HWND h2 = MakeWin(L"InitCls2");
    Say("    边框高度 %d px  %s\n", FrameH(h2),
        FrameH(h2) > 20 ? "（有标题栏）" : "【无标题栏】");

    // ---- 加 SetProcessDpiAwarenessContext(-4) ----
    Say("\n[3] + SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)\n");
    BOOL dpiOk = FALSE;
    if (HMODULE u = ::GetModuleHandleW(L"user32.dll")) {
        using PFN = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
        auto pfn = reinterpret_cast<PFN>(::GetProcAddress(u, "SetProcessDpiAwarenessContext"));
        if (pfn) {
            dpiOk = pfn(reinterpret_cast<DPI_AWARENESS_CONTEXT>(-4));
        }
    }
    Say("    SetProcessDpiAwarenessContext 返回 %d  err=%lu\n",
        dpiOk ? 1 : 0, (unsigned long)::GetLastError());
    HWND h3 = MakeWin(L"InitCls3");
    Say("    边框高度 %d px  %s\n", FrameH(h3),
        FrameH(h3) > 20 ? "（有标题栏）" : "【无标题栏】");

    // ---- 报告最终 DPI 上下文 ----
    Say("\n[4] 当前 DPI 感知上下文\n");
    if (HMODULE u = ::GetModuleHandleW(L"user32.dll")) {
        using PFN = DPI_AWARENESS_CONTEXT(WINAPI*)(void);
        auto pfn = reinterpret_cast<PFN>(::GetProcAddress(u, "GetThreadDpiAwarenessContext"));
        if (pfn) {
            DPI_AWARENESS_CONTEXT ctx = pfn();
            using PFN2 = int(WINAPI*)(DPI_AWARENESS_CONTEXT);
            auto pfn2 = reinterpret_cast<PFN2>(::GetProcAddress(u, "GetAwarenessFromDpiAwarenessContext"));
            const int aw = pfn2 ? pfn2(ctx) : -1;
            Say("    context 原始值=%lld   awareness=%d\n",
                (long long)reinterpret_cast<INT_PTR>(ctx), aw);
            Say("    (0=Unaware 1=System 2=PerMonitor)\n");
        }
    }

    // ---- 测量点检查 ----
    Say("\n[5] GetSystemMetrics\n");
    Say("    SM_CYCAPTION = %d\n", ::GetSystemMetrics(SM_CYCAPTION));
    Say("    SM_CYSIZEFRAME = %d\n", ::GetSystemMetrics(SM_CYSIZEFRAME));
    Say("    SM_CXPADDEDBORDER = %d\n", ::GetSystemMetrics(SM_CXPADDEDBORDER));

    Say("\n======== 判读 ========\n");
    Say("若某步之后边框高度变 0，那一步就是根因。\n");

    if (h0) ::DestroyWindow(h0);
    if (h1) ::DestroyWindow(h1);
    if (h2) ::DestroyWindow(h2);
    if (h3) ::DestroyWindow(h3);
    if (SUCCEEDED(hr)) ::CoUninitialize();
    if (g) std::fclose(g);
    return 0;
}
