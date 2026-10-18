#pragma once

#include <windows.h>

#include <functional>

#include "capture/image_encoder.h"

namespace lt {

// 框选结果
struct CaptureSelection {
    bool         accepted = false;   // 用户确认了选区
    RECT         screen_rect{};      // 选区在虚拟屏幕上的物理像素坐标
    bool         cancelled = false;  // 用户取消（Esc / 右键 / 选区过小）
    BitmapBuffer cropped;            // 从"冻结画面"裁出的选区位图（BGRA）

    // 多显示器 DPI 信息：选区所在显示器与主显示器的缩放比。
    // 覆盖层绘制时需要它把 DIP 与物理像素互相换算。
    UINT  monitor_dpi   = 96;
    float dpi_scale     = 1.0f;
};

// 全屏框选遮罩窗口。
//
// 交互：
//   1. 触发瞬间抓取整个虚拟屏幕作为"冻结"背景（用户看到画面静止）
//   2. 覆盖一层半透明暗色遮罩 + 十字光标
//   3. 用户按住左键拖拽出选区；选区内部显示原亮度，外部压暗
//   4. Esc 或鼠标右键取消
//   5. 松开左键完成（宽或高 < 10px 视为误触，直接取消）
//
// 实现要点：
//   - 用 UpdateLayeredWindow 呈现整屏位图，避免闪烁
//   - 遮罩窗口不抢焦点（WS_EX_NOACTIVATE），Esc 由低级键盘钩子捕获
class ScreenCapturer {
public:
    using DoneCallback = std::function<void(const CaptureSelection&)>;

    ScreenCapturer() = default;
    ~ScreenCapturer() { Abort(); }

    ScreenCapturer(const ScreenCapturer&) = delete;
    ScreenCapturer& operator=(const ScreenCapturer&) = delete;

    // 开始框选。done 在用户完成或取消时被调用（在主线程）。
    // 返回 false 表示无法开始（例如抓屏失败）。
    bool Begin(DoneCallback done);

    // 是否正在框选
    bool IsActive() const { return hwnd_ != nullptr; }

    // 取消当前框选（供 Esc 钩子调用）
    void Cancel();

    // 强制结束并清理（不做回调）
    void Abort();

    static bool EnsureClassRegistered();

    // 选区最小尺寸（像素），小于此值视为误触
    static int MinSelectionSize() { return 10; }

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);

    void Render();
    void Finish(bool accepted);

    HWND         hwnd_ = nullptr;
    DoneCallback on_done_;

    // 冻结的屏幕位图（整屏，虚拟屏幕坐标系）
    BitmapBuffer frozen_;
    // 用于 UpdateLayeredWindow 的 DIB 与 DC
    HDC          mem_dc_   = nullptr;
    HBITMAP      mem_bmp_  = nullptr;
    HGDIOBJ      mem_old_  = nullptr;
    void*        mem_bits_ = nullptr;
    int          virt_x_   = 0;
    int          virt_y_   = 0;
    int          virt_w_   = 0;
    int          virt_h_   = 0;

    bool  dragging_ = false;
    POINT drag_start_{};
    POINT drag_cur_{};
    bool  finished_ = false;
    // UpdateLayeredWindow 失败只记录一次，避免鼠标移动时刷屏
    bool  render_error_logged_ = false;
};

} // namespace lt
