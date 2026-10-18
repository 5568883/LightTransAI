// 最小化隔离实验：为什么窗口没有标题栏？
//
// 创建多个对照窗口，逐一测量"边框高度"（= 窗口高 - 客户区高）：
//   A. 系统默认类（STATIC）+ WS_OVERLAPPEDWINDOW
//   B. 自建类（不设 hbrBackground）+ WS_OVERLAPPEDWINDOW
//   C. 自建类（设 hbrBackground）+ WS_OVERLAPPEDWINDOW
//   D. 自建类 + WS_OVERLAPPEDWINDOW，且调用 SetWindowPos 后测量
//
// 有标题栏时边框高度应 > 20px（标题栏约 30px + 边框）。
#include <windows.h>
#include <cstdio>

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

static int FrameHeight(HWND h) {
    RECT wr{}, cr{};
    ::GetWindowRect(h, &wr);
    ::GetClientRect(h, &cr);
    return (wr.bottom - wr.top) - (cr.bottom - cr.top);
}

static LRESULT CALLBACK WP(HWND h, UINT m, WPARAM w, LPARAM l) {
    return ::DefWindowProcW(h, m, w, l);
}

static void Report(const char* tag, HWND h) {
    if (!h) { Say("  %s: 创建失败\n", tag); return; }
    const LONG_PTR st = ::GetWindowLongPtrW(h, GWL_STYLE);
    Say("  %s\n", tag);
    Say("    STYLE      : 0x%08lX\n", (unsigned long)st);
    Say("    边框高度   : %d px  %s\n", FrameHeight(h),
        FrameHeight(h) > 20 ? "（有标题栏）" : "【无标题栏】");
    RECT wr{}, cr{};
    ::GetWindowRect(h, &wr);
    ::GetClientRect(h, &cr);
    Say("    窗口 %dx%d   客户区 %dx%d\n",
        wr.right - wr.left, wr.bottom - wr.top, cr.right, cr.bottom);
}

int wmain() {
    g = _wfopen(L"D:\\LightTransAI\\isolate_result.txt", L"wb");
    if (!g) return 2;

    Say("======== 标题栏隔离实验 ========\n");
    Say("进程 DPI 感知: ");
    // 报告 DPI 感知状态
    {
        using PFN = DPI_AWARENESS_CONTEXT(WINAPI*)(void);
        auto pfn = reinterpret_cast<PFN>(
            ::GetProcAddress(::GetModuleHandleW(L"user32.dll"),
                             "GetThreadDpiAwarenessContext"));
        if (pfn) {
            DPI_AWARENESS_CONTEXT ctx = pfn();
            // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = -4
            const INT_PTR v = reinterpret_cast<INT_PTR>(ctx);
            Say("context=%lld\n", (long long)v);
        } else {
            Say("无法查询\n");
        }
    }
    Say("\n");

    // ---- A: 系统默认类 ----
    HWND a = ::CreateWindowExW(WS_EX_APPWINDOW, L"STATIC", L"A 默认类",
                               WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX,
                               100, 100, 400, 300, nullptr, nullptr,
                               ::GetModuleHandleW(nullptr), nullptr);
    Pump(300);
    ::ShowWindow(a, SW_SHOW);
    Pump(300);
    Report("A. 系统 STATIC 类 + WS_OVERLAPPEDWINDOW", a);

    // ---- B: 自建类，不设背景刷 ----
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WP;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"IsolateClsNoBg";
    wc.hbrBackground = nullptr;
    ::RegisterClassExW(&wc);

    HWND b = ::CreateWindowExW(WS_EX_APPWINDOW, L"IsolateClsNoBg", L"B 自建类",
                               WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX,
                               150, 150, 400, 300, nullptr, nullptr,
                               wc.hInstance, nullptr);
    Pump(300);
    ::ShowWindow(b, SW_SHOW);
    Pump(300);
    Report("B. 自建类（无背景刷）", b);

    // ---- C: 自建类，设背景刷 ----
    wc.lpszClassName = L"IsolateClsWithBg";
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    ::RegisterClassExW(&wc);

    HWND c = ::CreateWindowExW(WS_EX_APPWINDOW, L"IsolateClsWithBg", L"C 自建类+背景",
                               WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX,
                               200, 200, 400, 300, nullptr, nullptr,
                               wc.hInstance, nullptr);
    Pump(300);
    ::ShowWindow(c, SW_SHOW);
    Pump(300);
    Report("C. 自建类（有背景刷）", c);

    // ---- D: 用 AdjustWindowRectEx 计算后再创建 ----
    RECT adj{ 0, 0, 400, 300 };
    ::AdjustWindowRectEx(&adj, WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX, FALSE,
                         WS_EX_APPWINDOW);
    Say("\n  AdjustWindowRectEx 结果: 窗口应为 %dx%d（客户区 400x300）\n",
        adj.right - adj.left, adj.bottom - adj.top);

    HWND d = ::CreateWindowExW(WS_EX_APPWINDOW, L"IsolateClsWithBg", L"D 调整后",
                               WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX,
                               250, 250, adj.right - adj.left, adj.bottom - adj.top,
                               nullptr, nullptr, wc.hInstance, nullptr);
    Pump(300);
    ::ShowWindow(d, SW_SHOW);
    Pump(300);
    Report("D. 自建类（AdjustWindowRectEx 后）", d);

    Say("\n======== 结论 ========\n");
    Say("若 A 有标题栏而 B/C/D 无 → 与窗口类注册方式有关\n");
    Say("若全部无标题栏 → 系统/主题层面的原因，非代码问题\n");

    if (a) ::DestroyWindow(a);
    if (b) ::DestroyWindow(b);
    if (c) ::DestroyWindow(c);
    if (d) ::DestroyWindow(d);
    if (g) std::fclose(g);
    return 0;
}
