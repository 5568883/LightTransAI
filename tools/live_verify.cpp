// 对**运行中的真实程序**做窗口测量
//
// 之前的测试都在自己的进程里创建窗口，可能与真实程序不同。
// 本工具附加到真实 LightTransAI 进程，枚举它的窗口并测量边框。
#include <windows.h>
#include <cstdio>
#include <string>

static FILE* g = nullptr;
static void Say(const char* fmt, ...) {
    if (!g) return;
    va_list ap; va_start(ap, fmt);
    std::vfprintf(g, fmt, ap);
    va_end(ap); std::fflush(g);
}

struct Ctx { DWORD pid; int count; };

static BOOL CALLBACK EnumProc(HWND h, LPARAM lp) {
    auto* c = reinterpret_cast<Ctx*>(lp);
    DWORD pid = 0;
    ::GetWindowThreadProcessId(h, &pid);
    if (pid != c->pid) return TRUE;

    wchar_t cls[256]{}, title[256]{};
    ::GetClassNameW(h, cls, 255);
    ::GetWindowTextW(h, title, 255);

    RECT wr{}, cr{};
    ::GetWindowRect(h, &wr);
    ::GetClientRect(h, &cr);
    const int frameH = (wr.bottom - wr.top) - (cr.bottom - cr.top);
    const LONG_PTR st = ::GetWindowLongPtrW(h, GWL_STYLE);

    Say("  [%d] class=%ls\n", c->count, cls);
    Say("      标题     : %ls\n", title);
    Say("      可见     : %d   顶层: %d\n",
        ::IsWindowVisible(h) ? 1 : 0,
        (::GetWindow(h, GW_OWNER) == nullptr && (st & WS_CHILD) == 0) ? 1 : 0);
    Say("      STYLE    : 0x%08lX\n", (unsigned long)st);
    Say("      窗口 %dx%d   客户区 %dx%d\n",
        wr.right - wr.left, wr.bottom - wr.top, cr.right, cr.bottom);
    Say("      边框高度 : %d px  %s\n", frameH,
        frameH > 20 ? "（有标题栏）" : "【无标题栏】");

    // 检查窗口区域是否被裁剪
    HRGN rgn = ::CreateRectRgn(0, 0, 0, 0);
    const int rgnType = ::GetWindowRgn(h, rgn);
    if (rgnType != ERROR) {
        RECT rr{};
        ::GetRgnBox(rgn, &rr);
        Say("      窗口区域 : 类型=%d  范围 %dx%d\n", rgnType,
            rr.right - rr.left, rr.bottom - rr.top);
    } else {
        Say("      窗口区域 : 未设置（正常）\n");
    }
    ::DeleteObject(rgn);

    ++c->count;
    return TRUE;
}

int wmain(int argc, wchar_t** argv) {
    g = _wfopen(L"D:\\LightTransAI\\live_result.txt", L"wb");
    if (!g) return 2;

    DWORD pid = (argc > 1) ? static_cast<DWORD>(_wtoi(argv[1])) : 0;
    if (pid == 0) {
        // 自动找 LightTransAI 进程
        HWND probe = ::FindWindowW(L"LightTransAI_Main", nullptr);
        if (probe) {
            ::GetWindowThreadProcessId(probe, &pid);
        }
    }
    Say("======== 运行中程序窗口测量 ========\n");
    Say("目标 PID: %lu\n\n", (unsigned long)pid);
    if (pid == 0) { Say("未找到目标进程\n"); if (g) std::fclose(g); return 1; }

    Ctx c{ pid, 0 };
    ::EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&c));
    Say("\n共 %d 个窗口\n", c.count);

    if (g) std::fclose(g);
    return 0;
}
