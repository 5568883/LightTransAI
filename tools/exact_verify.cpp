// 决定性实验：逐字复制 MainWindow 的创建流程，找出标题栏消失的原因
//
// 前面已排除：样式位、DPI、manifest、窗口类、owner、DWM 调用。
// 本实验把 MainWindow::Show() 的代码**原样**搬过来，再逐项回退，
// 定位是哪一步导致边框高度变 0。
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

static int FrameH(HWND h) {
    RECT wr{}, cr{};
    ::GetWindowRect(h, &wr);
    ::GetClientRect(h, &cr);
    return (wr.bottom - wr.top) - (cr.bottom - cr.top);
}

static LRESULT CALLBACK WP(HWND h, UINT m, WPARAM w, LPARAM l) {
    return ::DefWindowProcW(h, m, w, l);
}

// 逐字复刻 MainWindow 的创建过程
static HWND MakeLikeMainWindow(const wchar_t* clsName, bool withSetForeground) {
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WP;
    wc.hInstance     = ::GetModuleHandleW(nullptr);
    wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = clsName;
    wc.hIcon         = ::LoadIconW(nullptr, IDI_APPLICATION);
    ::RegisterClassExW(&wc);

    RECT work{};
    ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int kWinW = 460, kWinH = 400;
    const int x = work.left + ((work.right - work.left) - kWinW) / 2;
    const int y = work.top + ((work.bottom - work.top) - kWinH) / 2;

    const DWORD style = (WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX);
    HWND h = ::CreateWindowExW(WS_EX_APPWINDOW, clsName, L"轻译 AI",
                               style, x, y, kWinW, kWinH,
                               nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    if (!h) return nullptr;

    // 创建若干子控件（复刻 CreateControls）
    ::CreateWindowExW(0, L"STATIC", L"轻译 AI", WS_CHILD | WS_VISIBLE | SS_LEFT,
                      22, 20, 416, 26, h, nullptr,
                      ::GetModuleHandleW(nullptr), nullptr);
    ::CreateWindowExW(0, L"BUTTON", L"打开设置",
                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                      22, 88, 150, 32, h, nullptr,
                      ::GetModuleHandleW(nullptr), nullptr);

    ::ShowWindow(h, SW_SHOW);
    ::UpdateWindow(h);
    if (withSetForeground) {
        ::SetForegroundWindow(h);
    }
    return h;
}

int wmain() {
    g = _wfopen(L"D:\\LightTransAI\\exact_result.txt", L"wb");
    if (!g) return 2;

    Say("======== 决定性实验：复刻 MainWindow ========\n\n");

    // 实验 1：完全复刻（含 SetForegroundWindow）
    HWND h1 = MakeLikeMainWindow(L"ExactCls1", true);
    Pump(400);
    Say("[1] 完全复刻（含 SetForegroundWindow）\n");
    Say("    窗口 %d x %d\n",
        ([](HWND h) { RECT r{}; ::GetWindowRect(h, &r); return r.right - r.left; })(h1),
        ([](HWND h) { RECT r{}; ::GetWindowRect(h, &r); return r.bottom - r.top; })(h1));
    Say("    边框高度 %d px  %s\n", FrameH(h1),
        FrameH(h1) > 20 ? "（有标题栏）" : "【无标题栏】");

    // 实验 2：不含 SetForegroundWindow
    HWND h2 = MakeLikeMainWindow(L"ExactCls2", false);
    Pump(400);
    Say("\n[2] 不含 SetForegroundWindow\n");
    Say("    边框高度 %d px  %s\n", FrameH(h2),
        FrameH(h2) > 20 ? "（有标题栏）" : "【无标题栏】");

    // 实验 3：不含任何子控件
    {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = WP;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = L"ExactCls3";
        ::RegisterClassExW(&wc);
        HWND h3 = ::CreateWindowExW(WS_EX_APPWINDOW, L"ExactCls3", L"轻译 AI",
                                    WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX,
                                    200, 200, 460, 400, nullptr, nullptr,
                                    ::GetModuleHandleW(nullptr), nullptr);
        Pump(300);
        ::ShowWindow(h3, SW_SHOW);
        Pump(300);
        Say("\n[3] 无子控件\n");
        Say("    边框高度 %d px  %s\n", FrameH(h3),
            FrameH(h3) > 20 ? "（有标题栏）" : "【无标题栏】");
        if (h3) ::DestroyWindow(h3);
    }

    // 实验 4：检查 AdjustWindowRectEx 在当前进程返回什么
    {
        RECT r{ 0, 0, 460, 400 };
        ::AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX, FALSE,
                             WS_EX_APPWINDOW);
        Say("\n[4] AdjustWindowRectEx(460x400) -> %dx%d  边框应占 %d px\n",
            r.right - r.left, r.bottom - r.top,
            (r.bottom - r.top) - 400);
    }

    Say("\n======== 判读 ========\n");
    Say("若 [1][2][3] 都有标题栏，说明复刻代码没问题，\n");
    Say("则 MainWindow 的差异在其它地方（需继续查）。\n");
    Say("若某个实验无标题栏，则对应那一步就是原因。\n");

    if (h1) ::DestroyWindow(h1);
    if (h2) ::DestroyWindow(h2);
    if (g) std::fclose(g);
    return 0;
}
