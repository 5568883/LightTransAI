#include "app/app_controller.h"

#include <windowsx.h>

#include "app/app_messages.h"
#include "capture/image_encoder.h"
#include "config/config_manager.h"
#include "input/text_extractor.h"
#include "recognition/ai_vision_provider.h"
#include "recognition/local_ocr_provider.h"
#include "ui/ui_common.h"
#include "utils/logger.h"
#include "utils/perf_monitor.h"
#include "utils/string_utils.h"

namespace lt {

AppController* AppController::s_instance_ = nullptr;

namespace {

// 功能可用性的编译期总开关。
//
// 功能一（划词翻译）与功能二（框选截图翻译）均已实现，恒为 true；
// 运行时的启用/禁用由配置里的 enable_feature1/2/3 决定。
//
// 功能三（原位置覆盖）尚未实现：即使把它打开，程序也只会完成
// "框选 → 识别"，不会在屏幕上画出覆盖层。因此这里恒为 false，
// 保证用户误开时不会得到一个"跑了一遍却什么都没发生"的困惑结果。
constexpr bool kFeature3Implemented = false;

// 划词悬浮按钮是否已就绪。
//
// 定位已按需求改为"贴在划选内容末尾的右侧"（见 ui::PlaceRightOfAnchor），
// 因此这里启用。若日后需要临时停用，把它改回 false 即可 ——
// 停用期间功能一依然完全可用（选中文字按 Alt+E），且不安装全局鼠标钩子。
constexpr bool kFloatButtonReady = true;

} // namespace

namespace {

// 判断光标是否为标准文本工字光标。
// 注意：这只是辅助预筛。很多应用（Electron、自绘控件）用 CopyCursor 生成
// 自定义句柄，其值不等于 LoadCursor(IDC_IBEAM)，因此不能单靠它决定是否弹按钮。
// 真正的判据是 UIA 是否探测到非空选区（正信号，且不触碰剪贴板）。
bool IsIBeamCursor() {
    CURSORINFO ci{};
    ci.cbSize = sizeof(ci);
    if (!::GetCursorInfo(&ci)) return false;
    if ((ci.flags & CURSOR_SHOWING) == 0) return false;

    static HCURSOR s_ibeam = ::LoadCursorW(nullptr, IDC_IBEAM);
    if (ci.hCursor == s_ibeam) return true;

    // 部分应用通过 CopyCursor 产生句柄，退一步用形状比对
    return false;
}

} // namespace

AppController::~AppController() {
    Shutdown();
}

// ================================================================ 初始化

bool AppController::CreateHostWindow(HINSTANCE hinst) {
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = [](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
        auto* self = reinterpret_cast<AppController*>(
            ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                                reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return ::DefWindowProcW(hwnd, msg, wp, lp);
        }
        if (self == nullptr) {
            return ::DefWindowProcW(hwnd, msg, wp, lp);
        }
        return self->HandleMessage(hwnd, msg, wp, lp);
    };
    wc.hInstance     = hinst;
    wc.lpszClassName = kHostWindowClass;

    if (::RegisterClassExW(&wc) == 0) {
        const DWORD err = ::GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            LT_ERROR(L"注册宿主窗口类失败，错误码 " + std::to_wstring(err));
            return false;
        }
    }

    // 不可见的消息宿主窗口：零常驻界面的关键。
    // 用 HWND_MESSAGE 让它不出现在任务栏、Alt+Tab 与任何 UI 枚举中。
    hwnd_ = ::CreateWindowExW(0, kHostWindowClass, L"LightTransAI_Host", 0,
                              0, 0, 0, 0, HWND_MESSAGE, nullptr, hinst, this);
    if (hwnd_ == nullptr) {
        LT_ERROR(L"创建宿主消息窗口失败，错误码 " + std::to_wstring(::GetLastError()));
        return false;
    }
    return true;
}

bool AppController::Initialize(HINSTANCE hinst) {
    s_instance_ = this;

    if (!CreateHostWindow(hinst)) return false;

    // ---- 配置 ----
    ConfigManager::Instance().Load();
    ConfigManager::Instance().EnsureDefaultExists();

    // ---- 日志 ----
    const std::wstring logPath = ConfigManager::Instance().DataDir() + L"\\LightTransAI.log";
    Logger::Instance().Init(logPath);
    LT_INFO(L"================ 鲤桥翻译 启动 ================");
    LT_INFO(L"配置目录: " + ConfigManager::Instance().DataDir());
    if (!ConfigManager::Instance().PathWarning().empty()) {
        LT_WARN(ConfigManager::Instance().PathWarning());
    }
    PerfMonitor::RecordToLog(L"启动完成");

    // ---- 托盘 ----
    tray_ = std::make_unique<TrayManager>();
    tray_->SetOnCommand([this](UINT cmd) { OnTrayCommand(cmd); });
    tray_->SetFloatButtonEnabled(ConfigManager::Instance().Get().FloatButtonEnabled());
    if (!tray_->Install(hwnd_, L"鲤桥翻译 —— 划词翻译")) {
        // 托盘是这个程序唯一的常驻 UI。若创建失败，用户将看不到任何界面，
        // 会误以为程序没启动。因此必须给出可见提示，而不是只写日志。
        LT_ERROR(L"托盘创建失败：程序将无可见界面，仅能通过快捷键使用");
        tray_failed_ = true;
    }

    // ---- 热键 ----
    hotkeys_ = std::make_unique<HotkeyManager>(hwnd_);
    hotkeys_->SetCallback([this](int id) { OnHotkeyPressed(id); });
    RegisterHotkeys();

    // ---- 划词检测 ----
    // 钩子只投递 WM_LT_SELECTION_CAND，回调不设置（避免在钩子栈内做任何额外工作）
    mouse_hook_ = std::make_unique<MouseSelectionHook>();

    // ---- UI 组件 ----
    //
    // 悬浮按钮是功能一的第二条触发通道，但定位改造尚未完成，
    // 因此 kFloatButtonReady 为 false 时不创建（详见该常量的说明）。
    if (kFloatButtonReady) EnsureFloatButton();

    popup_ = std::make_unique<PopupWindow>();
    popup_->SetOnClose([this]() {
        // 弹窗关闭：取消在途请求并回收内存。
        // 注意：必须投递 WM_LT_TRIM_MEMORY 这条消息，不能把定时器 ID 当消息用，
        // 否则消息永远不会被处理。
        if (cancel_token_) cancel_token_->store(true);
        UninstallEscHook();
        if (hwnd_ != nullptr) {
            ::PostMessageW(hwnd_, WM_LT_TRIM_MEMORY, 0, 0);
        }
    });

    settings_ = std::make_unique<SettingsWindow>();
    // 设置窗口只负责改配置与落盘，真正"生效"必须由这里做：
    // 热键要重新注册、鼠标钩子要按开关装拆，否则用户改完保存却毫无变化。
    settings_->SetOnSaved([this]() -> std::wstring { return ApplyConfigAfterSave(); });

    // ---- 主窗口（任务栏可见入口）----
    // 托盘是本程序原本唯一的常驻 UI。在受限环境（低完整性级别、任务栏未运行）
    // 下托盘会创建失败，用户将面对一个"没图标、没窗口、没报错"的进程。
    // 因此提供一个任务栏可见的主窗口作为可靠入口。
    main_win_ = std::make_unique<MainWindow>();
    main_win_->SetOnCommand([this](UINT cmd) { OnTrayCommand(cmd); });
    main_win_->SetOnCloseRequest([this]() { OnMainWindowCloseRequest(); });

    // ---- 划词检测 ----
    // 全局鼠标钩子只为"划词后弹出悬浮按钮"服务。
    // 因此只在悬浮按钮确实启用时才安装 —— 否则它没有任何用途，反而会：
    //   · 给系统每次鼠标事件增加一跳（对游戏有轻微影响）
    //   · 可能被反作弊程序视为可疑行为
    // 用户在设置里把功能一改成"仅快捷键"即可彻底不装这个钩子。
    // kFloatButtonReady 为 false 时按钮整体停用，钩子自然也不需要。
    if (kFloatButtonReady && ConfigManager::Instance().Get().FloatButtonEnabled()) {
        InstallMouseHook();
    } else {
        LT_INFO(L"划词检测未启用（悬浮按钮待重做定位，翻译请用 Alt+E）");
    }

    initialized_ = true;
    PerfMonitor::RecordToLog(L"初始化完成（闲置状态）");
    LT_INFO(L"初始化完成");

    // 显示主窗口，让用户能确认程序已启动
    main_win_->Show();
    SyncMainWindow();

    // 托盘失败时额外给出说明，解释为什么没有托盘图标
    if (tray_failed_) {
        ::PostMessageW(hwnd_, WM_LT_TRAY_FAILED_NOTICE, 0, 0);
    }
    return true;
}

// 同步主窗口的开关状态与提示文案
void AppController::SyncMainWindow() {
    if (!main_win_) return;

    const AppConfig& cfg = ConfigManager::Instance().Get();
    main_win_->SetFloatButtonEnabled(cfg.FloatButtonEnabled());
    main_win_->SetTrayAvailable(!tray_failed_);

    if (tray_failed_) {
        main_win_->SetStatus(
            L"托盘图标不可用（环境限制）。关闭本窗口将直接退出程序。");
    } else {
        main_win_->SetStatus(
            L"程序正在后台运行。关闭本窗口会收起到托盘，右键托盘图标可退出。");
    }
}

// 主窗口关闭请求。
// 关键区别：托盘可用时收起窗口（程序继续后台运行）；
// 托盘不可用时必须真正退出 —— 否则程序将没有任何入口，用户只能杀进程。
void AppController::OnMainWindowCloseRequest() {
    if (tray_failed_) {
        LT_INFO(L"主窗口关闭：托盘不可用，直接退出程序");
        ::PostQuitMessage(0);
        return;
    }
    LT_INFO(L"主窗口关闭：收起到托盘");
    if (main_win_) main_win_->Hide();
}

// 托盘不可用时的说明。
// 现在主窗口已经提供了可见入口，这个对话框只作为补充说明。
void AppController::ShowTrayFailureNotice() {
    LT_INFO(L"向用户显示托盘失败提示");

    const std::wstring logPath = ConfigManager::Instance().DataDir() + L"\\LightTransAI.log";
    std::wstring msg =
        L"鲤桥翻译 已启动，但无法在任务栏通知区域创建托盘图标。\n\n"
        L"程序仍在正常运行，您可以使用：\n"
        L"  · 本程序的主窗口（设置、开关、退出都在这里）\n"
        L"  · 快捷键：选中文字后按 Alt + E 翻译\n\n"
        L"托盘图标创建失败通常有两种原因：\n"
        L"  1. 程序运行在受限环境（如沙箱、低完整性级别），\n"
        L"     系统不允许向任务栏注册图标；\n"
        L"  2. 任务栏（explorer.exe）未运行或未响应。\n\n"
        L"关闭主窗口将直接退出程序。\n\n"
        L"日志文件位置：\n" + logPath;

    ::MessageBoxW(nullptr, msg.c_str(), L"鲤桥翻译 —— 托盘不可用",
                  MB_OK | MB_ICONWARNING | MB_TOPMOST);
}

std::vector<std::wstring> AppController::RegisterHotkeys() {
    const AppConfig& cfg = ConfigManager::Instance().Get();

    // 只注册"已启用"的功能对应的热键。
    // 关闭的功能连热键都不注册，这样按键会被目标程序正常接收，
    // 而不是被我们吃掉之后什么都不做。
    std::vector<HotkeySpec> specs;
    if (cfg.TranslateHotkeyEnabled()) {
        specs.push_back({ HOTKEY_ID_TRANSLATE, cfg.hk_translate_mods,
                          cfg.hk_translate_vk, L"选中翻译" });
    } else {
        LT_INFO(L"功能一已关闭或仅用悬浮按钮触发：不注册 Alt+E");
    }

    if (cfg.CaptureHotkeyEnabled()) {
        specs.push_back({ HOTKEY_ID_CAPTURE, cfg.hk_capture_mods,
                          cfg.hk_capture_vk, L"框选翻译" });
    } else {
        LT_INFO(L"功能二已关闭：不注册 Alt+Q");
    }

    if (cfg.OverlayHotkeyEnabled()) {
        specs.push_back({ HOTKEY_ID_OVERLAY, cfg.hk_overlay_mods,
                          cfg.hk_overlay_vk, L"覆盖翻译" });
    } else {
        LT_INFO(L"功能三已关闭：不注册 Alt+W");
    }

    const auto failures = hotkeys_->RegisterAll(specs);
    for (const auto& f : failures) {
        LT_WARN(L"热键注册问题: " + f);
    }
    return failures;
}

std::wstring AppController::ApplyConfigAfterSave() {
    if (!initialized_) return L"";

    const AppConfig& cfg = ConfigManager::Instance().Get();
    std::wstring warning;

    // ---- 热键：必须先全部注销再按新配置注册 ----
    // RegisterHotKey 对同一组合是幂等失败的，不注销就改键会把旧组合一直占着。
    if (hotkeys_) {
        hotkeys_->UnregisterAll();
        const auto failures = RegisterHotkeys();
        if (!failures.empty()) {
            warning = failures.front();
            LT_WARN(L"应用新热键时出现问题: " + warning);
        }
    }

    // ---- 划词悬浮按钮：按触发方式装拆鼠标钩子 ----
    // 只有"快捷键 + 悬浮按钮"或"仅悬浮按钮"才需要这个钩子；
    // 用户选"仅快捷键"时完全不装，避免白白给系统每次鼠标事件加一跳、
    // 也避免被反作弊程序误判。
    const bool wantHook = kFloatButtonReady && cfg.FloatButtonEnabled();
    if (mouse_hook_) {
        const bool hasHook = mouse_hook_->IsInstalled();
        if (wantHook && !hasHook) {
            InstallMouseHook();
        } else if (!wantHook && hasHook) {
            mouse_hook_->Uninstall();
            LT_INFO(L"划词检测已停用");
        }
    }

    // 主窗口上的勾选状态与提示文案需要跟着刷新
    SyncMainWindow();
    return warning;
}

void AppController::InstallMouseHook() {
    if (mouse_hook_ == nullptr) return;
    if (mouse_hook_->IsInstalled()) return;
    // 安装时先关闭滚轮通知，仅在悬浮按钮可见期间才开启
    mouse_hook_->SetActivityNotifyEnabled(false);
    if (!mouse_hook_->Install(hwnd_)) {
        LT_WARN(L"划词检测安装失败，可通过 Alt+E 手动触发翻译");
    }
}

void AppController::Shutdown() {
    if (!initialized_ && hwnd_ == nullptr) return;

    LT_INFO(L"正在退出……");
    PerfMonitor::RecordToLog(L"退出前");

    // 取消在途请求
    if (cancel_token_) cancel_token_->store(true);

    UninstallEscHook();

    if (mouse_hook_) mouse_hook_->Uninstall();
    if (hotkeys_)    hotkeys_->UnregisterAll();

    // 等待工作线程收尾，避免线程析构时 terminate
    if (translation_thread_.joinable()) translation_thread_.join();
    if (test_thread_.joinable())        test_thread_.join();

    if (float_btn_) float_btn_->Destroy();
    if (popup_)     popup_->Destroy();
    if (settings_)  settings_->Destroy();
    if (main_win_)  main_win_->Destroy();
    if (tray_)      tray_->Remove();

    main_win_.reset();
    settings_.reset();
    popup_.reset();
    float_btn_.reset();
    mouse_hook_.reset();
    hotkeys_.reset();
    tray_.reset();

    ui::ShutdownGraphics();

    if (hwnd_ != nullptr) {
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }

    initialized_ = false;
    if (s_instance_ == this) s_instance_ = nullptr;
}

// ================================================================ 功能一

void AppController::OnSelectionCandidate(const SelectionGesture& gesture) {
    if (!ConfigManager::Instance().Get().FloatButtonEnabled()) {
        LT_INFO(L"划词候选被忽略：功能一的悬浮按钮通道未启用");
        return;
    }
    if (busy_.load()) {
        LT_INFO(L"划词候选被忽略：正在翻译中");
        return;
    }

    // 防止探测线程堆积：快速连续划词时，同一时刻只允许一个探测在跑。
    // 否则用户快速拖动会不断派生线程，每个都做一次跨进程 UIA 调用。
    if (probe_in_flight_.exchange(true)) {
        LT_INFO(L"划词候选被忽略：上一次探测仍在进行");
        return;
    }

    const bool ibeam = IsIBeamCursor();
    LT_INFO(L"划词候选: 坐标=(" + std::to_wstring(gesture.pt.x) + L"," +
            std::to_wstring(gesture.pt.y) + L") 拖拽=" +
            (gesture.is_drag ? L"是" : L"否") + L" 双击=" +
            (gesture.is_double_click ? L"是" : L"否") + L" 工字光标=" +
            (ibeam ? L"是" : L"否"));

    // 光标是否为工字光标只是辅助判据：
    //  - 很多应用（Electron、自绘控件）用 CopyCursor 产生自定义句柄，值不等于 IDC_IBEAM
    //  - 反过来，光标是 IBeam 也不代表有选中文字（悬停时也是 IBeam）
    // 因此真正的判据是 UIA 是否探测到非空选区（正信号，且不触碰剪贴板）。
    auto payload = std::make_shared<ProbePayload>();
    payload->pt              = gesture.pt;
    payload->cursor_is_ibeam = ibeam;

    const HWND host = hwnd_;
    std::thread([this, payload, host]() {
        payload->selection_exists = TextExtractor::ProbeSelectionExists(200);
        probe_in_flight_.store(false);

        auto* out = new ProbePayload(*payload);
        if (!::PostMessageW(host, WM_LT_FLOATBTN_PROBE_DONE, 0,
                            reinterpret_cast<LPARAM>(out))) {
            delete out;
        }
    }).detach();
}

void AppController::OnFloatButtonClicked(POINT screen_pt) {
    (void)screen_pt;

    // 点击时立即取词（此刻原软件仍保有选区高亮，因为我们的按钮不抢焦点）
    const SelectionText sel = TextExtractor::GetSelectedText();
    if (!sel.found) {
        // 用弹窗给出明确提示
        const POINT anchor = screen_pt;
        if (popup_->ShowLoading(anchor)) {
            popup_->ShowError(sel.error.empty() ? L"未检测到选中文字" : sel.error);
            InstallEscHook();
        }
        return;
    }

    StartTranslation(TriggerSource::FloatButton, sel.text);
}

void AppController::OnHotkeyPressed(int id) {
    switch (id) {
        case HOTKEY_ID_TRANSLATE: {
            // 功能一被关掉时应完全不响应（热键本身也已不注册，这里是第二道防线）
            if (!ConfigManager::Instance().Get().Feature1Usable()) break;
            if (busy_.load()) {
                LT_INFO(L"已有翻译任务在进行中，忽略本次触发");
                return;
            }
            // Alt+E：先取消悬浮按钮，再取词
            DismissFloatButton();

            const SelectionText sel = TextExtractor::GetSelectedText();
            if (!sel.found) {
                POINT pt{};
                ::GetCursorPos(&pt);
                if (popup_->ShowLoading(pt)) {
                    popup_->ShowError(sel.error.empty() ? L"未检测到选中文字" : sel.error);
                    InstallEscHook();
                }
                return;
            }
            StartTranslation(TriggerSource::Hotkey, sel.text);
            break;
        }

        case HOTKEY_ID_CAPTURE:
            // 功能二：框选后弹出译文窗口
            if (!ConfigManager::Instance().Get().CaptureHotkeyEnabled()) break;
            StartCaptureFlow(/*require_boxes=*/false);
            break;

        case HOTKEY_ID_OVERLAY:
            // 功能三：框选后原位置覆盖
            if (!ConfigManager::Instance().Get().OverlayHotkeyEnabled()) break;
            // 覆盖层尚未实现：这里明确告知用户，而不是静默跑一遍却没任何显示
            if (!kFeature3Implemented) {
                LT_INFO(L"功能三尚未实现，忽略 Alt+W");
                break;
            }
            StartCaptureFlow(/*require_boxes=*/true);
            break;

        default:
            break;
    }
}

// ================================================================ 功能二 / 功能三

void AppController::StartCaptureFlow(bool require_boxes) {
    if (busy_.load()) {
        LT_INFO(L"已有任务在进行中，忽略框选触发");
        return;
    }
    if (capturer_ == nullptr) {
        capturer_ = std::make_unique<ScreenCapturer>();
    }
    if (capturer_->IsActive()) {
        LT_INFO(L"框选已在进行中");
        return;
    }

    // 收起可能存在的悬浮按钮与弹窗，避免遮挡选区
    DismissFloatButton();
    if (popup_) popup_->Destroy();

    vision_require_boxes_ = require_boxes;

    const bool ok = capturer_->Begin([this, require_boxes](const CaptureSelection& sel) {
        // 遮罩已销毁。把选区、冻结裁图与 DPI 信息带回主线程。
        auto* payload = new CaptureDonePayload();
        payload->accepted      = sel.accepted;
        payload->region        = sel.screen_rect;
        payload->require_boxes = require_boxes;
        payload->cropped       = sel.cropped;      // 拷贝（sel 即将析构）
        payload->dpi_scale     = sel.dpi_scale;
        if (!::PostMessageW(hwnd_, WM_LT_CAPTURE_DONE, 0,
                            reinterpret_cast<LPARAM>(payload))) {
            delete payload;
        }
    });

    if (!ok) {
        POINT pt{};
        ::GetCursorPos(&pt);
        if (popup_->ShowLoading(pt)) {
            popup_->ShowError(L"无法冻结屏幕，框选失败。");
            InstallEscHook();
        }
        return;
    }

    // 框选期间需要 Esc 能取消
    InstallEscHook();
}

// 框选完成 → 启动视觉识别
void AppController::StartVisionRequest(bool require_boxes) {
    if (busy_.exchange(true)) {
        LT_INFO(L"已有任务在进行中，忽略本次识别");
        return;
    }

    vision_require_boxes_ = require_boxes;

    // 功能二需要弹窗显示结果；功能三的覆盖层稍后创建，这里不弹窗
    if (!require_boxes) {
        POINT anchor{ vision_region_.right, vision_region_.bottom };
        if (!popup_->ShowLoading(anchor)) {
            busy_.store(false);
            LT_ERROR(L"无法创建翻译弹窗");
            return;
        }
        InstallEscHook();
    }

    cancel_token_ = std::make_shared<std::atomic<bool>>(false);
    PerfMonitor::RecordToLog(require_boxes ? L"覆盖翻译开始" : L"框选翻译开始");

    if (vision_thread_.joinable()) vision_thread_.join();

    // 把选区像素交给后台线程处理。
    //
    // 用 std::move 而不是拷贝：之前这里是 `BitmapBuffer croppedCopy = vision_cropped_;`，
    // 会把整张选区图再复制一份（例如 2560x1440 就是 14.7 MB），而且
    // vision_cropped_ 作为成员会一直占着这份内存到下次框选。
    // 现在像素所有权直接转移给线程，vision_cropped_ 立即变空，
    // 线程结束即释放，不再有常驻的大缓冲。
    BitmapBuffer croppedCopy = std::move(vision_cropped_);
    vision_cropped_ = BitmapBuffer{};

    const auto token = cancel_token_;
    const HWND host  = hwnd_;

    vision_thread_ = std::thread([require_boxes, token, host,
                                  croppedCopy = std::move(croppedCopy)]() {
        // ---- COM 初始化（必需）----
        // 编码走 WIC，网络走 WinHTTP，都依赖 COM。主线程的 CoInitializeEx
        // 只对主线程生效，线程之间不共享 COM 初始化状态。
        //
        // 这是"框选翻译一直报 WIC 工厂创建失败"的根因：
        // 本线程从未初始化 COM，CoCreateInstance(CLSID_WICImagingFactory)
        // 返回 CO_E_NOTINITIALIZED，而 WicFactory() 的 static 单例把这个
        // nullptr 永久缓存下来，导致之后每一次编码都失败——
        // 日志表现正是"一旦失败，之后永远失败"。
        const HRESULT hrCom = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool comHere = SUCCEEDED(hrCom);
        // RPC_E_CHANGED_MODE 表示本线程已被按别的模型初始化过，此时
        // CoInitializeEx 返回失败但 COM 本身可用，不需要也不能再 Uninitialize
        if (!comHere && hrCom != RPC_E_CHANGED_MODE) {
            LT_ERROR(L"视觉线程 COM 初始化失败 hr=0x" +
                     std::to_wstring(static_cast<unsigned long>(hrCom)));
        }
        struct ComGuard {
            bool owned = false;
            ~ComGuard() { if (owned) ::CoUninitialize(); }
        } comGuard;
        comGuard.owned = comHere;

        // 快照配置，避免与 UI 线程并发访问
        const AppConfig cfg = ConfigManager::Instance().Get();

        auto* payload = new VisionPayload();
        payload->require_boxes = require_boxes;

        std::vector<uint8_t> jpeg;
        std::string b64;

        // 直接用冻结裁图编码，不重新抓屏（保证与用户所见一致）
        if (!croppedCopy.Valid()) {
            payload->result.error_message = L"选区图像为空，无法识别。";
        } else {
            BitmapBuffer scaled;
            if (!ImageEncoder::ResizeToFit(croppedCopy, 1600, scaled)) {
                payload->result.error_message = L"图像缩放失败。";
            } else if (!ImageEncoder::EncodeJpeg(scaled, 92, jpeg)) {
                payload->result.error_message = L"图像编码失败。";
            } else {
                b64 = str::Base64Encode(jpeg);
                LT_INFO(L"图像编码完成: " + std::to_wstring(croppedCopy.width) +
                        L"x" + std::to_wstring(croppedCopy.height) + L" -> " +
                        std::to_wstring(scaled.width) + L"x" +
                        std::to_wstring(scaled.height) + L", JPEG " +
                        std::to_wstring(jpeg.size()) + L" 字节, 预估 " +
                        std::to_wstring(ImageEncoder::EstimateImageTokens(
                            scaled.width, scaled.height)) + L" token");

                // 按识别模式选择提供者
                std::unique_ptr<IRecognitionProvider> provider;
                if (cfg.recognition_mode == RecognitionModeConfig::LocalOcrReserve) {
                    provider = std::make_unique<LocalOcrProvider>();
                } else {
                    provider = std::make_unique<AiVisionProvider>(cfg);
                }
                payload->result = provider->ProcessImage(jpeg, require_boxes, *token);
            }
        }

        if (!::PostMessageW(host, WM_LT_VISION_DONE, 0,
                            reinterpret_cast<LPARAM>(payload))) {
            delete payload;
        }
    });
}

void AppController::OnVisionFinished(VisionPayload* payload) {
    std::unique_ptr<VisionPayload> guard(payload);
    if (payload == nullptr) {
        busy_.store(false);
        return;
    }

    if (cancel_token_ && cancel_token_->load()) {
        busy_.store(false);
        LT_INFO(L"视觉结果到达时任务已取消，丢弃");
        return;
    }

    const bool ok = payload->result.success;

    if (payload->require_boxes) {
        // 功能三：创建覆盖层（阶段四实现）
        if (ok) {
            LT_INFO(L"覆盖翻译结果就绪，文本块 " +
                    std::to_wstring(payload->result.blocks.size()) + L" 个");
        }
    } else {
        // 功能二：弹窗显示
        if (popup_ && popup_->IsVisible()) {
            if (ok) {
                popup_->ShowResult(payload->result.full_translation);
            } else {
                popup_->ShowError(payload->result.error_message.empty()
                                      ? L"识别失败，请稍后重试。"
                                      : payload->result.error_message);
            }
        }
    }

    busy_.store(false);
    PerfMonitor::RecordToLog(payload->require_boxes ? L"覆盖翻译结束"
                                                    : L"框选翻译结束");
}

void AppController::StartTranslation(TriggerSource source, const std::wstring& preset_text) {
    if (busy_.exchange(true)) {
        LT_INFO(L"已有翻译任务在进行中，忽略本次触发");
        return;
    }

    pending_text_   = preset_text;
    pending_source_ = source;

    // 新令牌：每次任务独立，避免旧任务的取消影响新任务
    cancel_token_ = std::make_shared<std::atomic<bool>>(false);

    // 弹出加载窗口，位置取鼠标附近
    POINT anchor{};
    ::GetCursorPos(&anchor);
    if (!popup_->ShowLoading(anchor)) {
        busy_.store(false);
        LT_ERROR(L"无法创建翻译弹窗");
        return;
    }
    InstallEscHook();

    PerfMonitor::RecordToLog(L"文本翻译开始");

    // 工作线程执行网络请求
    if (translation_thread_.joinable()) translation_thread_.join();

    const std::wstring text = preset_text;
    const auto token = cancel_token_;
    const HWND host  = hwnd_;

    translation_thread_ = std::thread([text, token, host]() {
        // 配置在 UI 线程修改，这里做一次快照，避免并发访问
        const AppConfig cfg = ConfigManager::Instance().Get();
        auto* result = new TranslationResult(
            DeepSeekClient::TranslateText(text, cfg, *token));

        if (!::PostMessageW(host, WM_LT_TRANSLATE_DONE, 0,
                            reinterpret_cast<LPARAM>(result))) {
            delete result;
        }
    });
}

void AppController::OnTranslationFinished(TranslationResult* result) {
    std::unique_ptr<TranslationResult> guard(result);
    if (result == nullptr) {
        busy_.store(false);
        return;
    }

    // 请求已被取消（用户按 Esc 关闭了弹窗）：不更新已销毁的窗口
    if (cancel_token_ && cancel_token_->load()) {
        busy_.store(false);
        LT_INFO(L"翻译结果到达时任务已取消，丢弃结果");
        return;
    }

    if (popup_ && popup_->IsVisible()) {
        if (result->success) {
            popup_->ShowResult(result->translated);
        } else {
            popup_->ShowError(result->error_message.empty()
                                  ? L"翻译失败，请稍后重试。"
                                  : result->error_message);
        }
    }

    busy_.store(false);
    PerfMonitor::RecordToLog(L"文本翻译结束");
}

// ================================================================ 测试连接

void AppController::OnTestConnectionRequested(bool use_vision) {
    if (test_thread_.joinable()) test_thread_.join();

    const HWND host = hwnd_;
    test_thread_ = std::thread([use_vision, host]() {
        const AppConfig cfg = ConfigManager::Instance().Get();
        std::atomic<bool> cancel{false};
        const TranslationResult r = DeepSeekClient::TestConnection(cfg, use_vision);

        auto* payload = new TestConnPayload();
        payload->use_vision = use_vision;
        payload->ok         = r.success;
        if (r.success) {
            payload->message = (use_vision ? L"视觉 API 连接成功（模型："
                                           : L"文本 API 连接成功（模型：") +
                               r.model_used + L"，耗时 " +
                               std::to_wstring(r.elapsed_ms) + L" ms）";
        } else {
            payload->message = r.error_message.empty() ? L"连接失败。" : r.error_message;
        }

        if (!::PostMessageW(host, WM_LT_TEST_CONN_RESULT, 0,
                            reinterpret_cast<LPARAM>(payload))) {
            delete payload;
        }
    });
}

void AppController::OnTestConnectionFinished(TestConnPayload* payload) {
    std::unique_ptr<TestConnPayload> guard(payload);
    if (payload == nullptr) return;
    if (settings_ && settings_->IsVisible()) {
        settings_->ReportTestResult(payload->ok, payload->message);
    }
    if (payload->ok) {
        LT_INFO(L"连接测试成功: " + payload->message);
    } else {
        LT_WARN(L"连接测试失败: " + payload->message);
    }
}

// ================================================================ 悬浮按钮生命周期

// 按当前配置创建或销毁悬浮按钮对象本身（与"显示/隐藏"不同）。
//
// 触发方式可以在设置里随时改动，所以这里必须能双向切换：
//   切到"仅悬浮按钮"或"两者都用" → 建出来
//   切到"仅快捷键"               → 拆掉并收起，同时卸掉鼠标钩子
void AppController::EnsureFloatButton() {
    const bool want = ConfigManager::Instance().Get().FloatButtonEnabled();

    if (want && !float_btn_) {
        float_btn_ = std::make_unique<FloatButtonWindow>();
        float_btn_->SetOnClick([this](POINT pt) { OnFloatButtonClicked(pt); });
        float_btn_->SetOnExpired([this]() { DismissFloatButton(); });
        LT_INFO(L"划词悬浮按钮已启用");
    } else if (!want && float_btn_) {
        DismissFloatButton();
        float_btn_.reset();
        LT_INFO(L"划词悬浮按钮已停用，功能一改由快捷键触发");
    }
}

void AppController::DismissFloatButton() {
    if (hwnd_ != nullptr) {
        ::KillTimer(hwnd_, TIMER_FLOATBTN_EXPIRE);
        ::KillTimer(hwnd_, TIMER_FLOATBTN_WATCH);
    }
    if (float_btn_) float_btn_->Destroy();
    // 按钮已销毁，不再需要滚轮通知
    if (mouse_hook_) mouse_hook_->SetActivityNotifyEnabled(false);
    // 键盘钩子按需安装、用完卸载。弹窗若仍在显示则必须保留（它也要响应 Esc）。
    ReleaseEscHookIfIdle();
}

// 仅当没有任何需要键盘响应的窗口时才卸载钩子。
// 钩子常驻会带来无谓的系统开销，但过早卸载会让弹窗的 Esc 失效。
void AppController::ReleaseEscHookIfIdle() {
    const bool popupVisible = (popup_ != nullptr) && popup_->IsVisible();
    const bool floatVisible = (float_btn_ != nullptr) && float_btn_->IsVisible();
    if (!popupVisible && !floatVisible) {
        UninstallEscHook();
    }
}

// ================================================================ Esc 低级键盘钩子

void AppController::InstallEscHook() {
    if (esc_hook_ != nullptr) return;

    esc_hook_ = ::SetWindowsHookExW(WH_KEYBOARD_LL, &AppController::EscHookProc,
                                    ::GetModuleHandleW(nullptr), 0);
    if (esc_hook_ == nullptr) {
        LT_WARN(L"Esc 捕获钩子安装失败，弹窗仍可通过关闭按钮关闭");
    }
}

void AppController::UninstallEscHook() {
    if (esc_hook_ != nullptr) {
        ::UnhookWindowsHookEx(esc_hook_);
        esc_hook_ = nullptr;
    }
}

LRESULT CALLBACK AppController::EscHookProc(int nCode, WPARAM wp, LPARAM lp) {
    if (nCode >= 0 && (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN) && s_instance_ != nullptr) {
        auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lp);
        if (kb != nullptr) {
            // Esc：关闭当前最上层窗口（弹窗 / 覆盖层）
            if (kb->vkCode == VK_ESCAPE) {
                // 只投递消息，绝不在钩子回调里做窗口操作
                ::PostMessageW(s_instance_->hwnd_, WM_LT_CANCEL_REQUEST, kCancelEsc, 0);
            } else if (s_instance_->float_btn_ && s_instance_->float_btn_->IsVisible()) {
                // 其它任意按键：按规范要求撤销划词悬浮按钮。
                // 仅在按钮确实可见时才投递，避免无谓的消息开销。
                ::PostMessageW(s_instance_->hwnd_, WM_LT_CANCEL_REQUEST, kCancelAnyKey, 0);
            }
        }
    }
    return ::CallNextHookEx(nullptr, nCode, wp, lp);
}

// ================================================================ 托盘命令

void AppController::ShowMemoryStatus() {
    const MemSnapshot s = PerfMonitor::Capture();
    wchar_t buf[256]{};
    swprintf_s(buf, L"内存占用：私有提交 %.2f MB / 工作集 %.2f MB",
               s.PrivateMb(), s.WorkingSetMb());
    LT_INFO(buf);

    if (tray_) tray_->SetTooltip(std::wstring(L"鲤桥翻译 —— ") + buf);
}

void AppController::OnTrayCommand(UINT cmd) {
    switch (cmd) {
        case CMD_TRAY_SETTINGS:
            if (settings_) settings_->Show(hwnd_);
            // 设置里可能改了开关状态，回来后同步主窗口
            SyncMainWindow();
            break;

        case CMD_TRAY_TOGGLE_FLOAT: {
            // 托盘菜单里的"功能一"开关：在"快捷键 + 悬浮按钮"与"仅快捷键"之间切换。
            // 之所以不是简单地开/关一个布尔值，是因为功能一现在有两条独立通道，
            // 用一个三态枚举表达才不会出现"快捷键也被一起关掉"的意外。
            AppConfig& cfg = ConfigManager::Instance().Get();
            const bool turnOff = cfg.FloatButtonEnabled();
            cfg.feature1_trigger = turnOff ? Feature1Trigger::HotkeyOnly
                                           : Feature1Trigger::Both;
            ConfigManager::Instance().Save();

            const bool floatOn = cfg.FloatButtonEnabled();
            if (tray_) tray_->SetFloatButtonEnabled(floatOn);
            if (main_win_) main_win_->SetFloatButtonEnabled(floatOn);

            if (kFloatButtonReady) EnsureFloatButton();
            if (floatOn && kFloatButtonReady) {
                InstallMouseHook();
            } else {
                if (mouse_hook_) mouse_hook_->Uninstall();
                DismissFloatButton();
            }
            LT_INFO(floatOn ? L"已开启划词悬浮翻译" : L"已关闭划词悬浮翻译");
            break;
        }

        case CMD_TRAY_FEATURE2:
            OnHotkeyPressed(HOTKEY_ID_CAPTURE);
            break;

        case CMD_TRAY_FEATURE3:
            OnHotkeyPressed(HOTKEY_ID_OVERLAY);
            break;

        case CMD_TRAY_MEMORY:
            ShowMemoryStatus();
            break;

        case CMD_TRAY_QUIT:
            ::PostQuitMessage(0);
            break;

        default:
            break;
    }
}

// ================================================================ 消息分发

LRESULT AppController::HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        // ---- 托盘 ----
        case WM_LT_TRAY:
            if (tray_) tray_->OnTrayMessage(wp, lp);
            return 0;

        // ---- 热键 ----
        case WM_HOTKEY:
            if (hotkeys_) hotkeys_->OnHotkey(static_cast<int>(wp));
            return 0;

        // ---- 托盘创建失败的可见提示 ----
        case WM_LT_TRAY_FAILED_NOTICE:
            ShowTrayFailureNotice();
            return 0;

        // ---- 显示主窗口 ----
        case WM_LT_SHOW_MAIN_WINDOW:
            if (main_win_) {
                main_win_->Show();
                SyncMainWindow();
            }
            return 0;

        // ---- 划词候选（由低级鼠标钩子投递）----
        case WM_LT_SELECTION_CAND: {
            std::unique_ptr<SelectionGesture> g(
                reinterpret_cast<SelectionGesture*>(lp));
            if (g) OnSelectionCandidate(*g);
            return 0;
        }

        // ---- 划词候选探测结果 ----
        case WM_LT_FLOATBTN_PROBE_DONE: {
            std::unique_ptr<ProbePayload> p(reinterpret_cast<ProbePayload*>(lp));
            if (!p) return 0;
            if (busy_.load()) return 0;
            if (!ConfigManager::Instance().Get().FloatButtonEnabled()) return 0;

            // 判据：UIA 探测到非空选区（正信号）优先；
            // 若无 UIA 信号但光标是工字光标，也给出机会（兼容不支持 UIA 的应用）。
            LT_INFO(L"划词探测结果: UIA有选区=" +
                    (p->selection_exists ? std::wstring(L"是") : std::wstring(L"否")) +
                    L" 工字光标=" +
                    (p->cursor_is_ibeam ? std::wstring(L"是") : std::wstring(L"否")));
            if (!p->selection_exists && !p->cursor_is_ibeam) {
                LT_INFO(L"划词探测未通过判据，不弹出悬浮按钮");
                return 0;
            }

            if (float_btn_) {
                float_btn_->Show(p->pt);
                // 3 秒无操作自动销毁
                ::SetTimer(hwnd_, TIMER_FLOATBTN_EXPIRE, 3000, nullptr);
                // 监视用户是否已在别处操作
                ::SetTimer(hwnd_, TIMER_FLOATBTN_WATCH, 120, nullptr);
                // 规范要求：按键时也要撤销悬浮按钮。
                // 键盘钩子按需安装，按钮销毁时立即卸载，避免常驻。
                InstallEscHook();
                // 按钮可见期间才需要滚轮撤销通知
                if (mouse_hook_) mouse_hook_->SetActivityNotifyEnabled(true);
            }
            return 0;
        }

        // ---- 悬浮按钮超时 ----
        case WM_TIMER: {
            if (wp == TIMER_FLOATBTN_EXPIRE) {
                ::KillTimer(hwnd_, TIMER_FLOATBTN_EXPIRE);
                DismissFloatButton();
                return 0;
            }
            if (wp == TIMER_FLOATBTN_WATCH) {
                if (float_btn_ && float_btn_->IsVisible()) {
                    // 鼠标左/右/中键在按钮外按下 → 撤销。
                    // 键盘与滚轮由钩子投递消息处理，这里只兜底鼠标按键。
                    const bool btnDown =
                        ((::GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0) ||
                        ((::GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0) ||
                        ((::GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0);
                    if (btnDown) {
                        POINT pt{};
                        ::GetCursorPos(&pt);
                        if (!float_btn_->HitTest(pt)) {
                            ::KillTimer(hwnd_, TIMER_FLOATBTN_WATCH);
                            DismissFloatButton();
                            return 0;
                        }
                    }
                } else {
                    ::KillTimer(hwnd_, TIMER_FLOATBTN_WATCH);
                }
                return 0;
            }
            if (wp == TIMER_TRIM_MEMORY) {
                ::KillTimer(hwnd_, TIMER_TRIM_MEMORY);
                // 先让堆把空闲段还给系统（这一步会降低"提交大小"），
                // 再把工作集换出。顺序不能反：_heapmin 需要触碰堆，
                // 放在 TrimWorkingSet 之后会把刚换出的页又拉回来。
                PerfMonitor::ReleaseFreeHeap();
                PerfMonitor::TrimWorkingSet();
                PerfMonitor::RecordToLog(L"弹窗关闭后回落");
                return 0;
            }
            break;
        }

        // ---- 翻译完成 ----
        case WM_LT_TRANSLATE_DONE:
            OnTranslationFinished(reinterpret_cast<TranslationResult*>(lp));
            return 0;

        // ---- 框选完成 ----
        case WM_LT_CAPTURE_DONE: {
            std::unique_ptr<CaptureDonePayload> p(
                reinterpret_cast<CaptureDonePayload*>(lp));
            if (!p) return 0;

            // 框选结束，Esc 钩子交给后续流程按需重装
            ReleaseEscHookIfIdle();

            if (!p->accepted) {
                LT_INFO(L"框选已取消");
                return 0;
            }

            // 保存选区数据供覆盖层与识别使用
            vision_region_    = p->region;
            vision_cropped_   = std::move(p->cropped);
            vision_dpi_scale_ = p->dpi_scale;

            StartVisionRequest(p->require_boxes);
            return 0;
        }

        // ---- 视觉识别完成 ----
        case WM_LT_VISION_DONE:
            OnVisionFinished(reinterpret_cast<VisionPayload*>(lp));
            return 0;

        // ---- 测试连接 ----
        case WM_LT_TEST_CONN_DONE:
            OnTestConnectionRequested(wp != 0);
            return 0;

        case WM_LT_TEST_CONN_RESULT:
            OnTestConnectionFinished(reinterpret_cast<TestConnPayload*>(lp));
            return 0;

        // ---- 键盘/鼠标活动导致的撤销 ----
        case WM_LT_CANCEL_REQUEST: {
            // 任意按键：只撤销划词悬浮按钮，不影响其它窗口
            // （规范要求"按键"时销毁按钮，但这不应顺带关掉翻译弹窗）
            if (wp == kCancelAnyKey) {
                if (float_btn_ && float_btn_->IsVisible()) {
                    DismissFloatButton();
                }
                return 0;
            }

            // Esc：关闭当前最上层窗口，优先翻译弹窗
            if (popup_ && popup_->IsVisible()) {
                if (cancel_token_) cancel_token_->store(true);
                popup_->Destroy();
                ReleaseEscHookIfIdle();
                ::SetTimer(hwnd_, TIMER_TRIM_MEMORY, 300, nullptr);
                return 0;
            }
            if (float_btn_ && float_btn_->IsVisible()) {
                DismissFloatButton();
                return 0;
            }
            if (settings_ && settings_->IsVisible()) {
                settings_->Destroy();
                ReleaseEscHookIfIdle();
                ::SetTimer(hwnd_, TIMER_TRIM_MEMORY, 300, nullptr);
                return 0;
            }
            // 没有可关闭的窗口：卸载钩子，避免常驻
            UninstallEscHook();
            return 0;
        }

        // ---- 左键按下：点按钮以外的地方就撤销悬浮按钮（事件驱动，不漏）----
        case WM_LT_MOUSE_DOWN: {
            std::unique_ptr<POINT> pt(reinterpret_cast<POINT*>(lp));
            if (float_btn_ && float_btn_->IsVisible()) {
                // 命中按钮本身时不撤销 —— 那正是用户要点击翻译。
                // 按钮带 WS_EX_NOACTIVATE，点击不会抢焦点，
                // 因此这次按下既触发按钮的 WM_LBUTTONUP，也不会破坏原选区。
                const POINT p = (pt != nullptr) ? *pt : POINT{};
                if (!float_btn_->HitTest(p)) {
                    DismissFloatButton();
                }
            }
            return 0;
        }

        // ---- 滚轮活动：撤销悬浮按钮（规范要求）----
        case WM_LT_MOUSE_ACTIVITY: {
            std::unique_ptr<POINT> pt(reinterpret_cast<POINT*>(lp));
            (void)pt;
            if (float_btn_ && float_btn_->IsVisible()) {
                DismissFloatButton();
            }
            return 0;
        }

        // ---- 回收工作集（由弹窗关闭等时机投递）----
        case WM_LT_TRIM_MEMORY: {
            // 延迟一小段再回收，等窗口真正销毁完毕
            ::SetTimer(hwnd_, TIMER_TRIM_MEMORY, 300, nullptr);
            return 0;
        }

        case WM_LT_SHOW_SETTINGS:
            if (settings_) settings_->Show(hwnd_);
            return 0;

        case WM_LT_QUIT_APP:
            ::PostQuitMessage(0);
            return 0;

        default:
            break;
    }

    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace lt
