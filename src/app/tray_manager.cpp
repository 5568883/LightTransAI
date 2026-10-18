#include "app/tray_manager.h"

#include "app/app_messages.h"
#include "utils/logger.h"
#include "utils/perf_monitor.h"
#include "utils/string_utils.h"

namespace lt {

namespace {

// 前置声明：CreateTrayIcon 在资源不可用时会回退到它
HICON CreateTrayIconByCode(int size);

// 托盘图标。
//
// 优先使用 exe 内嵌的多尺寸图标资源：LoadImage 会自动挑选与请求尺寸最接近的
// 那一帧（16/20/24/32/48/64/128/256 都有），因此托盘图标与任务栏、开始菜单里
// 看到的应用图标**完全一致**，不会出现"两套设计"的割裂感。
//
// 资源缺失时（例如仓库里没有 app.ico 就直接构建）退回代码绘制，
// 保证托盘不会变成空白图标。
HICON CreateTrayIcon(int size) {
    HICON fromRes = reinterpret_cast<HICON>(
        ::LoadImageW(::GetModuleHandleW(nullptr), MAKEINTRESOURCEW(kAppIconId),
                     IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
    if (fromRes != nullptr) return fromRes;

    LT_WARN(L"图标资源不可用，托盘图标退回代码绘制");
    return CreateTrayIconByCode(size);
}

// 用代码绘制一个极简的托盘图标（蓝底白字"译"）。
HICON CreateTrayIconByCode(int size) {
    // 创建 32bpp 带 alpha 的 DIB
    BITMAPV5HEADER bi{};
    bi.bV5Size        = sizeof(bi);
    bi.bV5Width       = size;
    bi.bV5Height      = -size;   // 负值 = 自上而下
    bi.bV5Planes      = 1;
    bi.bV5BitCount    = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask     = 0x00FF0000;
    bi.bV5GreenMask   = 0x0000FF00;
    bi.bV5BlueMask    = 0x000000FF;
    bi.bV5AlphaMask   = 0xFF000000;

    HDC hdc = ::GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP color = ::CreateDIBSection(hdc, reinterpret_cast<BITMAPINFO*>(&bi),
                                       DIB_RGB_COLORS, &bits, nullptr, 0);
    HBITMAP mask = ::CreateBitmap(size, size, 1, 1, nullptr);
    ::ReleaseDC(nullptr, hdc);

    if (color == nullptr || bits == nullptr) {
        if (color) ::DeleteObject(color);
        if (mask)  ::DeleteObject(mask);
        return nullptr;
    }

    // 绘制：圆角蓝底 + 白色"译"字
    HDC memDc = ::CreateCompatibleDC(nullptr);
    HGDIOBJ oldBmp = ::SelectObject(memDc, color);

    auto* px = static_cast<uint32_t*>(bits);
    const int cx = size / 2;
    const int cy = size / 2;
    const int r  = size / 2 - 1;

    // 背景透明，画一个抗锯齿近似的实心圆
    //
    // 颜色字节序注意：32bpp DIB 的内存布局是 B、G、R、A（小端 uint32 下
    // bit0-7=B, bit8-15=G, bit16-23=R, bit24-31=A）。
    // 目标色 #2E6FF2（R=0x2E, G=0x6F, B=0xF2）必须按 R/G/B 归位，
    // 早期写法把 R 与 B 写反，结果渲染成了橙色 #F26F2E。
    constexpr uint32_t kBrandR = 0x2E;
    constexpr uint32_t kBrandG = 0x6F;
    constexpr uint32_t kBrandB = 0xF2;

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const int dx = x - cx;
            const int dy = y - cy;
            const int dist2 = dx * dx + dy * dy;
            const int r2 = r * r;

            uint32_t argb = 0x00000000;   // 全透明
            if (dist2 <= r2) {
                // 边缘 1px 做柔和过渡
                const uint8_t alpha = (dist2 > (r - 1) * (r - 1)) ? 0xB0 : 0xFF;
                argb = (static_cast<uint32_t>(alpha) << 24) |
                       (kBrandR << 16) | (kBrandG << 8) | kBrandB;
            }
            px[y * size + x] = argb;
        }
    }

    // 写白色"译"字
    ::SetBkMode(memDc, TRANSPARENT);
    ::SetTextColor(memDc, RGB(255, 255, 255));
    HFONT font = ::CreateFontW(-static_cast<int>(size * 0.62f), 0, 0, 0,
                               FW_BOLD, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                               ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    HGDIOBJ oldFont = ::SelectObject(memDc, font);
    RECT tr{ 0, 0, size, size };
    ::DrawTextW(memDc, L"译", 1, &tr,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    ::SelectObject(memDc, oldFont);
    ::DeleteObject(font);

    // 修复 alpha 通道。
    // GDI 的文本/图形绘制在 32bpp DIB 上**不会写入 alpha 字节**，
    // 刚画上的白色文字其 alpha 仍为 0（即"完全透明"），
    // 这会得到一个畸形的图标：文字像素声称自己不可见。
    // 不同合成路径下表现不一致（有时文字消失、有时正常），必须显式补回。
    // 判据：alpha==0 但 RGB 非 0 的像素，就是被 GDI 画上却没写 alpha 的内容。
    for (int i = 0; i < size * size; ++i) {
        const uint32_t a   = (px[i] >> 24) & 0xFF;
        const uint32_t rgb = px[i] & 0x00FFFFFFu;
        if (a == 0 && rgb != 0) {
            px[i] = 0xFF000000u | rgb;
        }
    }

    ::SelectObject(memDc, oldBmp);
    ::DeleteDC(memDc);

    ICONINFO ii{};
    ii.fIcon    = TRUE;
    ii.hbmColor = color;
    ii.hbmMask  = mask;
    HICON icon = ::CreateIconIndirect(&ii);

    ::DeleteObject(color);
    ::DeleteObject(mask);
    return icon;
}

} // namespace

bool TrayManager::Install(HWND hwnd, const std::wstring& tooltip) {
    if (installed_) return true;
    if (hwnd == nullptr) return false;

    hwnd_ = hwnd;

    nid_ = {};
    nid_.cbSize = sizeof(nid_);
    nid_.hWnd   = hwnd;
    nid_.uID    = 1;
    nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid_.uCallbackMessage = WM_LT_TRAY;
    nid_.hIcon  = CreateTrayIcon(::GetSystemMetrics(SM_CXSMICON));
    if (nid_.hIcon == nullptr) {
        LT_ERROR(L"创建托盘图标失败（SM_CXSMICON=" +
                 std::to_wstring(::GetSystemMetrics(SM_CXSMICON)) + L"）");
        return false;
    }

    // 截断到 TIP 缓冲区内
    const std::wstring tip = tooltip.empty() ? L"鲤桥翻译" : tooltip;
    wcsncpy_s(nid_.szTip, tip.c_str(), _TRUNCATE);

    if (!::Shell_NotifyIconW(NIM_ADD, &nid_)) {
        // 记录真实错误码。这个数字是判断根因的唯一依据：
        //   5    = 拒绝访问（低完整性进程无法向中完整性的任务栏发消息）
        //   0    = 任务栏未响应或未运行
        // 没有它只能靠猜。
        const DWORD err = ::GetLastError();
        LT_ERROR(L"添加托盘图标失败，GetLastError=" + std::to_wstring(err) +
                 L"（5=拒绝访问/完整性级别受限，0=任务栏未响应）");
        ::DestroyIcon(nid_.hIcon);
        nid_.hIcon = nullptr;
        return false;
    }

    // 声明为 4.0 版本以支持后续可能的扩展
    nid_.uVersion = NOTIFYICON_VERSION_4;
    ::Shell_NotifyIconW(NIM_SETVERSION, &nid_);

    installed_ = true;
    LT_INFO(L"托盘图标已创建");
    return true;
}

void TrayManager::Remove() {
    if (!installed_) return;
    ::Shell_NotifyIconW(NIM_DELETE, &nid_);
    if (nid_.hIcon != nullptr) {
        ::DestroyIcon(nid_.hIcon);
        nid_.hIcon = nullptr;
    }
    installed_ = false;
    LT_INFO(L"托盘图标已移除");
}

void TrayManager::SetTooltip(const std::wstring& tip) {
    if (!installed_) return;
    wcsncpy_s(nid_.szTip, tip.c_str(), _TRUNCATE);
    nid_.uFlags = NIF_TIP;
    ::Shell_NotifyIconW(NIM_MODIFY, &nid_);
    nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
}

void TrayManager::OnTrayMessage(WPARAM /*wp*/, LPARAM lp) {
    switch (LOWORD(lp)) {
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            ShowContextMenu();
            break;
        case WM_LBUTTONDBLCLK:
            if (on_command_) on_command_(CMD_TRAY_SETTINGS);
            break;
        default:
            break;
    }
}

void TrayManager::ShowContextMenu() {
    if (!installed_ || hwnd_ == nullptr) return;

    HMENU menu = ::CreatePopupMenu();
    if (menu == nullptr) return;

    // 每次弹出前把前台窗口设为自己，确保菜单能正常关闭
    ::SetForegroundWindow(hwnd_);

    ::AppendMenuW(menu, MF_STRING, CMD_TRAY_SETTINGS, L"打开设置(&S)");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    ::AppendMenuW(menu, MF_STRING | (float_button_enabled_ ? MF_CHECKED : 0),
                  CMD_TRAY_TOGGLE_FLOAT, L"功能一：划词悬浮翻译(&F)");
    ::AppendMenuW(menu, MF_STRING, CMD_TRAY_FEATURE2, L"功能二：框选翻译 (Alt+Q)(&Q)");
    ::AppendMenuW(menu, MF_STRING, CMD_TRAY_FEATURE3, L"功能三：原位置覆盖 (Alt+W)(&W)");

    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    // 内存占用状态：实时读取并显示在菜单里
    const MemSnapshot snap = PerfMonitor::Capture();
    wchar_t memText[128]{};
    swprintf_s(memText, L"内存占用：%.1f MB（工作集）", snap.WorkingSetMb());
    ::AppendMenuW(menu, MF_STRING | MF_GRAYED, CMD_TRAY_MEMORY, memText);

    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, CMD_TRAY_QUIT, L"退出(&X)");

    POINT pt{};
    ::GetCursorPos(&pt);

    // TPM_RETURNCMD 让本函数直接拿到选择结果，避免额外的 WM_COMMAND 往返
    const UINT cmd = ::TrackPopupMenu(
        menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
        pt.x, pt.y, 0, hwnd_, nullptr);

    ::DestroyMenu(menu);

    // 让菜单能正确消失
    ::PostMessageW(hwnd_, WM_NULL, 0, 0);

    if (cmd != 0 && on_command_) {
        on_command_(cmd);
    }
}

} // namespace lt
