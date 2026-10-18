#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include <string>

namespace lt {
namespace ui {

// ---- 进程级图形工厂（懒加载；ShutdownGraphics 在退出时释放）----
ID2D1Factory*   D2DFactory();
IDWriteFactory* DWriteFactory();
void            ShutdownGraphics();

// ---- DPI ----
float DpiScale(HWND hwnd);                     // 1.0 = 96 DPI
int   ScaleForDpi(int value, HWND hwnd);       // 把 DIP 值按窗口 DPI 换算为像素

// ---- 窗口外观 ----
void ApplyRoundedRegion(HWND hwnd, int radius_px);   // 圆角窗口区域（各版本通用）
void ApplyDwmRoundCorners(HWND hwnd);                // Win11 原生圆角
void ApplyDwmBorderlessShadow(HWND hwnd);            // 无边框窗口的投影

// ---- 渲染目标 ----
// 创建与窗口客户区匹配的 HwndRenderTarget；失败返回 nullptr
ID2D1HwndRenderTarget* CreateHwndRenderTarget(HWND hwnd);

// 调整已有渲染目标尺寸；必要时重建。成功返回可用的目标
ID2D1HwndRenderTarget* ResizeRenderTarget(ID2D1HwndRenderTarget* rt, HWND hwnd,
                                          UINT width, UINT height);

// ---- 文本 ----
// 创建文本格式。family 为空时使用「Microsoft YaHei UI」并回退「Segoe UI」
IDWriteTextFormat* CreateTextFormat(float size_dip,
                                    DWRITE_FONT_WEIGHT weight,
                                    DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING,
                                    const wchar_t* family = nullptr,
                                    DWRITE_PARAGRAPH_ALIGNMENT valign = DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

// ---- 定位 ----
// 把 (x, y, w, h) 矩形约束到光标所在显示器的工作区内，避免弹窗溢出屏幕
POINT ClampToWorkArea(int x, int y, int w, int h);

// 把尺寸为 w x h 的小窗口摆到锚点右侧（垂直居中对齐锚点），并保证不超出工作区。
//
// 与 ClampToWorkArea 的区别：本函数在右侧放不下时会**翻到锚点左侧**，
// 而不是简单地贴住右边界 —— 贴边会让按钮盖住用户刚选中的文字。
// flipped_x 回传是否发生了翻转，供调用方镜像投影方向。
POINT PlaceRightOfAnchor(POINT anchor, int w, int h, int gap, bool* flipped_x);

// ---- 分层窗口渲染（逐像素 alpha）----
//
// 为什么需要它：ID2D1HwndRenderTarget **不支持**逐像素 alpha 合成到屏幕，
// 配 WS_EX_LAYERED 也只会得到不透明背景。需要投影、圆角透明、
// 原位置覆盖这类效果时，必须走"渲染到 32bpp DIB → UpdateLayeredWindow"。
//
// 用法：
//   LayeredCanvas canvas;
//   canvas.Ensure(hwnd);              // 窗口必须带 WS_EX_LAYERED
//   if (canvas.BeginDraw()) {
//       ... 用 canvas.Rt() 绘制 ...
//       canvas.EndDrawAndPresent();   // 提交到屏幕
//   }
class LayeredCanvas {
public:
    LayeredCanvas() = default;
    ~LayeredCanvas() { Release(); }

    LayeredCanvas(const LayeredCanvas&) = delete;
    LayeredCanvas& operator=(const LayeredCanvas&) = delete;

    // 准备与窗口客户区等大的 DIB 与 DC 渲染目标
    bool Ensure(HWND hwnd);

    // 尺寸变化时重建
    bool Resize(HWND hwnd, UINT width, UINT height);

    ID2D1DCRenderTarget* Rt() const { return rt_; }

    // 开始绘制（自动 BindDC 并清空为全透明）
    bool BeginDraw(HWND hwnd);

    // 结束绘制并呈现到窗口
    bool EndDrawAndPresent(HWND hwnd, int x, int y);

    void Release();

private:
    HDC                  memDc_  = nullptr;
    HBITMAP              memBmp_ = nullptr;
    HGDIOBJ              memOld_ = nullptr;
    void*                bits_   = nullptr;
    int                  width_  = 0;
    int                  height_ = 0;
    ID2D1DCRenderTarget* rt_     = nullptr;
};

} // namespace ui
} // namespace lt
