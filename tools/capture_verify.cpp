// 框选遮罩交互验证
//
// 自动模拟一次真实的框选：Begin() 后程序化发送鼠标按下/移动/松开，
// 检查回调是否收到正确的选区与裁图。
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

int wmain() {
    g = _wfopen(L"D:\\LightTransAI\\capture_result.txt", L"wb");
    if (!g) return 2;
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    lt::Logger::Instance().Init(L"D:\\LightTransAI\\capture_probe.log");

    Say("======== 框选遮罩交互验证 ========\n\n");

    int pass = 0, fail = 0;
    auto check = [&](bool ok, const char* what) {
        if (ok) ++pass; else { ++fail; Say("  [失败] %s\n", what); }
    };

    lt::CaptureSelection got{};
    bool called = false;

    lt::ScreenCapturer cap;
    const bool begun = cap.Begin([&](const lt::CaptureSelection& sel) {
        got = sel;
        called = true;
    });
    Say("[1] Begin: %s\n", begun ? "成功" : "失败");
    check(begun, "Begin 应成功");

    if (!begun) {
        Say("\n无法继续\n");
        if (g) std::fclose(g);
        return 1;
    }

    Pump(300);
    Say("[2] 遮罩已显示，IsActive=%d\n", cap.IsActive() ? 1 : 0);
    check(cap.IsActive(), "遮罩应处于活动状态");

    // 找到遮罩窗口
    HWND overlay = ::FindWindowW(L"LightTransAI_CaptureOverlay", nullptr);
    Say("[3] 遮罩窗口: %p\n", (void*)overlay);
    check(overlay != nullptr, "应能找到遮罩窗口");
    if (!overlay) { cap.Abort(); if (g) std::fclose(g); return 1; }

    // 屏幕坐标需转成客户区坐标（遮罩覆盖整个虚拟屏幕，偏移即虚拟屏幕原点）
    const int vx = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = ::GetSystemMetrics(SM_YVIRTUALSCREEN);

    // 诊断：确认窗口属于本进程、USERDATA 已设置、位置与预期一致
    {
        DWORD pid = 0;
        ::GetWindowThreadProcessId(overlay, &pid);
        Say("[3a] 窗口 PID=%lu (本进程=%lu) 匹配=%d\n",
            (unsigned long)pid, (unsigned long)::GetCurrentProcessId(),
            pid == ::GetCurrentProcessId() ? 1 : 0);
        const LONG_PTR ud = ::GetWindowLongPtrW(overlay, GWLP_USERDATA);
        Say("[3b] GWLP_USERDATA=%p (应为非 0)\n", (void*)ud);
        RECT rc{};
        ::GetWindowRect(overlay, &rc);
        Say("[3c] 窗口位置 (%d,%d)-(%d,%d)\n", rc.left, rc.top, rc.right, rc.bottom);
        Say("[3d] 虚拟屏幕原点 (%d,%d)\n", vx, vy);
    }

    // 检查扩展样式
    const LONG_PTR ex = ::GetWindowLongPtrW(overlay, GWL_EXSTYLE);
    Say("[4] 扩展样式: 0x%08lX\n", (unsigned long)ex);
    check((ex & WS_EX_TOPMOST) != 0, "应带 WS_EX_TOPMOST");
    check((ex & WS_EX_NOACTIVATE) != 0, "应带 WS_EX_NOACTIVATE（不抢焦点）");
    check((ex & WS_EX_TOOLWINDOW) != 0, "应带 WS_EX_TOOLWINDOW");

    // 检查光标是否为十字
    ::SetCursor(::LoadCursorW(nullptr, IDC_CROSS));

    // ---- 模拟拖拽：从 (200,200) 拖到 (600,500) ----
    const int x0 = 200, y0 = 200, x1 = 600, y1 = 500;
    Say("[5] 模拟拖拽 (%d,%d) -> (%d,%d)\n", x0, y0, x1, y1);

    auto sendMouse = [&](UINT msg, int sx, int sy) {
        LPARAM lp = MAKELPARAM(sx - vx, sy - vy);
        ::SendMessageW(overlay, msg, MK_LBUTTON, lp);
    };

    sendMouse(WM_LBUTTONDOWN, x0, y0);
    Pump(80);
    for (int i = 1; i <= 4; ++i) {
        const int mx = x0 + (x1 - x0) * i / 4;
        const int my = y0 + (y1 - y0) * i / 4;
        sendMouse(WM_MOUSEMOVE, mx, my);
        Pump(50);
    }
    // 松开时不带 MK_LBUTTON
    ::SendMessageW(overlay, WM_LBUTTONUP, 0, MAKELPARAM(x1 - vx, y1 - vy));
    Pump(400);

    Say("[6] 回调被调用: %d\n", called ? 1 : 0);
    check(called, "应触发完成回调");

    if (called) {
        Say("[7] accepted=%d  cancelled=%d\n", got.accepted ? 1 : 0,
            got.cancelled ? 1 : 0);
        check(got.accepted, "应被接受");

        const int w = got.screen_rect.right - got.screen_rect.left;
        const int h = got.screen_rect.bottom - got.screen_rect.top;
        Say("[8] 选区 %d x %d  @ (%d,%d)\n", w, h,
            got.screen_rect.left, got.screen_rect.top);
        check(w == 400 && h == 300, "选区尺寸应为 400x300");
        check(got.screen_rect.left == 200 && got.screen_rect.top == 200,
              "选区起点应为 (200,200)");

        Say("[9] 冻结裁图: %dx%d  有效=%d\n",
            got.cropped.width, got.cropped.height, got.cropped.Valid() ? 1 : 0);
        check(got.cropped.Valid(), "应带回冻结裁图");
        check(got.cropped.width == 400 && got.cropped.height == 300,
              "裁图尺寸应与选区一致");

        Say("[10] DPI: %u (缩放 %.2f)\n", got.monitor_dpi, got.dpi_scale);
        check(got.monitor_dpi >= 96, "DPI 应不低于 96");

        // 校验裁图内容非全黑（说明真的截到了画面）
        if (got.cropped.Valid()) {
            long sum = 0;
            const size_t n = got.cropped.pixels.size();
            for (size_t i = 0; i < n; i += 400) sum += got.cropped.pixels[i];
            const double avg = static_cast<double>(sum) / (n / 400);
            Say("[11] 裁图平均亮度: %.1f\n", avg);
            check(avg > 1.0, "裁图不应为全黑（说明截到了真实画面）");
        }
    }

    Say("\n[12] 遮罩已销毁: %d\n", cap.IsActive() ? 0 : 1);
    check(!cap.IsActive(), "完成后遮罩应已销毁");

    // ---- 测试误触取消：拖 5x5 ----
    Say("\n--- 误触测试：拖 5x5 像素 ---\n");
    called = false;
    got = lt::CaptureSelection{};
    const bool b2 = cap.Begin([&](const lt::CaptureSelection& sel) {
        got = sel; called = true;
    });
    check(b2, "第二次 Begin 应成功");
    if (b2) {
        Pump(250);
        HWND ov2 = ::FindWindowW(L"LightTransAI_CaptureOverlay", nullptr);
        if (ov2) {
            // 必须完整发 DOWN -> MOVE -> UP，否则 dragging_ 不会被置位，
            // WM_LBUTTONUP 会被直接忽略（实测踩过这个测试脚本缺陷）。
            ::SendMessageW(ov2, WM_LBUTTONDOWN, MK_LBUTTON,
                           MAKELPARAM(300 - vx, 300 - vy));
            Pump(60);
            ::SendMessageW(ov2, WM_MOUSEMOVE, MK_LBUTTON,
                           MAKELPARAM(303 - vx, 303 - vy));
            Pump(40);
            ::SendMessageW(ov2, WM_LBUTTONUP, 0, MAKELPARAM(305 - vx, 305 - vy));
            Pump(350);
        }
        Say("  回调=%d  accepted=%d (应为 0)\n", called ? 1 : 0, got.accepted ? 1 : 0);
        check(called && !got.accepted, "过小选区应被判定为误触并取消");
    }

    // ---- 测试右键取消 ----
    Say("\n--- 右键取消测试 ---\n");
    called = false;
    got = lt::CaptureSelection{};
    const bool b3 = cap.Begin([&](const lt::CaptureSelection& sel) {
        got = sel; called = true;
    });
    check(b3, "第三次 Begin 应成功");
    if (b3) {
        Pump(250);
        HWND ov3 = ::FindWindowW(L"LightTransAI_CaptureOverlay", nullptr);
        if (ov3) {
            ::SendMessageW(ov3, WM_RBUTTONDOWN, 0, MAKELPARAM(400 - vx, 400 - vy));
            Pump(350);
        }
        Say("  回调=%d  cancelled=%d (应为 1)\n", called ? 1 : 0, got.cancelled ? 1 : 0);
        check(called && !got.accepted, "右键应取消框选");
    }

    Say("\n========================================\n");
    Say("  通过: %d   失败: %d\n", pass, fail);
    Say("========================================\n");

    ::CoUninitialize();
    if (g) std::fclose(g);
    return fail == 0 ? 0 : 1;
}
