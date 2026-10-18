#include "capture/screen_capturer.h"

#include <windowsx.h>

#include <algorithm>

#include "app/app_messages.h"
#include "utils/logger.h"
#include "utils/perf_monitor.h"

namespace lt {

namespace {

constexpr wchar_t kCaptureClass[] = L"LightTransAI_CaptureOverlay";

// 遮罩压暗强度（0~255，越小越暗）
constexpr BYTE kDimAlpha = 150;

} // namespace

bool ScreenCapturer::EnsureClassRegistered() {
    static bool registered = false;
    if (registered) return true;

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = 0;   // 不做重绘，全部由 UpdateLayeredWindow 呈现
    wc.lpfnWndProc   = &ScreenCapturer::WndProc;
    wc.hInstance     = ::GetModuleHandleW(nullptr);
    wc.hCursor       = ::LoadCursorW(nullptr, IDC_CROSS);
    wc.lpszClassName = kCaptureClass;
    wc.hbrBackground = nullptr;

    if (::RegisterClassExW(&wc) == 0) {
        const DWORD err = ::GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            LT_ERROR(L"注册框选遮罩窗口类失败，错误码 " + std::to_wstring(err));
            return false;
        }
    }
    registered = true;
    return true;
}

bool ScreenCapturer::Begin(DoneCallback done) {
    if (hwnd_ != nullptr) {
        LT_WARN(L"框选已在进行中，忽略重复触发");
        return false;
    }
    if (!EnsureClassRegistered()) return false;

    on_done_ = std::move(done);
    finished_ = false;
    dragging_ = false;
    render_error_logged_ = false;

    // 重置拖拽状态。
    // 若不重置，上一轮遗留的 drag_start_/drag_cur_ 会让"未收到 WM_LBUTTONDOWN
    // 就收到 WM_LBUTTONUP"的异常路径算出错误的选区（实测踩到过：
    // 误触测试里 drag_start_ 残留导致 5x5 被算成 289x215）。
    drag_start_ = POINT{ 0, 0 };
    drag_cur_   = POINT{ 0, 0 };

    // ---- 1) 冻结当前屏幕 ----
    if (!ImageEncoder::CaptureVirtualScreen(frozen_)) {
        LT_ERROR(L"屏幕冻结失败，无法开始框选");
        frozen_.pixels.clear();
        return false;
    }

    virt_x_ = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
    virt_y_ = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
    virt_w_ = frozen_.width;
    virt_h_ = frozen_.height;

    LT_INFO(L"屏幕已冻结: " + std::to_wstring(virt_w_) + L"x" +
            std::to_wstring(virt_h_) + L" @ (" + std::to_wstring(virt_x_) +
            L"," + std::to_wstring(virt_y_) + L")");

    // ---- 2) 准备分层窗口用的 DIB ----
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = virt_w_;
    bmi.bmiHeader.biHeight      = -virt_h_;   // 自上而下
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    HDC screen = ::GetDC(nullptr);
    mem_bmp_ = ::CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &mem_bits_, nullptr, 0);
    mem_dc_  = ::CreateCompatibleDC(screen);
    ::ReleaseDC(nullptr, screen);

    if (mem_bmp_ == nullptr || mem_dc_ == nullptr || mem_bits_ == nullptr) {
        LT_ERROR(L"创建遮罩位图失败");
        Abort();
        return false;
    }
    mem_old_ = ::SelectObject(mem_dc_, mem_bmp_);

    // ---- 3) 创建窗口 ----
    // WS_EX_LAYERED 是**必需**的：UpdateLayeredWindow 要求窗口带此样式，
    // 否则调用直接失败（ERROR_INVALID_PARAMETER），结果是遮罩与拖拽框
    // 什么都画不出来 —— 但鼠标消息仍能收到，所以框选"能工作却看不见"。
    // 这个坑很隐蔽：功能测试全过，只有肉眼看屏幕才能发现。
    //
    // WS_EX_TOOLWINDOW 不进 Alt+Tab；WS_EX_NOACTIVATE 不抢焦点
    // （焦点必须留在原程序，否则用户看不到自己的选区）。
    const DWORD exStyle = WS_EX_LAYERED | WS_EX_TOPMOST |
                          WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    hwnd_ = ::CreateWindowExW(exStyle, kCaptureClass, L"", WS_POPUP,
                              virt_x_, virt_y_, virt_w_, virt_h_,
                              nullptr, nullptr, ::GetModuleHandleW(nullptr), this);
    if (hwnd_ == nullptr) {
        LT_ERROR(L"创建框选遮罩失败，错误码 " + std::to_wstring(::GetLastError()));
        Abort();
        return false;
    }

    Render();
    ::ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    ::SetWindowPos(hwnd_, HWND_TOPMOST, virt_x_, virt_y_, virt_w_, virt_h_,
                   SWP_NOACTIVATE | SWP_SHOWWINDOW);

    // 捕获鼠标，确保拖拽到窗口外也能收到消息
    ::SetCapture(hwnd_);

    PerfMonitor::RecordToLog(L"框选开始");
    LT_INFO(L"框选遮罩已显示（Esc 或右键取消）");
    return true;
}

void ScreenCapturer::Cancel() {
    if (hwnd_ == nullptr) return;
    LT_INFO(L"框选被取消");
    Finish(false);
}

void ScreenCapturer::Abort() {
    if (hwnd_ != nullptr) {
        ::ReleaseCapture();
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (mem_dc_ != nullptr) {
        if (mem_old_) ::SelectObject(mem_dc_, mem_old_);
        ::DeleteDC(mem_dc_);
        mem_dc_ = nullptr;
    }
    if (mem_bmp_ != nullptr) {
        ::DeleteObject(mem_bmp_);
        mem_bmp_ = nullptr;
    }
    mem_old_ = nullptr;
    mem_bits_ = nullptr;

    frozen_.pixels.clear();
    frozen_.pixels.shrink_to_fit();
    frozen_.width = 0;
    frozen_.height = 0;

    on_done_ = nullptr;
}

void ScreenCapturer::Finish(bool accepted) {
    if (finished_) return;
    finished_ = true;

    CaptureSelection sel{};
    sel.accepted = accepted;
    sel.cancelled = !accepted;

    if (accepted) {
        sel.screen_rect.left   = std::min(drag_start_.x, drag_cur_.x);
        sel.screen_rect.top    = std::min(drag_start_.y, drag_cur_.y);
        sel.screen_rect.right  = std::max(drag_start_.x, drag_cur_.x);
        sel.screen_rect.bottom = std::max(drag_start_.y, drag_cur_.y);

        // 从"冻结画面"裁出选区，而不是等遮罩关闭后重新抓屏。
        //
        // 为什么必须这样：遮罩一关，底下的窗口可能立刻重绘/滚动，
        // 重新抓屏拿到的画面与用户框选时看到的**不一致**，
        // 功能三的坐标覆盖会因此错位。用冻结图裁剪才能保证所见即所得。
        RECT local{};
        local.left   = sel.screen_rect.left - virt_x_;
        local.top    = sel.screen_rect.top  - virt_y_;
        local.right  = sel.screen_rect.right - virt_x_;
        local.bottom = sel.screen_rect.bottom - virt_y_;
        if (!ImageEncoder::Crop(frozen_, local, sel.cropped)) {
            LT_WARN(L"从冻结画面裁剪选区失败");
        }

        // 冻结画面到此为止就再也用不到了（遮罩即将销毁）。
        // 主动释放这两块全屏缓冲：frozen_ 是 vector、mem_bmp_ 由 Abort 删，
        // 但这里先把 frozen_ 的 14.7 MB 还掉，让峰值更早降下来。
        frozen_.pixels.clear();
        frozen_.pixels.shrink_to_fit();
        frozen_.width  = 0;
        frozen_.height = 0;

        // 查询选区所在显示器的 DPI（多屏环境下各屏可能不同）
        POINT center{ (sel.screen_rect.left + sel.screen_rect.right) / 2,
                      (sel.screen_rect.top + sel.screen_rect.bottom) / 2 };
        HMONITOR mon = ::MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST);

        using PFN_GetDpiForMonitor = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
        static PFN_GetDpiForMonitor pfn = []() -> PFN_GetDpiForMonitor {
            HMODULE shcore = ::LoadLibraryW(L"shcore.dll");
            if (shcore == nullptr) return nullptr;
            return reinterpret_cast<PFN_GetDpiForMonitor>(
                ::GetProcAddress(shcore, "GetDpiForMonitor"));
        }();

        if (pfn != nullptr && mon != nullptr) {
            UINT dx = 96, dy = 96;
            // MDT_EFFECTIVE_DPI = 0
            if (SUCCEEDED(pfn(mon, 0, &dx, &dy)) && dx >= 48 && dx <= 480) {
                sel.monitor_dpi = dx;
            }
        }
        sel.dpi_scale = static_cast<float>(sel.monitor_dpi) / 96.0f;
    }

    // 先取出回调再清理，避免清理过程中回调重入
    DoneCallback cb = std::move(on_done_);
    on_done_ = nullptr;

    Abort();

    // 框选刚释放了两块全屏缓冲（冻结画面 + 遮罩 DIB，各约 14.7 MB）。
    // 立刻整理堆并把工作集换出，否则这两块会一直留在进程的提交量里，
    // 表现为"用过一次框选，提交量就再也回不到 2 MB"。
    PerfMonitor::ReleaseFreeHeap();
    PerfMonitor::TrimWorkingSet();
    PerfMonitor::RecordToLog(L"框选结束");

    if (cb) cb(sel);
}

void ScreenCapturer::Render() {
    if (mem_bits_ == nullptr || !frozen_.Valid()) return;

    auto* dst = static_cast<uint32_t*>(mem_bits_);
    const auto* src = reinterpret_cast<const uint32_t*>(frozen_.pixels.data());

    // 计算当前选区（相对虚拟屏幕）
    int selL = 0, selT = 0, selR = 0, selB = 0;
    bool hasSel = false;
    if (dragging_) {
        selL = std::min(drag_start_.x, drag_cur_.x) - virt_x_;
        selT = std::min(drag_start_.y, drag_cur_.y) - virt_y_;
        selR = std::max(drag_start_.x, drag_cur_.x) - virt_x_;
        selB = std::max(drag_start_.y, drag_cur_.y) - virt_y_;
        hasSel = (selR > selL && selB > selT);
    }

    const int total = virt_w_ * virt_h_;
    for (int i = 0; i < total; ++i) {
        const uint32_t px = src[i];
        const int x = i % virt_w_;
        const int y = i / virt_w_;

        const bool inSel = hasSel && x >= selL && x < selR && y >= selT && y < selB;

        if (inSel) {
            // 选区内保持原始亮度，完全不透明
            dst[i] = px | 0xFF000000u;
        } else {
            // 选区外压暗：按 kDimAlpha 把 RGB 向黑色混合
            const uint32_t b = px & 0xFF;
            const uint32_t g = (px >> 8) & 0xFF;
            const uint32_t r = (px >> 16) & 0xFF;
            const uint32_t k = 255 - kDimAlpha;
            const uint32_t nb = b * k / 255;
            const uint32_t ng = g * k / 255;
            const uint32_t nr = r * k / 255;
            dst[i] = 0xFF000000u | (nr << 16) | (ng << 8) | nb;
        }
    }

    // 选区边框（1px 白色，带一点蓝色点缀）
    if (hasSel) {
        auto put = [&](int x, int y, uint32_t color) {
            if (x >= 0 && y >= 0 && x < virt_w_ && y < virt_h_) {
                dst[y * virt_w_ + x] = color;
            }
        };
        const uint32_t kLine = 0xFFFFFFFFu;
        for (int x = selL; x < selR; ++x) {
            put(x, selT, kLine);
            put(x, selB - 1, kLine);
        }
        for (int y = selT; y < selB; ++y) {
            put(selL, y, kLine);
            put(selR - 1, y, kLine);
        }
    }

    // 呈现到分层窗口。
    POINT srcPos{ 0, 0 };
    SIZE  size{ virt_w_, virt_h_ };
    POINT dstPos{ virt_x_, virt_y_ };
    BLENDFUNCTION bf{};
    bf.BlendOp             = AC_SRC_OVER;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat         = AC_SRC_ALPHA;

    // UpdateLayeredWindow 失败是**静默**的（画面上什么都不显示），
    // 因此必须检查返回值并记录，否则会出现"功能正常但看不见"的怪象。
    ::SetLastError(0);
    const BOOL ok = ::UpdateLayeredWindow(hwnd_, nullptr, &dstPos, &size,
                                          mem_dc_, &srcPos, 0, &bf, ULW_ALPHA);
    if (!ok) {
        const DWORD err = ::GetLastError();
        // 只记一次，避免鼠标移动时刷屏
        if (!render_error_logged_) {
            render_error_logged_ = true;
            LT_ERROR(L"UpdateLayeredWindow 失败，错误码 " + std::to_wstring(err) +
                     (err == ERROR_INVALID_PARAMETER
                          ? L"（常见原因：窗口缺少 WS_EX_LAYERED 样式）"
                          : L""));
        }
    }
}

LRESULT CALLBACK ScreenCapturer::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        auto* self = reinterpret_cast<ScreenCapturer*>(cs->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                            reinterpret_cast<LONG_PTR>(self));
        // 创建期间即绑定句柄（详见 main_window.cpp 的说明）。
        if (self != nullptr) self->hwnd_ = hwnd;
        return ::DefWindowProcW(hwnd, msg, wp, lp);
    }
    auto* self = reinterpret_cast<ScreenCapturer*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self == nullptr) return ::DefWindowProcW(hwnd, msg, wp, lp);
    return self->HandleMessage(msg, wp, lp);
}

LRESULT ScreenCapturer::HandleMessage(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_LBUTTONDOWN: {
            dragging_ = true;
            drag_start_.x = GET_X_LPARAM(lp) + virt_x_;
            drag_start_.y = GET_Y_LPARAM(lp) + virt_y_;
            drag_cur_ = drag_start_;
            Render();
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (dragging_) {
                drag_cur_.x = GET_X_LPARAM(lp) + virt_x_;
                drag_cur_.y = GET_Y_LPARAM(lp) + virt_y_;
                Render();
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            if (!dragging_) return 0;
            dragging_ = false;

            drag_cur_.x = GET_X_LPARAM(lp) + virt_x_;
            drag_cur_.y = GET_Y_LPARAM(lp) + virt_y_;

            const int w = std::abs(drag_cur_.x - drag_start_.x);
            const int h = std::abs(drag_cur_.y - drag_start_.y);

            // 误触判定：规范要求宽或高 < 10px 视为误触
            if (w < MinSelectionSize() || h < MinSelectionSize()) {
                LT_INFO(L"选区过小（" + std::to_wstring(w) + L"x" +
                        std::to_wstring(h) + L"），视为误触，已取消");
                Finish(false);
                return 0;
            }

            LT_INFO(L"框选完成: " + std::to_wstring(w) + L"x" + std::to_wstring(h));
            Finish(true);
            return 0;
        }

        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN: {
            LT_INFO(L"框选被鼠标右键/中键取消");
            Finish(false);
            return 0;
        }

        case WM_KEYDOWN: {
            if (wp == VK_ESCAPE) {
                LT_INFO(L"框选被 Esc 取消");
                Finish(false);
            }
            return 0;
        }

        case WM_SETCURSOR: {
            ::SetCursor(::LoadCursorW(nullptr, IDC_CROSS));
            return TRUE;
        }

        case WM_DESTROY:
            hwnd_ = nullptr;
            return 0;

        default:
            break;
    }
    return ::DefWindowProcW(hwnd_, msg, wp, lp);
}

} // namespace lt
