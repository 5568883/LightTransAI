#pragma once

#include <windows.h>

#include <string>

#include "capture/image_encoder.h"   // BitmapBuffer
#include "recognition/i_recognizer.h"

namespace lt {

// ---- 应用内部消息（由宿主隐藏窗口接收） ----
constexpr UINT WM_LT_TRAY           = WM_APP + 1;   // 托盘图标回调
constexpr UINT WM_LT_SELECTION_CAND = WM_APP + 2;   // 划词候选（低级鼠标钩子投递，LPARAM=屏幕坐标）
constexpr UINT WM_LT_FLOATBTN_CLICK = WM_APP + 3;   // 悬浮按钮被点击
constexpr UINT WM_LT_FLOATBTN_EXPIRED = WM_APP + 4; // 悬浮按钮超时/失效
constexpr UINT WM_LT_TRANSLATE_BEGIN = WM_APP + 5;  // 开始翻译（LPARAM=触发来源）
constexpr UINT WM_LT_TRANSLATE_DONE  = WM_APP + 6;  // 翻译完成（LPARAM=TranslationResult*）
constexpr UINT WM_LT_CANCEL_REQUEST  = WM_APP + 7;  // 请求取消当前任务
constexpr UINT WM_LT_SHOW_SETTINGS   = WM_APP + 8;  // 打开设置窗口
constexpr UINT WM_LT_TEST_CONN_DONE  = WM_APP + 9;  // 测试连接完成（LPARAM=TestConnPayload*）
constexpr UINT WM_LT_QUIT_APP        = WM_APP + 10; // 退出程序
constexpr UINT WM_LT_TRAY_SHOW_MEM   = WM_APP + 11; // 显示内存状态
constexpr UINT WM_LT_UIA_PROBE_DONE  = WM_APP + 12; // UIA 选区探测完成（LPARAM=ProbePayload*）
constexpr UINT WM_LT_POPUP_CLOSED    = WM_APP + 13; // 弹窗已销毁
constexpr UINT WM_LT_TEST_CONN_RESULT = WM_APP + 14; // 测试连接结果（LPARAM=TestConnPayload*）
constexpr UINT WM_LT_FLOATBTN_PROBE_DONE = WM_APP + 15; // 悬浮按钮弹出判定完成
constexpr UINT WM_LT_TRIM_MEMORY     = WM_APP + 16; // 请求回收工作集（须用独立消息，不能借用 TIMER ID）
constexpr UINT WM_LT_SELECTION_RAW   = WM_APP + 17; // 划词原始手势（由钩子投递，LPARAM=SelectionGesture*）
constexpr UINT WM_LT_MOUSE_ACTIVITY  = WM_APP + 18; // 滚轮/其它按键等鼠标活动（LPARAM=POINT*，用于撤销悬浮按钮）
constexpr UINT WM_LT_TRAY_FAILED_NOTICE = WM_APP + 19; // 托盘创建失败，提示用户
constexpr UINT WM_LT_SHOW_MAIN_WINDOW = WM_APP + 20; // 显示主窗口（单实例激活等）
constexpr UINT WM_LT_CAPTURE_DONE    = WM_APP + 21; // 框选完成（LPARAM=CaptureDonePayload*）
constexpr UINT WM_LT_VISION_DONE     = WM_APP + 22; // 视觉识别完成（LPARAM=VisionPayload*）
constexpr UINT WM_LT_MOUSE_DOWN      = WM_APP + 23; // 鼠标左键按下（LPARAM=POINT*，用于撤销悬浮按钮）

// WM_LT_CANCEL_REQUEST 的 wParam 语义
constexpr WPARAM kCancelEsc     = 0;  // Esc：关闭当前最上层窗口
constexpr WPARAM kCancelAnyKey  = 1;  // 任意按键：只撤销悬浮按钮，不影响其它窗口

// ---- 划词候选判定载荷（由工作线程投递给宿主窗口，接收方负责 delete）----
struct ProbePayload {
    POINT pt{};                    // 鼠标松开位置（屏幕坐标）
    bool  selection_exists = false; // UIA 是否探测到非空选区
    bool  cursor_is_ibeam  = false; // 光标是否为工字光标（辅助判据）
};

// ---- 测试连接结果载荷（由工作线程投递，接收方负责 delete）----
struct TestConnPayload {
    bool         use_vision = false;
    bool         ok = false;
    std::wstring message;
};

// ---- 框选完成载荷（由遮罩窗口回调路径投递，接收方负责 delete）----
struct CaptureDonePayload {
    RECT         region{};          // 选区（虚拟屏幕物理像素坐标）
    bool         accepted = false;
    bool         require_boxes = false;   // true=功能三（覆盖），false=功能二（弹窗）
    BitmapBuffer cropped;                 // 从冻结画面裁出的选区（BGRA）
    float        dpi_scale = 1.0f;        // 选区所在显示器的缩放比
};

// ---- 视觉识别结果载荷（由工作线程投递，接收方负责 delete）----
struct VisionPayload {
    bool              require_boxes = false;
    RECT              region{};
    RecognitionResult result;
};

// ---- 定时器 ID ----
constexpr UINT_PTR TIMER_FLOATBTN_EXPIRE = 0x1001;  // 悬浮按钮 3 秒自动销毁
constexpr UINT_PTR TIMER_FLOATBTN_WATCH  = 0x1002;  // 悬浮按钮失效监视（点击别处/滚轮/按键）
constexpr UINT_PTR TIMER_UIA_PROBE       = 0x1003;  // 延迟探测 UIA 选区
constexpr UINT_PTR TIMER_POPUP_ANIM      = 0x1004;  // 弹窗加载动画
constexpr UINT_PTR TIMER_TRIM_MEMORY     = 0x1005;  // 空闲后回收工作集

// ---- 托盘菜单命令 ID ----
constexpr UINT CMD_TRAY_SETTINGS      = 40001;
constexpr UINT CMD_TRAY_TOGGLE_FLOAT  = 40002;
constexpr UINT CMD_TRAY_FEATURE2      = 40003;
constexpr UINT CMD_TRAY_FEATURE3      = 40004;
constexpr UINT CMD_TRAY_MEMORY        = 40005;
constexpr UINT CMD_TRAY_QUIT          = 40006;

// ---- 热键 ID ----
constexpr int HOTKEY_ID_TRANSLATE = 0x2001;  // 功能一：Alt+E
constexpr int HOTKEY_ID_CAPTURE   = 0x2002;  // 功能二：Alt+Q（阶段三）
constexpr int HOTKEY_ID_OVERLAY   = 0x2003;  // 功能三：Alt+W（阶段四）

// ---- 宿主窗口类名 ----
constexpr wchar_t kHostWindowClass[]     = L"LightTransAI_Host";
// 类名带版本号：外观改动后旧实例注册的类不会被复用（同一进程内注册同名类会失败）
constexpr wchar_t kFloatButtonClass[]    = L"LightTransAI_FloatButton_v2";
constexpr wchar_t kPopupWindowClass[]    = L"LightTransAI_Popup";
constexpr wchar_t kSettingsWindowClass[] = L"LightTransAI_Settings";

// ---- 资源 ID ----
// 与 app.rc 里的 `101 ICON "app.ico"` 必须一致
constexpr int kAppIconId = 101;

// ---- 触发来源 ----
enum class TriggerSource {
    FloatButton = 0,  // 划词后点击悬浮按钮
    Hotkey      = 1,  // Alt+E 兜底热键
};

} // namespace lt
