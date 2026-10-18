// 验证假设：先创建 HWND_MESSAGE 宿主窗口，是否导致后续普通窗口失去标题栏？
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
            ::TranslateMessage(&m); ::DispatchMessageW(&m);
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

static void RegCls(const wchar_t* n) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WP;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = n;
    ::RegisterClassExW(&wc);
}

static HWND MakeNormal(const wchar_t* cls, const wchar_t* title) {
    HWND h = ::CreateWindowExW(WS_EX_APPWINDOW, cls, title,
                               WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX,
                               100, 100, 460, 400, nullptr, nullptr,
                               ::GetModuleHandleW(nullptr), nullptr);
    Pump(200);
    if (h) { ::ShowWindow(h, SW_SHOW); Pump(300); }
    return h;
}

int wmain() {
    g = _wfopen(L"D:\\LightTransAI\\msgonly_result.txt", L"wb");
    if (!g) return 2;

    Say("======== HWND_MESSAGE 影响验证 ========\n\n");

    RegCls(L"MsgTestNormal");
    RegCls(L"MsgTestHost");

    // 基准：先建普通窗口
    Say("[1] 基准：直接创建普通窗口\n");
    HWND h1 = MakeNormal(L"MsgTestNormal", L"基准窗口");
    Say("    边框高度 %d px  %s\n", FrameH(h1),
        FrameH(h1) > 20 ? "（有标题栏）" : "【无标题栏】");

    // 关键实验：先创建 HWND_MESSAGE 宿主，再建普通窗口
    Say("\n[2] 先创建 HWND_MESSAGE 宿主窗口\n");
    HWND host = ::CreateWindowExW(0, L"MsgTestHost", L"宿主", 0,
                                  0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                  ::GetModuleHandleW(nullptr), nullptr);
    Say("    宿主窗口: %p\n", (void*)host);
    Pump(200);

    HWND h2 = MakeNormal(L"MsgTestNormal", L"宿主后创建的窗口");
    Say("    边框高度 %d px  %s\n", FrameH(h2),
        FrameH(h2) > 20 ? "（有标题栏）" : "【无标题栏】");

    // 对照：再建一个
    HWND h3 = MakeNormal(L"MsgTestNormal", L"第三个窗口");
    Say("    第三个窗口边框 %d px  %s\n", FrameH(h3),
        FrameH(h3) > 20 ? "（有标题栏）" : "【无标题栏】");

    Say("\n======== 判读 ========\n");
    if (FrameH(h1) > 20 && FrameH(h2) <= 20) {
        Say("确认：HWND_MESSAGE 宿主窗口导致后续窗口失去标题栏！\n");
    } else if (FrameH(h1) > 20 && FrameH(h2) > 20) {
        Say("HWND_MESSAGE 无影响，需继续排查。\n");
    } else {
        Say("基准窗口也无标题栏，说明是环境问题。\n");
    }

    if (h1) ::DestroyWindow(h1);
    if (h2) ::DestroyWindow(h2);
    if (h3) ::DestroyWindow(h3);
    if (host) ::DestroyWindow(host);
    if (g) std::fclose(g);
    return 0;
}
