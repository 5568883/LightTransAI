// 鲤桥翻译 (LightTransAI) —— 程序入口
//
// 职责：
//  - 单实例互斥体（防止重复启动导致热键/钩子冲突）
//  - DPI 感知初始化（必须在创建任何窗口之前）
//  - COM 初始化（UIAutomation / WIC / Direct2D 需要）
//  - 消息循环（纯事件驱动，零轮询）

#include <windows.h>
#include <objbase.h>
#include <commctrl.h>        // INITCOMMONCONTROLSEX / InitCommonControlsEx
#include <shellscalingapi.h>

#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "comctl32.lib")

#include <memory>

#include "app/app_controller.h"
#include "utils/logger.h"

namespace {

constexpr wchar_t kMutexName[] = L"LightTransAI_SingleInstance_Mutex_v1";

// 单实例守卫
class SingleInstanceGuard {
public:
    bool Acquire() {
        // 先尝试打开已存在的互斥体：成功说明已有实例在运行
        HANDLE existing = ::OpenMutexW(SYNCHRONIZE, FALSE, kMutexName);
        if (existing != nullptr) {
            ::CloseHandle(existing);
            return false;
        }

        mutex_ = ::CreateMutexW(nullptr, TRUE, kMutexName);
        if (mutex_ == nullptr) {
            return false;
        }
        if (::GetLastError() == ERROR_ALREADY_EXISTS) {
            ::CloseHandle(mutex_);
            mutex_ = nullptr;
            return false;
        }
        return true;
    }

    ~SingleInstanceGuard() {
        if (mutex_ != nullptr) {
            ::ReleaseMutex(mutex_);
            ::CloseHandle(mutex_);
            mutex_ = nullptr;
        }
    }

private:
    HANDLE mutex_ = nullptr;
};

// 把已有实例的托盘图标提示一下，让用户知道程序已在运行
void NotifyExistingInstance() {
    ::MessageBoxW(nullptr,
                  L"鲤桥翻译 已经在运行中。\n\n请在任务栏通知区域（系统托盘）找到「译」图标进行操作。",
                  L"鲤桥翻译", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/,
                    PWSTR /*lpCmdLine*/, int /*nCmdShow*/) {
    // ---- 单实例检查（必须在最前面）----
    SingleInstanceGuard guard;
    if (!guard.Acquire()) {
        NotifyExistingInstance();
        return 0;
    }

    // ---- DPI 感知：必须在创建任何窗口之前设置 ----
    // 优先使用 Per-Monitor V2（Win10 1703+）
    bool dpiOk = false;
    if (HMODULE user32 = ::GetModuleHandleW(L"user32.dll")) {
        using PFN_SetCtx = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
        auto pfn = reinterpret_cast<PFN_SetCtx>(
            ::GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (pfn != nullptr) {
            // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4
            dpiOk = pfn(reinterpret_cast<DPI_AWARENESS_CONTEXT>(-4)) != FALSE;
        }
    }
    if (!dpiOk) {
        // 回退：Win8.1 的 Per-Monitor
        ::SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);
    }

    // ---- COM：UIAutomation 与 WIC 都需要 ----
    const HRESULT hrCom = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hrCom) && hrCom != RPC_E_CHANGED_MODE) {
        ::MessageBoxW(nullptr, L"COM 初始化失败，程序无法启动。",
                      L"鲤桥翻译", MB_OK | MB_ICONERROR);
        return 1;
    }

    // 通用控件初始化（设置窗口的字体与视觉样式）
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_STANDARD_CLASSES | ICC_BAR_CLASSES;
    ::InitCommonControlsEx(&icc);

    int exitCode = 0;

    {
        auto controller = std::make_unique<lt::AppController>();

        if (!controller->Initialize(hInstance)) {
            ::MessageBoxW(nullptr,
                          L"鲤桥翻译 初始化失败。\n\n请查看日志了解详情：\n%APPDATA%\\LightTransAI\\LightTransAI.log",
                          L"鲤桥翻译", MB_OK | MB_ICONERROR);
            exitCode = 1;
        } else {
            // ---- 消息循环：纯事件驱动，无任何轮询 ----
            MSG msg{};
            while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
                // 宿主窗口是 HWND_MESSAGE，不参与 TranslateMessage/DispatchMessage
                // 之外的任何 UI 处理
                ::TranslateMessage(&msg);
                ::DispatchMessageW(&msg);
            }
            exitCode = static_cast<int>(msg.wParam);
        }

        controller->Shutdown();
    }

    if (SUCCEEDED(hrCom)) {
        ::CoUninitialize();
    }

    return exitCode;
}
