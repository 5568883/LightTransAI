#include "app/settings_window.h"

#include <commctrl.h>

#pragma comment(lib, "comctl32.lib")

#include "app/app_messages.h"
#include "config/config_manager.h"
#include "config/crypto_dpapi.h"
#include "input/hotkey_manager.h"
#include "ui/ui_common.h"
#include "utils/logger.h"
#include "utils/string_utils.h"

namespace lt {

namespace {

// 控件 ID
enum : int {
    IDC_EDIT_URL        = 1001,
    IDC_EDIT_KEY        = 1002,
    IDC_EDIT_MODEL      = 1003,
    IDC_EDIT_VURL       = 1004,
    IDC_EDIT_VKEY       = 1005,
    IDC_EDIT_VMODEL     = 1006,
    IDC_COMBO_VDETAIL   = 1007,
    IDC_CHECK_VENABLE   = 1008,
    IDC_CHECK_F1        = 1009,   // 功能一开关（沿用原悬浮按钮控件的 ID）
    IDC_COMBO_RECOG     = 1010,
    IDC_EDIT_HKTEXT     = 1011,
    IDC_EDIT_HKCAP      = 1012,
    IDC_EDIT_HKOVL      = 1013,
    IDC_BTN_TEST_TEXT   = 1014,
    IDC_BTN_TEST_VISION = 1015,
    IDC_BTN_SAVE        = 1016,
    IDC_STATUS          = 1017,
    IDC_NOTE            = 1018,
    IDC_BTN_QUIT        = 1019,
    IDC_CHECK_F2        = 1020,   // 功能二开关
    IDC_CHECK_F3        = 1021,   // 功能三开关
    IDC_COMBO_F1TRIG    = 1022,   // 功能一触发方式
};

constexpr int kWindowW = 620;
// 窗口高度**不再手写**：由 CreateControls() 按实际内容算出（见 content_height_）。
// 这里只保留一个兜底值，供布局尚未执行时使用。
constexpr int kWindowHFallback = 700;
constexpr int kStatusH        = 26;   // 状态栏行高（含一点余量，避免贴底）

// 布局辅助
constexpr int kLabelW  = 118;
constexpr int kFieldX  = 136;
constexpr int kFieldW  = 452;
constexpr int kRowH    = 26;
constexpr int kGap     = 8;

// 简易字体
//
// 注意：GetDC 必须配 ReleaseDC。原写法把 GetDC 直接嵌在 MulDiv 里，
// 每调用一次就泄漏一个 DC（窗口每次打开都会泄漏两个）。
HFONT MakeFont(int pt, bool bold) {
    HDC screen = ::GetDC(nullptr);
    const int dpiY = (screen != nullptr) ? ::GetDeviceCaps(screen, LOGPIXELSY) : 96;
    if (screen != nullptr) ::ReleaseDC(nullptr, screen);

    const int height = -::MulDiv(pt, dpiY > 0 ? dpiY : 96, 72);
    return ::CreateFontW(height, 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL,
                         FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
}

HWND MakeLabel(HWND parent, const wchar_t* text, int x, int y, int w, HFONT font) {
    HWND h = ::CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
                               x, y, w, 20, parent, nullptr,
                               ::GetModuleHandleW(nullptr), nullptr);
    if (font) ::SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return h;
}

HWND MakeEdit(HWND parent, int id, int x, int y, int w, HFONT font, bool password) {
    const DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL |
                        (password ? ES_PASSWORD : 0);
    HWND h = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", style,
                               x, y, w, kRowH, parent,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                               ::GetModuleHandleW(nullptr), nullptr);
    if (font) ::SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return h;
}

HWND MakeButton(HWND parent, int id, const wchar_t* text, int x, int y, int w, int h, HFONT font) {
    HWND hw = ::CreateWindowExW(0, L"BUTTON", text,
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                x, y, w, h, parent,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                ::GetModuleHandleW(nullptr), nullptr);
    if (font) ::SendMessageW(hw, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return hw;
}

HWND MakeCheck(HWND parent, int id, const wchar_t* text, int x, int y, int w, HFONT font) {
    HWND h = ::CreateWindowExW(0, L"BUTTON", text,
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                               x, y, w, 22, parent,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                               ::GetModuleHandleW(nullptr), nullptr);
    if (font) ::SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return h;
}

HWND MakeCombo(HWND parent, int id, int x, int y, int w, int dropdown_h, HFONT font) {
    HWND hc = ::CreateWindowExW(0, L"COMBOBOX", L"",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                x, y, w, dropdown_h, parent,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                ::GetModuleHandleW(nullptr), nullptr);
    if (font) ::SendMessageW(hc, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return hc;
}

std::wstring GetText(HWND h) {
    const int len = ::GetWindowTextLengthW(h);
    if (len <= 0) return {};
    std::wstring s(static_cast<size_t>(len) + 1, L'\0');
    ::GetWindowTextW(h, s.data(), len + 1);
    s.resize(static_cast<size_t>(len));
    return s;
}

void SetText(HWND h, const std::wstring& s) {
    ::SetWindowTextW(h, s.c_str());
}

} // namespace

bool SettingsWindow::EnsureClassRegistered() {
    static bool registered = false;
    if (registered) return true;

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = &SettingsWindow::WndProc;
    wc.hInstance     = ::GetModuleHandleW(nullptr);
    wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kSettingsWindowClass;
    // 与主窗口同一套图标：大图标给 Alt+Tab / 任务栏，小图标给标题栏
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
            LT_ERROR(L"注册设置窗口类失败，错误码 " + std::to_wstring(err));
            return false;
        }
    }
    registered = true;
    return true;
}

bool SettingsWindow::Show(HWND owner) {
    if (!EnsureClassRegistered()) return false;

    // 已打开则激活并前置，不重复创建
    if (hwnd_ != nullptr) {
        ::ShowWindow(hwnd_, SW_RESTORE);
        ::SetForegroundWindow(hwnd_);
        return true;
    }

    hwndOwner_ = owner;

    const float scale = 1.0f;  // 设置窗口使用系统 DPI 缩放，交给窗口本身处理
    const int w = static_cast<int>(kWindowW * scale);
    // 先用兜底高度建窗，CreateControls 之后会按实测内容高度校正（见下）。
    const int h = static_cast<int>(kWindowHFallback * scale);

    // 居中于主显示器工作区
    RECT work{};
    ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int x = work.left + ((work.right - work.left) - w) / 2;
    const int y = work.top + ((work.bottom - work.top) - h) / 2;

    // 关键：owner 传 nullptr，而不是主窗口。
    //
    // 传入 owner 会让设置窗口**永远被 owner 压在下面**（Windows 保证 owned 窗口
    // 始终位于 owner 之上，反之亦然），结果就是用户点不到设置窗口的标题栏按钮，
    // 表现为"关不掉、也最小化不了"。
    // 传 nullptr 后两个窗口彼此独立，可正常互相切换层级。
    //
    // 样式同样使用系统预定义组合；WS_CAPTION 等位的正确性并不保证标题栏存在，
    // 关键在 WndProc 的 WM_NCCREATE 里绑定句柄（见 main_window.cpp 的说明）。
    const DWORD style = (WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX);
    HWND created = ::CreateWindowExW(
        WS_EX_APPWINDOW, kSettingsWindowClass, L"鲤桥翻译 · 设置",
        style,
        x, y, w, h, nullptr, nullptr, ::GetModuleHandleW(nullptr), this);
    hwnd_ = created;

    if (created == nullptr) {
        LT_ERROR(L"创建设置窗口失败，错误码 " + std::to_wstring(::GetLastError()));
        return false;
    }

    // 运行时自检 + 客户区尺寸校正（同 main_window.cpp）：
    // CreateWindowExW 的宽高是整个窗口，扣掉标题栏/边框后客户区会变小。
    // 先测出边框厚度，等 CreateControls 算出内容高度后再统一校正。
    int frameW = 0;
    int frameH = 0;
    {
        RECT wr{}, cr{};
        ::GetWindowRect(hwnd_, &wr);
        ::GetClientRect(hwnd_, &cr);
        frameW = (wr.right - wr.left) - cr.right;
        frameH = (wr.bottom - wr.top) - cr.bottom;
        wchar_t buf[256]{};
        swprintf_s(buf,
            L"设置窗口创建后: 样式=0x%08lX 窗口=%dx%d 客户区=%dx%d 边框=%dx%dpx",
            static_cast<unsigned long>(::GetWindowLongPtrW(hwnd_, GWL_STYLE)),
            wr.right - wr.left, wr.bottom - wr.top,
            cr.right, cr.bottom, frameW, frameH);
        if (frameH > 20) {
            LT_INFO(buf);
        } else {
            LT_WARN(std::wstring(buf) + L"  ← 缺少标题栏（异常）");
        }
    }

    CreateControls();
    LoadFromConfig();

    // 按**实测内容高度**定窗口大小。
    //
    // 这是"保存设置后看不到成功提示"的修复：状态栏的 y 坐标由 CreateControls
    // 累加得出，窗口高度必须跟着它走，否则状态栏会落在客户区之外。
    // 同时做一次上限保护：屏幕放不下时贴住工作区，不做无意义的超高窗口。
    if (frameW >= 0 && frameH >= 0 && content_height_ > 0) {
        const int winW = w + frameW;
        int clientH = content_height_;
        const int maxClientH = (work.bottom - work.top) - frameH - 8;
        if (clientH > maxClientH) clientH = maxClientH;
        const int winH = clientH + frameH;
        const int cx = work.left + ((work.right - work.left) - winW) / 2;
        const int cy = work.top + ((work.bottom - work.top) - winH) / 2;
        ::SetWindowPos(hwnd_, nullptr, cx, cy, winW, winH,
                       SWP_NOZORDER | SWP_NOACTIVATE);
        LT_INFO(L"设置窗口按内容定高: 客户区 " + std::to_wstring(w) + L"x" +
                std::to_wstring(clientH));
    }

    // 显式设置窗口图标（理由同 main_window.cpp：类图标不一定生效）
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

    LT_INFO(L"设置窗口已打开");
    return true;
}

void SettingsWindow::CreateControls() {
    fontNormal_ = MakeFont(9, false);
    fontBold_   = MakeFont(10, true);

    const int x0 = 20;
    const int fieldX = x0 + kLabelW;
    const int fieldW = kWindowW - fieldX - 24;
    const int btnW = 150;
    const int btnH = 30;
    int y = 18;

    // ---------- 标题 ----------
    HWND title = MakeLabel(hwnd_, L"鲤桥翻译 · 设置", x0, y, 300, fontBold_);
    (void)title;
    y += 30;

    // ---------- DeepSeek API 区 ----------
    MakeLabel(hwnd_, L"DeepSeek API（功能一）", x0, y, 400, fontBold_);
    y += 24;

    MakeLabel(hwnd_, L"API 地址", x0, y + 4, kLabelW, fontNormal_);
    hUrl_ = MakeEdit(hwnd_, IDC_EDIT_URL, fieldX, y, fieldW, fontNormal_, false);
    y += kRowH + kGap;

    MakeLabel(hwnd_, L"API Key", x0, y + 4, kLabelW, fontNormal_);
    hKey_ = MakeEdit(hwnd_, IDC_EDIT_KEY, fieldX, y, fieldW, fontNormal_, true);
    y += kRowH + kGap;

    MakeLabel(hwnd_, L"模型名称", x0, y + 4, kLabelW, fontNormal_);
    hModel_ = MakeEdit(hwnd_, IDC_EDIT_MODEL, fieldX, y, fieldW, fontNormal_, false);
    y += kRowH + kGap;

    hTestText_ = MakeButton(hwnd_, IDC_BTN_TEST_TEXT, L"测试文本 API 连接",
                            fieldX, y, btnW, btnH, fontNormal_);
    y += btnH + 14;

    // ---------- 视觉模型区 ----------
    MakeLabel(hwnd_, L"视觉模型（功能二 / 功能三）", x0, y, 400, fontBold_);
    y += 24;

    hVisionEnable_ = MakeCheck(hwnd_, IDC_CHECK_VENABLE,
                               L"使用独立的视觉模型接口（不勾选则复用上面的 DeepSeek 配置）",
                               x0, y, 520, fontNormal_);
    y += 26;

    MakeLabel(hwnd_, L"API 地址", x0, y + 4, kLabelW, fontNormal_);
    hVisionUrl_ = MakeEdit(hwnd_, IDC_EDIT_VURL, fieldX, y, fieldW, fontNormal_, false);
    y += kRowH + kGap;

    MakeLabel(hwnd_, L"API Key", x0, y + 4, kLabelW, fontNormal_);
    hVisionKey_ = MakeEdit(hwnd_, IDC_EDIT_VKEY, fieldX, y, fieldW, fontNormal_, true);
    y += kRowH + kGap;

    MakeLabel(hwnd_, L"模型名称", x0, y + 4, kLabelW, fontNormal_);
    hVisionModel_ = MakeEdit(hwnd_, IDC_EDIT_VMODEL, fieldX, y, fieldW, fontNormal_, false);
    y += kRowH + kGap;

    MakeLabel(hwnd_, L"图片精度", x0, y + 4, kLabelW, fontNormal_);
    hVisionDetail_ = MakeCombo(hwnd_, IDC_COMBO_VDETAIL, fieldX, y, 160, 200, fontNormal_);
    ::SendMessageW(hVisionDetail_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"auto（自动）"));
    ::SendMessageW(hVisionDetail_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"low（低，更快）"));
    ::SendMessageW(hVisionDetail_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"high（高，更准）"));
    y += kRowH + kGap;

    hTestVision_ = MakeButton(hwnd_, IDC_BTN_TEST_VISION, L"测试视觉 API 连接",
                              fieldX, y, btnW, btnH, fontNormal_);
    y += btnH + 14;

    // ---------- 功能开关 ----------
    MakeLabel(hwnd_, L"功能开关", x0, y, 400, fontBold_);
    y += 24;

    hEnableF1_ = MakeCheck(hwnd_, IDC_CHECK_F1,
                           L"功能一：划词翻译（选中文字后翻译）", x0, y, 420, fontNormal_);
    y += 24;
    hEnableF2_ = MakeCheck(hwnd_, IDC_CHECK_F2,
                           L"功能二：框选截图翻译（框选屏幕区域识别并翻译）", x0, y, 460, fontNormal_);
    y += 24;
    // 功能三尚未实现，控件保持可用但给出明确说明，避免用户以为程序坏了
    hEnableF3_ = MakeCheck(hwnd_, IDC_CHECK_F3,
                           L"功能三：原位置覆盖（尚未实现，开启后也只会走框选识别）",
                           x0, y, 520, fontNormal_);
    y += 28;

    // 功能一的触发方式：二选一或两者都要。
    // 选"仅快捷键"时程序完全不安装全局鼠标钩子，对游戏和反作弊最友好。
    MakeLabel(hwnd_, L"功能一触发方式", x0, y + 4, kLabelW, fontNormal_);
    hF1Trigger_ = MakeCombo(hwnd_, IDC_COMBO_F1TRIG, fieldX, y, 260, 200, fontNormal_);
    ::SendMessageW(hF1Trigger_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"快捷键 + 悬浮按钮"));
    ::SendMessageW(hF1Trigger_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"仅快捷键（不装鼠标钩子）"));
    ::SendMessageW(hF1Trigger_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"仅悬浮按钮"));
    y += kRowH + kGap;

    // ---------- 触发与快捷键 ----------
    MakeLabel(hwnd_, L"快捷键", x0, y, 400, fontBold_);
    y += 24;

    MakeLabel(hwnd_, L"选中翻译", x0, y + 4, kLabelW, fontNormal_);
    hHkTranslate_ = MakeEdit(hwnd_, IDC_EDIT_HKTEXT, fieldX, y, 140, fontNormal_, false);
    MakeLabel(hwnd_, L"（如 Alt+E）", fieldX + 148, y + 4, 200, fontNormal_);
    y += kRowH + kGap;

    MakeLabel(hwnd_, L"框选翻译", x0, y + 4, kLabelW, fontNormal_);
    hHkCapture_ = MakeEdit(hwnd_, IDC_EDIT_HKCAP, fieldX, y, 140, fontNormal_, false);
    MakeLabel(hwnd_, L"（如 Alt+Q）", fieldX + 148, y + 4, 280, fontNormal_);
    y += kRowH + kGap;

    MakeLabel(hwnd_, L"覆盖翻译", x0, y + 4, kLabelW, fontNormal_);
    hHkOverlay_ = MakeEdit(hwnd_, IDC_EDIT_HKOVL, fieldX, y, 140, fontNormal_, false);
    MakeLabel(hwnd_, L"（如 Alt+W）", fieldX + 148, y + 4, 280, fontNormal_);
    y += kRowH + kGap;

    MakeLabel(hwnd_, L"识别模式", x0, y + 4, kLabelW, fontNormal_);
    hRecogMode_ = MakeCombo(hwnd_, IDC_COMBO_RECOG, fieldX, y, 260, 200, fontNormal_);
    ::SendMessageW(hRecogMode_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"AI 直接识图（默认）"));
    ::SendMessageW(hRecogMode_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"本地 OCR（扩展预留）"));
    y += kRowH + 16;

    // ---------- 底部 ----------
    hSave_ = MakeButton(hwnd_, IDC_BTN_SAVE, L"保存设置", x0, y, 120, 34, fontBold_);

    hNote_ = MakeLabel(hwnd_, L"",
                       x0 + 132, y + 8, kWindowW - x0 - 150, fontNormal_);
    y += 42;

    hStatus_ = MakeLabel(hwnd_, L"就绪。", x0, y, kWindowW - 2 * x0, fontNormal_);

    // 把内容总高度回传，由调用方据此决定窗口高度。
    //
    // 教训：之前窗口高度是个手写的常量（kWindowH），加"功能开关"区时只把常量
    // 从 700 改到 790，但内容实际需要 812 —— 状态栏正好落在客户区之外，
    // 于是"设置已保存"这条提示用户永远看不到，表现为"保存后没有任何反馈"。
    // 改成由布局自己算出高度，以后增删控件都不会再出现这个问题。
    content_height_ = y + kStatusH;

    // 限制输入长度
    ::SendMessageW(hUrl_, EM_SETLIMITTEXT, 512, 0);
    ::SendMessageW(hVisionUrl_, EM_SETLIMITTEXT, 512, 0);
    ::SendMessageW(hKey_, EM_SETLIMITTEXT, 256, 0);
    ::SendMessageW(hVisionKey_, EM_SETLIMITTEXT, 256, 0);
    ::SendMessageW(hModel_, EM_SETLIMITTEXT, 128, 0);
    ::SendMessageW(hVisionModel_, EM_SETLIMITTEXT, 128, 0);
}

void SettingsWindow::LoadFromConfig() {
    const AppConfig& cfg = ConfigManager::Instance().Get();

    SetText(hUrl_, cfg.api_url);
    SetText(hKey_, str::FromUtf8(cfg.api_key_plain));
    SetText(hModel_, cfg.text_model);

    const bool useSeparateVision = !cfg.vision_api_url.empty() ||
                                   !cfg.vision_api_key_plain.empty();
    ::SendMessageW(hVisionEnable_, BM_SETCHECK,
                   useSeparateVision ? BST_CHECKED : BST_UNCHECKED, 0);

    SetText(hVisionUrl_, cfg.vision_api_url);
    SetText(hVisionKey_, str::FromUtf8(cfg.vision_api_key_plain));
    SetText(hVisionModel_, cfg.vision_model);

    int detailSel = 0;
    if (cfg.vision_image_detail == L"low")       detailSel = 1;
    else if (cfg.vision_image_detail == L"high") detailSel = 2;
    ::SendMessageW(hVisionDetail_, CB_SETCURSEL, detailSel, 0);

    // 功能开关
    auto check = [](HWND h, bool on) {
        ::SendMessageW(h, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
    };
    check(hEnableF1_, cfg.enable_feature1);
    check(hEnableF2_, cfg.enable_feature2);
    check(hEnableF3_, cfg.enable_feature3);
    ::SendMessageW(hF1Trigger_, CB_SETCURSEL, static_cast<int>(cfg.feature1_trigger), 0);

    HotkeySpec hk{};
    hk.mods = cfg.hk_translate_mods;
    hk.vk   = cfg.hk_translate_vk;
    SetText(hHkTranslate_, HotkeyManager::Format(hk));
    hk.mods = cfg.hk_capture_mods;
    hk.vk   = cfg.hk_capture_vk;
    SetText(hHkCapture_, HotkeyManager::Format(hk));
    hk.mods = cfg.hk_overlay_mods;
    hk.vk   = cfg.hk_overlay_vk;
    SetText(hHkOverlay_, HotkeyManager::Format(hk));

    ::SendMessageW(hRecogMode_, CB_SETCURSEL,
                   cfg.recognition_mode == RecognitionModeConfig::LocalOcrReserve ? 1 : 0, 0);

    // 若上次因换账户导致 Key 无法解密，明确提示
    const std::wstring& warn = ConfigManager::Instance().LastDecryptWarning();
    if (!warn.empty()) {
        SetStatus(warn, true);
    } else if (cfg.api_key_plain.empty()) {
        SetStatus(L"尚未配置 API Key，请填写后点击「保存设置」。", true);
    }

    // 配置目录回退提示（%APPDATA% 不可写时）
    const std::wstring& pathWarn = ConfigManager::Instance().PathWarning();
    if (!pathWarn.empty()) {
        SetText(hNote_, pathWarn);
    } else {
        SetText(hNote_, L"API Key 使用 Windows DPAPI 加密后存储于本地。");
    }
}

bool SettingsWindow::SaveToConfig() {
    AppConfig& cfg = ConfigManager::Instance().Get();

    cfg.api_url    = str::Trim(GetText(hUrl_));
    cfg.text_model = str::Trim(GetText(hModel_));

    const std::string newKey = str::ToUtf8(str::Trim(GetText(hKey_)));
    if (!newKey.empty()) {
        cfg.api_key_plain = newKey;
        cfg.api_key_enc   = CryptoDpapi::Encrypt(newKey);
        if (cfg.api_key_enc.empty()) {
            SetStatus(L"API Key 加密失败，未能保存。", true);
            return false;
        }
    } else {
        cfg.api_key_plain.clear();
        cfg.api_key_enc.clear();
    }

    const bool useSeparateVision =
        ::SendMessageW(hVisionEnable_, BM_GETCHECK, 0, 0) == BST_CHECKED;

    if (useSeparateVision) {
        cfg.vision_api_url = str::Trim(GetText(hVisionUrl_));
        cfg.vision_model   = str::Trim(GetText(hVisionModel_));

        const std::string vKey = str::ToUtf8(str::Trim(GetText(hVisionKey_)));
        if (!vKey.empty()) {
            cfg.vision_api_key_plain = vKey;
            cfg.vision_api_key_enc   = CryptoDpapi::Encrypt(vKey);
        } else {
            cfg.vision_api_key_plain.clear();
            cfg.vision_api_key_enc.clear();
        }
    } else {
        cfg.vision_api_url.clear();
        cfg.vision_api_key_plain.clear();
        cfg.vision_api_key_enc.clear();
        cfg.vision_model = cfg.text_model;
    }

    const int detailSel = static_cast<int>(::SendMessageW(hVisionDetail_, CB_GETCURSEL, 0, 0));
    cfg.vision_image_detail = (detailSel == 1) ? L"low"
                            : (detailSel == 2) ? L"high"
                                               : L"auto";

    // ---------- 功能开关 ----------
    // 先取旧值再赋值：下面的"是否变化"比较要用旧值，写反了就会恒等于自己。
    const bool oldF1 = cfg.enable_feature1;
    const bool oldF2 = cfg.enable_feature2;
    const bool oldF3 = cfg.enable_feature3;

    cfg.enable_feature1 =
        ::SendMessageW(hEnableF1_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    cfg.enable_feature2 =
        ::SendMessageW(hEnableF2_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    cfg.enable_feature3 =
        ::SendMessageW(hEnableF3_, BM_GETCHECK, 0, 0) == BST_CHECKED;

    const int trigSel = static_cast<int>(::SendMessageW(hF1Trigger_, CB_GETCURSEL, 0, 0));
    const Feature1Trigger oldTrig = cfg.feature1_trigger;
    cfg.feature1_trigger = (trigSel == 1) ? Feature1Trigger::HotkeyOnly
                         : (trigSel == 2) ? Feature1Trigger::FloatOnly
                                          : Feature1Trigger::Both;

    // 功能三尚未实现：允许勾选，但每次保存都给出明确说明，
    // 避免用户以为"打开了却没反应"是程序坏了。
    const bool f3Warn = cfg.enable_feature3;

    const int recogSel = static_cast<int>(::SendMessageW(hRecogMode_, CB_GETCURSEL, 0, 0));
    cfg.recognition_mode = (recogSel == 1) ? RecognitionModeConfig::LocalOcrReserve
                                           : RecognitionModeConfig::AiVisionDirect;

    // ---------- 快捷键 ----------
    // 这三个输入框以前是完全没被读取的：用户改了、点了保存，改动被静默丢弃。
    // 现在先全部解析成功再写入，任何一个不合法就整体放弃保存，
    // 避免出现"两个热键生效、一个没生效"的半吊子状态。
    struct HkTarget {
        HWND           edit;
        const wchar_t* label;
        unsigned int*  mods;
        unsigned int*  vk;
    };
    const HkTarget hkTargets[] = {
        { hHkTranslate_, L"选中翻译", &cfg.hk_translate_mods, &cfg.hk_translate_vk },
        { hHkCapture_,   L"框选翻译", &cfg.hk_capture_mods,   &cfg.hk_capture_vk   },
        { hHkOverlay_,   L"覆盖翻译", &cfg.hk_overlay_mods,   &cfg.hk_overlay_vk   },
    };

    unsigned int newMods[3]{};
    unsigned int newVk[3]{};
    for (int i = 0; i < 3; ++i) {
        std::wstring err;
        if (!HotkeyManager::Parse(GetText(hkTargets[i].edit),
                                  newMods[i], newVk[i], err)) {
            SetStatus(std::wstring(hkTargets[i].label) + L"快捷键无效：" + err, true);
            return false;
        }
    }

    // 三个热键之间不能重复（同一组合注册两次必然失败）
    for (int i = 0; i < 3; ++i) {
        for (int j = i + 1; j < 3; ++j) {
            if (newMods[i] == newMods[j] && newVk[i] == newVk[j]) {
                SetStatus(std::wstring(L"“") + hkTargets[i].label + L"”与“" +
                          hkTargets[j].label + L"”的快捷键相同，请改成不同的组合。",
                          true);
                return false;
            }
        }
    }

    bool hotkeysChanged = false;
    for (int i = 0; i < 3; ++i) {
        if (*hkTargets[i].mods != newMods[i] || *hkTargets[i].vk != newVk[i]) {
            hotkeysChanged = true;
        }
        *hkTargets[i].mods = newMods[i];
        *hkTargets[i].vk   = newVk[i];
    }

    const bool switchesChanged =
        (oldF1 != cfg.enable_feature1) || (oldF2 != cfg.enable_feature2) ||
        (oldF3 != cfg.enable_feature3) || (oldTrig != cfg.feature1_trigger);

    if (!ConfigManager::Instance().Save()) {
        SetStatus(L"保存失败：无法写入配置文件。", true);
        return false;
    }

    // 改动已落盘，让 app_controller 重新应用（重新注册热键 / 装拆鼠标钩子与悬浮按钮）。
    // 只在相关项确实变化时才做，避免每次"测试连接"都重装一遍。
    std::wstring applyWarn;
    if (on_saved_ && (hotkeysChanged || switchesChanged)) {
        applyWarn = on_saved_();
    }

    if (!applyWarn.empty()) {
        SetStatus(L"设置已保存，但" + applyWarn, true);
    } else if (f3Warn) {
        SetStatus(L"设置已保存。注意：功能三尚未实现，暂时只会完成框选与识别。", true);
    } else {
        SetStatus(L"设置已保存。", false);
    }
    LT_INFO(L"设置已保存");
    return true;
}

void SettingsWindow::SetStatus(const std::wstring& text, bool is_error) {
    if (hStatus_ == nullptr) return;
    ::SetWindowTextW(hStatus_, text.c_str());
    (void)is_error;  // 静态控件不支持着色，保留参数以便将来换成自绘
}

void SettingsWindow::ReportTestResult(bool ok, const std::wstring& message) {
    test_in_progress_ = false;
    if (hTestText_ != nullptr)   ::EnableWindow(hTestText_, TRUE);
    if (hTestVision_ != nullptr) ::EnableWindow(hTestVision_, TRUE);
    SetStatus(message, !ok);
}

void SettingsWindow::OnCommand(int id) {
    switch (id) {
        case IDC_BTN_SAVE:
            SaveToConfig();
            break;

        case IDC_BTN_TEST_TEXT:
        case IDC_BTN_TEST_VISION: {
            if (test_in_progress_) return;

            // 测试前先保存当前输入，确保测的就是用户刚填的内容
            if (!SaveToConfig()) return;

            test_in_progress_ = true;
            ::EnableWindow(hTestText_, FALSE);
            ::EnableWindow(hTestVision_, FALSE);
            SetStatus(L"正在测试连接，请稍候……", false);

            // 交给 app_controller 在后台线程执行，避免阻塞 UI
            ::PostMessageW(hwndOwner_, WM_LT_TEST_CONN_DONE,
                           (id == IDC_BTN_TEST_VISION) ? 1 : 0, 0);
            break;
        }

        case IDC_CHECK_VENABLE: {
            const bool on = ::SendMessageW(hVisionEnable_, BM_GETCHECK, 0, 0) == BST_CHECKED;
            ::EnableWindow(hVisionUrl_, on);
            ::EnableWindow(hVisionKey_, on);
            ::EnableWindow(hVisionModel_, on);
            ::EnableWindow(hVisionDetail_, on);
            break;
        }

        default:
            break;
    }
}

void SettingsWindow::Destroy() {
    if (hwnd_ != nullptr) {
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (fontNormal_ != nullptr) { ::DeleteObject(fontNormal_); fontNormal_ = nullptr; }
    if (fontBold_ != nullptr)   { ::DeleteObject(fontBold_);   fontBold_ = nullptr; }
}

LRESULT CALLBACK SettingsWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        auto* self = reinterpret_cast<SettingsWindow*>(cs->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                            reinterpret_cast<LONG_PTR>(self));
        // 见 main_window.cpp：创建期间就必须绑定句柄，否则 WM_NCCALCSIZE 会被
        // 交给 DefWindowProcW(nullptr, ...)，非客户区被算成 0，标题栏消失。
        if (self != nullptr) self->hwnd_ = hwnd;
        return ::DefWindowProcW(hwnd, msg, wp, lp);
    }

    auto* self = reinterpret_cast<SettingsWindow*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self == nullptr) {
        return ::DefWindowProcW(hwnd, msg, wp, lp);
    }
    return self->HandleMessage(msg, wp, lp);
}

LRESULT SettingsWindow::HandleMessage(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_COMMAND: {
            const int id = LOWORD(wp);
            const int code = HIWORD(wp);
            if (code == BN_CLICKED || code == CBN_SELCHANGE) {
                OnCommand(id);
            }
            return 0;
        }

        case WM_CLOSE:
            // 关闭即销毁，让内存回落
            Destroy();
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
