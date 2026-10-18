#include "ui/main_window.h"

#include <commctrl.h>

#pragma comment(lib, "comctl32.lib")

#include "app/app_messages.h"
#include "ui/ui_common.h"
#include "utils/logger.h"
#include "utils/perf_monitor.h"
#include "utils/string_utils.h"

namespace lt {

namespace {

// 控件 ID
enum : int {
    IDC_TITLE     = 2001,
    IDC_SUBTITLE  = 2002,
    IDC_CHECK_FLOAT = 2010,
    IDC_BTN_SETTINGS = 2011,
    IDC_BTN_FEATURE2 = 2012,
    IDC_BTN_FEATURE3 = 2013,
    IDC_STATIC_MEM   = 2014,
    IDC_STATIC_STATUS = 2015,
    IDC_BTN_QUIT     = 2016,
};

constexpr int kWinW = 460;
constexpr int kWinH = 400;

HFONT MakeFont(int pt, bool bold) {
    HDC screen = ::GetDC(nullptr);
    const int dpiY = (screen != nullptr) ? ::GetDeviceCaps(screen, LOGPIXELSY) : 96;
    if (screen != nullptr) ::ReleaseDC(nullptr, screen);

    return ::CreateFontW(-::MulDiv(pt, dpiY > 0 ? dpiY : 96, 72),
                         0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL,
                         FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
}

HWND MakeLabel(HWND parent, int id, const wchar_t* text, HFONT font) {
    HWND h = ::CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
                               0, 0, 10, 10, parent,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                               ::GetModuleHandleW(nullptr), nullptr);
    if (font) ::SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return h;
}

HWND MakeButton(HWND parent, int id, const wchar_t* text, HFONT font) {
    HWND h = ::CreateWindowExW(0, L"BUTTON", text,
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                               0, 0, 10, 10, parent,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                               ::GetModuleHandleW(nullptr), nullptr);
    if (font) ::SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return h;
}

HWND MakeCheck(HWND parent, int id, const wchar_t* text, HFONT font) {
    HWND h = ::CreateWindowExW(0, L"BUTTON", text,
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                               0, 0, 10, 10, parent,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                               ::GetModuleHandleW(nullptr), nullptr);
    if (font) ::SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return h;
}

} // namespace

bool MainWindow::EnsureClassRegistered() {
    static bool registered = false;
    if (registered) return true;

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = &MainWindow::WndProc;
    wc.hInstance     = ::GetModuleHandleW(nullptr);
    wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"LightTransAI_Main";
    // 大图标用于 Alt+Tab 与任务栏，小图标用于标题栏左上角。
    // 资源缺失时退回系统默认图标，不至于出现空白。
    wc.hIcon         = ::LoadIconW(::GetModuleHandleW(nullptr),
                                   MAKEINTRESOURCEW(kAppIconId));
    wc.hIconSm       = reinterpret_cast<HICON>(
        ::LoadImageW(::GetModuleHandleW(nullptr), MAKEINTRESOURCEW(kAppIconId),
                     IMAGE_ICON, ::GetSystemMetrics(SM_CXSMICON),
                     ::GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    if (wc.hIcon == nullptr) wc.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);

    if (::RegisterClassExW(&wc) == 0) {
        const DWORD err = ::GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            LT_ERROR(L"注册主窗口类失败，错误码 " + std::to_wstring(err));
            return false;
        }
    }
    registered = true;
    return true;
}

bool MainWindow::Show() {
    if (!EnsureClassRegistered()) return false;

    if (hwnd_ != nullptr) {
        ::ShowWindow(hwnd_, SW_RESTORE);
        ::SetForegroundWindow(hwnd_);
        RefreshMemoryText();
        return true;
    }

    // 居中于主显示器工作区
    RECT work{};
    ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int x = work.left + ((work.right - work.left) - kWinW) / 2;
    const int y = work.top + ((work.bottom - work.top) - kWinH) / 2;

    // 主窗口样式。
    //
    // 教训：**不要手工拼装窗口样式位**。
    // WS_CAPTION 并非独立标志位，它等价于 WS_BORDER | WS_DLGFRAME；
    // 而 WS_OVERLAPPED 的值就是 0。手工拼装容易漏位或写错组合，
    // 正确做法是用系统预定义组合 WS_OVERLAPPEDWINDOW
    // （= BORDER|DLGFRAME|SYSMENU|THICKFRAME|MINIMIZEBOX|MAXIMIZEBOX），
    // 再按需去掉最大化。
    //
    // 注意：真正导致"关不掉、没法最小化"的原因不在样式位，而是创建期间
    // DefWindowProc 拿到了空句柄（见 WndProc 中 WM_NCCREATE 的说明）。
    // 样式位本身一直是对的 —— 所以只检查 GWL_STYLE 永远发现不了那个 bug，
    // 必须测量"窗口高度 - 客户区高度"。
    const DWORD style = (WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX);

    // 用局部变量接收返回值：WM_NCCREATE 已把句柄写进 hwnd_，
    // 若创建失败必须用返回值覆盖掉那个已失效的句柄。
    HWND created = ::CreateWindowExW(
        WS_EX_APPWINDOW, L"LightTransAI_Main", L"鲤桥翻译",
        style,
        x, y, kWinW, kWinH, nullptr, nullptr, ::GetModuleHandleW(nullptr), this);
    hwnd_ = created;

    if (created == nullptr) {
        LT_ERROR(L"创建主窗口失败，错误码 " + std::to_wstring(::GetLastError()));
        return false;
    }

    // CreateWindowExW 的宽高描述的是**整个窗口**（含标题栏与边框），
    // 因此建出来的客户区会比 kWinW x kWinH 小一圈，底部控件可能被裁掉。
    // 这里按实测边框反算并校正，使客户区恰好等于设计尺寸。
    // 该做法不依赖 DPI：边框宽高直接由系统实测得到。
    {
        RECT wr{}, cr{};
        ::GetWindowRect(hwnd_, &wr);
        ::GetClientRect(hwnd_, &cr);
        const int frameW = (wr.right - wr.left) - cr.right;
        const int frameH = (wr.bottom - wr.top) - cr.bottom;

        // 运行时自检：有标题栏时边框高度约 39px（标题栏 23 + 边框 + 内边距）。
        // 为 0 即表示没有非客户区 —— 这正是"关不掉、没法最小化"的表现。
        wchar_t buf[256]{};
        swprintf_s(buf,
            L"主窗口创建后: 样式=0x%08lX 窗口=%dx%d 客户区=%dx%d 边框=%dx%dpx",
            static_cast<unsigned long>(::GetWindowLongPtrW(hwnd_, GWL_STYLE)),
            wr.right - wr.left, wr.bottom - wr.top,
            cr.right, cr.bottom, frameW, frameH);
        if (frameH > 20) {
            LT_INFO(buf);
        } else {
            LT_WARN(std::wstring(buf) + L"  ← 缺少标题栏（异常）");
        }

        if (frameW >= 0 && frameH >= 0) {
            const int winW = kWinW + frameW;
            const int winH = kWinH + frameH;
            const int cx = work.left + ((work.right - work.left) - winW) / 2;
            const int cy = work.top + ((work.bottom - work.top) - winH) / 2;
            ::SetWindowPos(hwnd_, nullptr, cx, cy, winW, winH,
                           SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }

    CreateControls();
    Layout();

    // 显式给窗口设置图标。
    // 类图标只在**首次注册类**时生效，若同一进程里类已注册过（或注册失败），
    // 窗口就会拿到空图标，任务栏上显示成默认的白纸。
    // 直接对窗口 SetIcon 是唯一稳妥的做法，同时设置大/小两个尺寸。
    // 注意 HICON 是 void*，条件里声明变量会与 MSVC 的解析冲突，故分开写。
    HICON bigIcon = ::LoadIconW(::GetModuleHandleW(nullptr),
                                MAKEINTRESOURCEW(kAppIconId));
    if (bigIcon != nullptr) {
        ::SendMessageW(hwnd_, WM_SETICON, ICON_BIG,
                       reinterpret_cast<LPARAM>(bigIcon));
    }
    HICON smallIcon = reinterpret_cast<HICON>(
        ::LoadImageW(::GetModuleHandleW(nullptr), MAKEINTRESOURCEW(kAppIconId),
                     IMAGE_ICON, ::GetSystemMetrics(SM_CXSMICON),
                     ::GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    if (smallIcon != nullptr) {
        ::SendMessageW(hwnd_, WM_SETICON, ICON_SMALL,
                       reinterpret_cast<LPARAM>(smallIcon));
    }

    ::ShowWindow(hwnd_, SW_SHOW);
    ::UpdateWindow(hwnd_);
    ::SetForegroundWindow(hwnd_);

    RefreshMemoryText();
    LT_INFO(L"主窗口已显示");
    return true;
}

void MainWindow::Hide() {
    if (hwnd_ != nullptr) {
        ::ShowWindow(hwnd_, SW_HIDE);
    }
}

void MainWindow::Destroy() {
    if (hwnd_ != nullptr) {
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (fontNormal_ != nullptr) { ::DeleteObject(fontNormal_); fontNormal_ = nullptr; }
    if (fontBold_ != nullptr)   { ::DeleteObject(fontBold_);   fontBold_ = nullptr; }
    if (fontSmall_ != nullptr)  { ::DeleteObject(fontSmall_);  fontSmall_ = nullptr; }
}

bool MainWindow::IsVisible() const {
    return hwnd_ != nullptr && ::IsWindowVisible(hwnd_) != FALSE;
}

void MainWindow::CreateControls() {
    fontBold_   = MakeFont(13, true);
    fontNormal_ = MakeFont(9, false);
    fontSmall_  = MakeFont(8, false);

    hTitle_    = MakeLabel(hwnd_, IDC_TITLE, L"鲤桥翻译", fontBold_);
    hSubtitle_ = MakeLabel(hwnd_, IDC_SUBTITLE,
                           L"选中文字后按 Alt+E 翻译　·　Alt+Q 框选翻译", fontSmall_);

    // 划词悬浮按钮暂时停用（外观待重做），这里保留控件但置灰，
    // 让用户明白该功能暂不可用，而不是以为程序坏了。
    hFloat_    = MakeCheck(hwnd_, IDC_CHECK_FLOAT,
                           L"划词悬浮翻译按钮（暂停用，请改用 Alt+E）", fontNormal_);
    ::EnableWindow(hFloat_, FALSE);

    hSettings_ = MakeButton(hwnd_, IDC_BTN_SETTINGS, L"打开设置", fontNormal_);
    hFeature2_ = MakeButton(hwnd_, IDC_BTN_FEATURE2, L"功能二：框选翻译 (Alt+Q)", fontNormal_);
    hFeature3_ = MakeButton(hwnd_, IDC_BTN_FEATURE3, L"功能三：原位置覆盖 (Alt+W)", fontNormal_);

    hMem_      = MakeLabel(hwnd_, IDC_STATIC_MEM, L"内存：读取中……", fontSmall_);
    hStatus_   = MakeLabel(hwnd_, IDC_STATIC_STATUS, L"就绪。", fontSmall_);
    hQuit_     = MakeButton(hwnd_, IDC_BTN_QUIT, L"退出程序", fontNormal_);

    ::SendMessageW(hFloat_, BM_SETCHECK,
                   float_enabled_ ? BST_CHECKED : BST_UNCHECKED, 0);
}

void MainWindow::Layout() {
    if (hwnd_ == nullptr) return;

    RECT rc{};
    ::GetClientRect(hwnd_, &rc);
    const int W = rc.right;
    const int pad = 22;
    const int innerW = W - pad * 2;
    int y = 20;

    auto move = [](HWND h, int x, int y2, int w, int hh) {
        if (h) ::MoveWindow(h, x, y2, w, hh, TRUE);
    };

    move(hTitle_, pad, y, innerW, 26);
    y += 28;
    move(hSubtitle_, pad, y, innerW, 18);
    y += 30;

    move(hFloat_, pad, y, innerW, 24);
    y += 40;

    move(hSettings_, pad, y, 150, 32);
    move(hQuit_, W - pad - 110, y, 110, 32);
    y += 48;

    move(hFeature2_, pad, y, innerW, 32);
    y += 40;
    move(hFeature3_, pad, y, innerW, 32);
    y += 48;

    move(hMem_, pad, y, innerW, 18);
    y += 24;
    move(hStatus_, pad, y, innerW, 34);
}

void MainWindow::RefreshMemoryText() {
    if (hMem_ == nullptr) return;
    const MemSnapshot s = PerfMonitor::Capture();
    wchar_t buf[160]{};
    swprintf_s(buf, L"内存：工作集 %.2f MB　私有提交 %.2f MB",
               s.WorkingSetMb(), s.PrivateMb());
    ::SetWindowTextW(hMem_, buf);
}

void MainWindow::SetFloatButtonEnabled(bool enabled) {
    float_enabled_ = enabled;
    if (hFloat_ != nullptr) {
        ::SendMessageW(hFloat_, BM_SETCHECK, enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    }
}

void MainWindow::SetStatus(const std::wstring& text) {
    if (hStatus_ != nullptr) {
        ::SetWindowTextW(hStatus_, text.c_str());
    }
}

void MainWindow::OnCommand(int id) {
    switch (id) {
        case IDC_CHECK_FLOAT:
            // 交给 app_controller 处理（它会更新配置、钩子与托盘勾选状态）
            if (on_command_) on_command_(CMD_TRAY_TOGGLE_FLOAT);
            break;

        case IDC_BTN_SETTINGS:
            if (on_command_) on_command_(CMD_TRAY_SETTINGS);
            break;

        case IDC_BTN_FEATURE2:
            if (on_command_) on_command_(CMD_TRAY_FEATURE2);
            break;

        case IDC_BTN_FEATURE3:
            if (on_command_) on_command_(CMD_TRAY_FEATURE3);
            break;

        case IDC_BTN_QUIT:
            if (on_command_) on_command_(CMD_TRAY_QUIT);
            break;

        default:
            break;
    }
}

LRESULT CALLBACK MainWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        auto* self = reinterpret_cast<MainWindow*>(cs->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                            reinterpret_cast<LONG_PTR>(self));
        // 关键：必须在 WM_NCCREATE 阶段就绑定真实句柄。
        //
        // 消息顺序是 WM_NCCREATE -> WM_NCCALCSIZE -> WM_CREATE，而 hwnd_ 原先只在
        // CreateWindowExW 返回后才赋值。于是创建期间的默认分支执行的是
        //     ::DefWindowProcW(nullptr, WM_NCCALCSIZE, ...)
        // —— 句柄为 NULL，DefWindowProc 直接返回 0，系统据此认定"客户区 = 整个
        // 窗口"，非客户区高度被算成 0。
        //
        // 后果：窗口没有标题栏、系统菜单、最小化/关闭按钮，但 GWL_STYLE 里
        // WS_CAPTION 等标志位一切正常，样式自检完全看不出问题，只有测量边框高度
        // （窗口高 - 客户区高）才会暴露为 0。用户反馈的"关不掉、没法最小化"即是此因。
        if (self != nullptr) self->hwnd_ = hwnd;
        return ::DefWindowProcW(hwnd, msg, wp, lp);
    }

    auto* self = reinterpret_cast<MainWindow*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self == nullptr) {
        return ::DefWindowProcW(hwnd, msg, wp, lp);
    }
    return self->HandleMessage(msg, wp, lp);
}

LRESULT MainWindow::HandleMessage(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_COMMAND: {
            const int id = LOWORD(wp);
            const int code = HIWORD(wp);
            if (code == BN_CLICKED) {
                OnCommand(id);
            }
            return 0;
        }

        case WM_SIZE:
            Layout();
            return 0;

        case WM_ACTIVATE:
            // 激活时刷新内存显示（不轮询，只在用户切回来时更新一次）
            if (LOWORD(wp) != WA_INACTIVE) {
                RefreshMemoryText();
            }
            return 0;

        case WM_SETFOCUS:
            RefreshMemoryText();
            return 0;

        case WM_CLOSE:
            // 关闭行为由 app_controller 决定：
            //   托盘可用 -> 收起（隐藏），程序继续后台运行
            //   托盘不可用 -> 直接退出，否则程序将彻底失联
            if (on_close_request_) {
                on_close_request_();
            } else {
                Destroy();
            }
            return 0;

        case WM_DESTROY:
            hwnd_ = nullptr;
            return 0;

        default:
            break;
    }
    return ::DefWindowProcW(hwnd_, msg, wp, lp);
}

} // namespace lt
