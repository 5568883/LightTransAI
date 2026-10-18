#include "input/text_extractor.h"

#include <UIAutomation.h>
#include <shellapi.h>
#include <shlobj.h>   // DROPFILES 结构（CF_HDROP 还原需要）

#pragma comment(lib, "uiautomationcore.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "app/app_messages.h"
#include "utils/logger.h"
#include "utils/string_utils.h"

namespace lt {

namespace {

// ---------------------------------------------------------------- 剪贴板备份

// 一份剪贴板快照。只覆盖可忠实还原的格式。
struct ClipboardBackup {
    bool                        valid = false;
    bool                        hadText = false;
    std::wstring                text;
    bool                        hadBitmap = false;
    std::vector<uint8_t>        bitmap;   // 序列化后的 BITMAPINFO + 像素
    bool                        hadDrop = false;
    std::vector<std::wstring>   files;

    void Clear() {
        valid = hadText = hadBitmap = hadDrop = false;
        text.clear();
        bitmap.clear();
        files.clear();
    }
};

bool OpenClipboardRetry(HWND owner, int attempts = 10) {
    for (int i = 0; i < attempts; ++i) {
        if (::OpenClipboard(owner)) return true;
        ::Sleep(10);
    }
    return false;
}

void BackupClipboard(ClipboardBackup& backup) {
    backup.Clear();

    if (!OpenClipboardRetry(nullptr)) return;

    // CF_UNICODETEXT
    if (HANDLE h = ::GetClipboardData(CF_UNICODETEXT)) {
        if (auto* p = static_cast<const wchar_t*>(::GlobalLock(h))) {
            backup.text = p;
            backup.hadText = true;
            ::GlobalUnlock(h);
        }
    }

    // CF_HDROP（文件列表）
    if (HANDLE h = ::GetClipboardData(CF_HDROP)) {
        if (auto hd = static_cast<HDROP>(h)) {
            const UINT count = ::DragQueryFileW(hd, 0xFFFFFFFF, nullptr, 0);
            if (count > 0) {
                for (UINT i = 0; i < count; ++i) {
                    const UINT len = ::DragQueryFileW(hd, i, nullptr, 0);
                    if (len == 0) continue;
                    std::wstring path(len + 1, L'\0');
                    ::DragQueryFileW(hd, i, path.data(), len + 1);
                    path.resize(len);
                    backup.files.push_back(path);
                }
                backup.hadDrop = !backup.files.empty();
            }
        }
    }

    backup.valid = true;
    ::CloseClipboard();
}

// 尽力还原剪贴板。返回是否完全还原。
bool RestoreClipboard(const ClipboardBackup& backup) {
    if (!backup.valid) return false;

    if (!OpenClipboardRetry(nullptr)) return false;

    ::EmptyClipboard();
    bool ok = true;

    if (backup.hadText) {
        const size_t bytes = (backup.text.size() + 1) * sizeof(wchar_t);
        if (HGLOBAL h = ::GlobalAlloc(GMEM_MOVEABLE, bytes)) {
            if (void* p = ::GlobalLock(h)) {
                memcpy(p, backup.text.c_str(), bytes);
                ::GlobalUnlock(h);
                if (!::SetClipboardData(CF_UNICODETEXT, h)) {
                    ::GlobalFree(h);
                    ok = false;
                }
            } else {
                ::GlobalFree(h);
                ok = false;
            }
        } else {
            ok = false;
        }
    }

    if (backup.hadDrop && !backup.files.empty()) {
        // 构造 DROPFILES 结构
        size_t chars = 0;
        for (const auto& f : backup.files) chars += f.size() + 1;
        chars += 1;  // 末尾双 null

        const size_t headerSize = sizeof(DROPFILES);
        const size_t total = headerSize + chars * sizeof(wchar_t);

        if (HGLOBAL h = ::GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, total)) {
            if (auto* df = static_cast<DROPFILES*>(::GlobalLock(h))) {
                df->pFiles = static_cast<DWORD>(headerSize);
                df->fWide  = TRUE;
                auto* dst = reinterpret_cast<wchar_t*>(
                    reinterpret_cast<uint8_t*>(df) + headerSize);
                for (const auto& f : backup.files) {
                    memcpy(dst, f.c_str(), (f.size() + 1) * sizeof(wchar_t));
                    dst += f.size() + 1;
                }
                *dst = L'\0';
                ::GlobalUnlock(h);
                if (!::SetClipboardData(CF_HDROP, h)) {
                    ::GlobalFree(h);
                    ok = false;
                }
            } else {
                ::GlobalFree(h);
                ok = false;
            }
        } else {
            ok = false;
        }
    }

    // 注意：CF_BITMAP / HTML / RTF 等格式未做还原。
    // 位图句柄语义（所有权转移）与延迟渲染格式无法安全复制，
    // 强行处理反而可能破坏用户原有数据，故有意跳过并在日志中说明。
    if (!backup.hadText && !backup.hadDrop) {
        ok = true;  // 原本没有可还原内容
    }

    ::CloseClipboard();
    return ok;
}

// ---------------------------------------------------------------- 剪贴板更新监听

// 消息专用窗口，用于接收 WM_CLIPBOARDUPDATE
class ClipListener {
public:
    bool EnsureCreated() {
        if (hwnd_ != nullptr) return true;
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = &ClipListener::WndProc;
        wc.hInstance     = ::GetModuleHandleW(nullptr);
        wc.lpszClassName = L"LightTransAI_ClipListener";
        ::RegisterClassExW(&wc);  // 已注册时返回 0，无害

        hwnd_ = ::CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                                  HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
        return hwnd_ != nullptr;
    }

    void Destroy() {
        if (hwnd_ != nullptr) {
            ::DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
    }

    HWND Handle() const { return hwnd_; }

    void Reset() { updated_ = false; }
    bool Updated() const { return updated_; }
    void MarkUpdated() { updated_ = true; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_CLIPBOARDUPDATE) {
            if (auto* self = reinterpret_cast<ClipListener*>(
                    ::GetWindowLongPtrW(hwnd, GWLP_USERDATA))) {
                self->MarkUpdated();
            }
            return 0;
        }
        return ::DefWindowProcW(hwnd, msg, wp, lp);
    }

public:
    // 需要在创建后绑定 this
    void Bind() {
        if (hwnd_) ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA,
                                       reinterpret_cast<LONG_PTR>(this));
    }

private:
    HWND hwnd_ = nullptr;
    volatile bool updated_ = false;
};

// 发送 Ctrl+C。
// 返回是否成功注入全部按键事件。
//
// 两个关键点（都是实测踩出来的）：
//  1. 必须先显式释放 Alt/Ctrl/Shift/Win。
//     用户是按 Alt+E 触发的，此时 Alt 可能仍处于按下状态，
//     直接发 Ctrl+C 会变成 Ctrl+Alt+C，目标程序不会执行复制。
//     实测日志："Ctrl+C 后序列号 161 -> 161（未变化，Ctrl+C 未生效）"。
//  2. 同时携带虚拟键码与扫描码。部分程序（尤其游戏与自绘控件）
//     只认扫描码，缺少扫描码的注入会被忽略。
bool SendCtrlC() {
    std::vector<INPUT> inputs;

    auto addKey = [&inputs](WORD vk, DWORD extraFlags) {
        INPUT in{};
        in.type       = INPUT_KEYBOARD;
        in.ki.wVk     = vk;
        in.ki.wScan   = static_cast<WORD>(::MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
        // 同时给出 wVk 与 wScan，兼容只认其中一种的目标程序
        in.ki.dwFlags = extraFlags;
        inputs.push_back(in);
    };

    // ---- 第 1 步：显式释放所有可能干扰的修饰键 ----
    addKey(VK_MENU,    KEYEVENTF_KEYUP);   // Alt
    addKey(VK_LMENU,   KEYEVENTF_KEYUP);
    addKey(VK_RMENU,   KEYEVENTF_KEYUP);
    addKey(VK_SHIFT,   KEYEVENTF_KEYUP);
    addKey(VK_LWIN,    KEYEVENTF_KEYUP);
    addKey(VK_RWIN,    KEYEVENTF_KEYUP);
    addKey(VK_CONTROL, KEYEVENTF_KEYUP);
    addKey(VK_LCONTROL, KEYEVENTF_KEYUP);
    addKey(VK_RCONTROL, KEYEVENTF_KEYUP);

    // ---- 第 2 步：Ctrl+C ----
    addKey(VK_CONTROL, 0);
    addKey('C', 0);
    addKey('C', KEYEVENTF_KEYUP);
    addKey(VK_CONTROL, KEYEVENTF_KEYUP);

    ::SetLastError(0);
    const UINT inserted = ::SendInput(static_cast<UINT>(inputs.size()),
                                      inputs.data(), sizeof(INPUT));
    if (inserted != inputs.size()) {
        LT_WARN(L"SendInput 未能注入全部按键（" + std::to_wstring(inserted) +
                L"/" + std::to_wstring(inputs.size()) + L"），错误码 " +
                std::to_wstring(::GetLastError()) +
                L"。目标窗口可能以更高权限运行。");
        return false;
    }

    // 给目标程序一点处理时间
    ::Sleep(30);
    return true;
}

// ---------------------------------------------------------------- UIA 工作线程

// HRESULT 转十六进制字符串（避免依赖 swprintf 的额外头文件）
std::wstring HrStr(HRESULT hr) {
    static const wchar_t* kHex = L"0123456789ABCDEF";
    const unsigned v = static_cast<unsigned>(hr);
    wchar_t buf[11]{};
    buf[0] = L'0';
    buf[1] = L'x';
    for (int i = 0; i < 8; ++i) {
        buf[2 + i] = kHex[(v >> ((7 - i) * 4)) & 0xF];
    }
    return std::wstring(buf, 10);
}

struct UiaProbeState {
    std::wstring text;
    bool         found = false;
    std::atomic<bool> done{false};
    std::wstring diag;   // 诊断：失败卡在哪一步（用于定位取词问题）
};

// 上一轮 UIA 探测是否仍在后台运行。
//
// 为什么需要它：UIA 读取超时后工作线程是 detached 的，仍会继续在目标窗口上
// 做跨进程调用。若此时立刻走剪贴板通道，目标窗口的 UI 线程正忙于应付 UIA 遍历，
// 来不及处理注入的 Ctrl+C —— 实测表现为"单独测剪贴板成功(94ms)，
// 但在完整链路里失败（序列号不变）"。
// 因此剪贴板通道启动前，先等上一轮 UIA 探测真正结束（或给一个上限）。
std::atomic<bool> g_uia_probe_running{false};

// 尝试从单个元素读取 TextPattern 选区。
// 返回：true=取到非空文本；false=该元素不可用（原因写入 why）
bool TryTextPatternFrom(IUIAutomationElement* element, bool readText,
                        std::wstring& outText, std::wstring& why) {
    outText.clear();
    why.clear();
    if (element == nullptr) { why = L"元素为空"; return false; }

    IUIAutomationTextPattern* tp = nullptr;
    const HRESULT hr = element->GetCurrentPatternAs(UIA_TextPatternId, IID_PPV_ARGS(&tp));
    if (FAILED(hr) || tp == nullptr) {
        why = L"不支持 TextPattern";
        return false;
    }

    IUIAutomationTextRangeArray* ranges = nullptr;
    if (FAILED(tp->GetSelection(&ranges)) || ranges == nullptr) {
        why = L"GetSelection 失败";
        tp->Release();
        return false;
    }

    bool ok = false;
    int count = 0;
    ranges->get_Length(&count);
    if (count <= 0) {
        why = L"选区数量为 0";
    } else {
        IUIAutomationTextRange* range = nullptr;
        if (SUCCEEDED(ranges->GetElement(0, &range)) && range != nullptr) {
            // 空选区（仅有光标）会返回空串，这本身就是"无选中文字"的判据。
            // 注意：COM 版 IUIAutomationTextRange 没有 IsDegenerate
            // （那是 WPF TextPatternRange 的属性），故不依赖它。
            BSTR bstr = nullptr;
            if (SUCCEEDED(range->GetText(-1, &bstr)) && bstr != nullptr) {
                const UINT len = ::SysStringLen(bstr);
                if (len > 0) {
                    if (readText) outText.assign(bstr, len);
                    ok = true;
                    why = L"取到文本长度=" + std::to_wstring(len);
                } else {
                    why = L"选区文本为空（可能只有光标）";
                }
                ::SysFreeString(bstr);
            } else {
                why = L"GetText 失败";
            }
            range->Release();
        } else {
            why = L"GetElement(0) 失败";
        }
    }

    ranges->Release();
    tp->Release();
    return ok;
}

// 从给定元素沿祖先链向上查找 TextPattern。
//
// 这是浏览器场景的关键修复。
// Chromium 的顶层窗口类名是 Chrome_WidgetWin_1，它本身不实现 TextPattern；
// 真正的文本选区挂在其内部较深的文档元素上。原实现只查"焦点元素本身"，
// 因此在 Chrome/Edge 中必然失败（实测日志：焦点元素类名=Chrome_WidgetWin_1；
// 该元素不支持 TextPattern）。
bool WalkUpForTextPattern(IUIAutomation* uia, IUIAutomationElement* start,
                          bool readText, std::wstring& outText,
                          std::wstring& trace, int maxDepth) {
    if (uia == nullptr || start == nullptr) return false;

    IUIAutomationTreeWalker* walker = nullptr;
    // RawViewWalker 能看到未过滤的原始树，比 ControlView 更容易命中文本元素
    if (FAILED(uia->get_RawViewWalker(&walker)) || walker == nullptr) {
        if (FAILED(uia->get_ControlViewWalker(&walker)) || walker == nullptr) {
            return false;
        }
    }

    IUIAutomationElement* cur = start;
    cur->AddRef();   // 统一由本函数负责 Release

    bool found = false;
    for (int depth = 0; depth < maxDepth && cur != nullptr; ++depth) {
        std::wstring why, text;
        if (TryTextPatternFrom(cur, readText, text, why)) {
            outText = text;
            trace += L"；第" + std::to_wstring(depth) + L"层命中";
            found = true;
            break;
        }

        // 记录前几层的类名，便于判断树结构
        if (depth < 3) {
            BSTR cls = nullptr;
            if (SUCCEEDED(cur->get_CurrentClassName(&cls)) && cls != nullptr) {
                trace += L"；第" + std::to_wstring(depth) + L"层[" +
                         std::wstring(cls) + L":" + why + L"]";
                ::SysFreeString(cls);
            }
        }

        IUIAutomationElement* parent = nullptr;
        const HRESULT hr = walker->GetParentElement(cur, &parent);
        cur->Release();
        cur = (SUCCEEDED(hr)) ? parent : nullptr;
    }
    if (cur != nullptr) cur->Release();

    walker->Release();
    return found;
}

// 在给定元素的【子树】中查找带 TextPattern 的元素。
//
// 为什么必须向下搜索：
// Chrome/Edge 的焦点元素是顶层窗口 Chrome_WidgetWin_1，真正的文档节点在它的
// 子树里（Chrome_RenderWidgetHostHWND 之下）。实测日志显示，从顶层窗口沿
// 【祖先】链向上走两层就到达桌面（#32769），永远找不到文本节点 —— 方向反了。
//
// UIA 是跨进程调用，Chrome 的原始树极其庞大，因此：
//   - 严格限制深度（maxDepth）与访问元素数（maxNodes）
//   - 每访问若干元素就检查 deadline，超时立即放弃
//     （否则单个卡住的跨进程调用会让工作线程长期挂住）
//   - 找到即返回
bool SearchDescendantsForTextPattern(IUIAutomation* uia, IUIAutomationElement* root,
                                     bool readText, std::wstring& outText,
                                     std::wstring& trace, int maxDepth, int maxNodes,
                                     const std::chrono::steady_clock::time_point& deadline) {
    if (uia == nullptr || root == nullptr) return false;

    IUIAutomationTreeWalker* walker = nullptr;
    if (FAILED(uia->get_RawViewWalker(&walker)) || walker == nullptr) {
        if (FAILED(uia->get_ControlViewWalker(&walker)) || walker == nullptr) {
            return false;
        }
    }

    struct Node {
        IUIAutomationElement* e;
        int depth;
    };
    std::vector<Node> stack;
    root->AddRef();
    stack.push_back({ root, 0 });

    bool found = false;
    int visited = 0;
    int textPatternSeen = 0;

    while (!stack.empty() && visited < maxNodes) {
        // 超时熔断：跨进程 UIA 调用可能长时间阻塞，
        // 每访问 8 个元素检查一次，及时放弃，避免工作线程被永久占住。
        if ((visited & 7) == 0 &&
            std::chrono::steady_clock::now() >= deadline) {
            trace += L"；子树搜索超时(已访问" + std::to_wstring(visited) + L"个)";
            break;
        }

        const Node n = stack.back();
        stack.pop_back();
        ++visited;

        std::wstring why, text;
        if (TryTextPatternFrom(n.e, readText, text, why)) {
            outText = text;
            trace += L"；子树第" + std::to_wstring(n.depth) + L"层命中(访问" +
                     std::to_wstring(visited) + L"个)";
            found = true;
            n.e->Release();
            break;
        }
        // 有 TextPattern 但选区为空，说明找到了文本节点却没有选中内容
        if (why == L"选区数量为 0" || why == L"选区文本为空（可能只有光标）") {
            ++textPatternSeen;
        }

        // 记录浅层类名，便于诊断树结构
        if (n.depth < 2 && visited <= 6) {
            BSTR cls = nullptr;
            if (SUCCEEDED(n.e->get_CurrentClassName(&cls)) && cls != nullptr) {
                trace += L"；d" + std::to_wstring(n.depth) + L"[" +
                         std::wstring(cls) + L"]";
                ::SysFreeString(cls);
            }
        }

        if (n.depth < maxDepth) {
            IUIAutomationElement* child = nullptr;
            if (SUCCEEDED(walker->GetFirstChildElement(n.e, &child)) && child != nullptr) {
                int siblingCount = 0;
                for (;;) {
                    child->AddRef();
                    stack.push_back({ child, n.depth + 1 });
                    ++siblingCount;
                    IUIAutomationElement* next = nullptr;
                    const HRESULT hr = walker->GetNextSiblingElement(child, &next);
                    child->Release();
                    if (FAILED(hr) || next == nullptr) break;
                    child = next;
                    // 单层子节点上限，避免某一层扇出过大拖垮搜索
                    if (siblingCount >= 60) break;
                }
            }
        }
        n.e->Release();
    }

    // 释放栈中未处理的元素
    for (const auto& n : stack) {
        n.e->Release();
    }

    if (!found) {
        trace += L"；子树搜索" + std::to_wstring(visited) + L"个元素未命中(有TP但空选区的=" +
                 std::to_wstring(textPatternSeen) + L")";
    }
    walker->Release();
    return found;
}

// 在工作线程内执行 UIA 读取。
// 若主线程超时放弃，本线程仍会自行跑完并释放资源（共享指针保证生命周期安全）。
//
// 策略（按优先级）：
//   1) 焦点元素本身
//   2) 焦点元素的祖先链（浏览器顶层窗口不含 TextPattern）
//   3) 鼠标位置处的元素及其祖先链（用户刚划词，光标就在选区附近）
//
// 每一步失败都记录诊断信息。没有它，取词失败时日志完全静默，
// 无法区分"元素不支持 TextPattern"与"选区为空"这两种截然不同的原因。
void UiaWorker(std::shared_ptr<UiaProbeState> state, bool readText) {
    // RAII 守卫：无论从哪条路径返回，都要清掉"UIA 探测进行中"标志，
    // 否则剪贴板通道会一直等一个早已结束的探测。
    struct RunningGuard {
        RunningGuard() { g_uia_probe_running.store(true); }
        ~RunningGuard() { g_uia_probe_running.store(false); }
    } runningGuard;

    const HRESULT hrInit = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    IUIAutomation* uia = nullptr;
    HRESULT hr = ::CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&uia));
    if (FAILED(hr) || uia == nullptr) {
        state->diag = L"CoCreateInstance(CUIAutomation) 失败 hr=" + HrStr(hr);
        if (SUCCEEDED(hrInit)) ::CoUninitialize();
        state->done = true;
        return;
    }

    IUIAutomationElement* focused = nullptr;
    hr = uia->GetFocusedElement(&focused);
    if (FAILED(hr) || focused == nullptr) {
        state->diag = L"GetFocusedElement 失败 hr=" + HrStr(hr);
        uia->Release();
        if (SUCCEEDED(hrInit)) ::CoUninitialize();
        state->done = true;
        return;
    }

    // 记录焦点元素类名：判断"焦点究竟落在谁身上"的关键线索
    {
        BSTR cls = nullptr;
        if (SUCCEEDED(focused->get_CurrentClassName(&cls)) && cls != nullptr) {
            state->diag = L"焦点类名=" + std::wstring(cls);
            ::SysFreeString(cls);
        } else {
            state->diag = L"焦点类名=<取不到>";
        }
    }

    // ---- 策略 1：焦点元素本身 ----
    std::wstring text, why;
    if (TryTextPatternFrom(focused, readText, text, why)) {
        state->found = true;
        state->text  = text;
        state->diag += L"；焦点元素直接命中";
        focused->Release();
        uia->Release();
        if (SUCCEEDED(hrInit)) ::CoUninitialize();
        state->done = true;
        return;
    }

    // ---- 策略 2：焦点元素的【子树】搜索 ----
    // 这是浏览器场景的正确方向。Chrome 的焦点元素是顶层窗口，
    // 文本节点在它的子树里；沿祖先向上走两层就到桌面，永远找不到。
    // 为整轮搜索设一个总时限，避免跨进程调用卡住工作线程。
    const auto searchDeadline = std::chrono::steady_clock::now() +
                                std::chrono::milliseconds(900);
    std::wstring trace;
    if (SearchDescendantsForTextPattern(uia, focused, readText, text, trace,
                                        /*maxDepth=*/6, /*maxNodes=*/250,
                                        searchDeadline)) {
        state->found = true;
        state->text  = text;
        state->diag += L"；焦点子树命中" + trace;
        focused->Release();
        uia->Release();
        if (SUCCEEDED(hrInit)) ::CoUninitialize();
        state->done = true;
        return;
    }

    // ---- 策略 3：鼠标位置处的元素及其子树 ----
    // 用户刚完成划词，光标就在选区附近，从该点定位更准。
    POINT pt{};
    if (::GetCursorPos(&pt)) {
        IUIAutomationElement* atPoint = nullptr;
        if (SUCCEEDED(uia->ElementFromPoint(pt, &atPoint)) && atPoint != nullptr) {
            std::wstring trace2;
            if (SearchDescendantsForTextPattern(uia, atPoint, readText, text, trace2,
                                                /*maxDepth=*/6, /*maxNodes=*/250,
                                                searchDeadline)) {
                state->found = true;
                state->text  = text;
                state->diag += L"；光标处子树命中" + trace2;
                atPoint->Release();
                focused->Release();
                uia->Release();
                if (SUCCEEDED(hrInit)) ::CoUninitialize();
                state->done = true;
                return;
            }
            state->diag += L"；光标处子树未命中" + trace2;
            atPoint->Release();
        }
    }

    // ---- 策略 4：祖先链（保留给非浏览器场景，如自绘控件的嵌套结构）----
    std::wstring trace3;
    if (WalkUpForTextPattern(uia, focused, readText, text, trace3, 12)) {
        state->found = true;
        state->text  = text;
        state->diag += L"；祖先链命中" + trace3;
        focused->Release();
        uia->Release();
        if (SUCCEEDED(hrInit)) ::CoUninitialize();
        state->done = true;
        return;
    }

    // 全部失败：记录完整轨迹
    state->diag += L"；焦点子树未命中" + trace + L"；祖先链未命中" + trace3;
    focused->Release();
    uia->Release();

    if (SUCCEEDED(hrInit)) ::CoUninitialize();
    state->done = true;
}

// 启动 UIA 工作线程并等待至多 timeout_ms。超时返回 false（线程自行收尾）。
bool RunUiaWithTimeout(std::shared_ptr<UiaProbeState> state, bool readText, int timeout_ms) {
    std::thread worker(UiaWorker, state, readText);
    worker.detach();  // 超时后不再等待；state 由 shared_ptr 保活

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (state->done) return true;
        ::Sleep(5);
    }
    return state->done;
}

} // namespace

// ================================================================ 公开接口

bool TextExtractor::IsForegroundConsole() {
    HWND fg = ::GetForegroundWindow();
    if (fg == nullptr) return false;

    wchar_t cls[256]{};
    if (::GetClassNameW(fg, cls, static_cast<int>(std::size(cls))) == 0) {
        return false;
    }

    // conhost 与 Windows Terminal 的窗口类名
    static const wchar_t* kConsoleClasses[] = {
        L"ConsoleWindowClass",
        L"CASCADIA_HOSTING_WINDOW_CLASS",
    };
    for (const auto* c : kConsoleClasses) {
        if (str::EqualsIgnoreCaseW(cls, c)) return true;
    }
    return false;
}

void TextExtractor::WaitForModifiersReleased(int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    for (;;) {
        const bool alt   = (::GetAsyncKeyState(VK_MENU)    & 0x8000) != 0;
        const bool ctrl  = (::GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift = (::GetAsyncKeyState(VK_SHIFT)   & 0x8000) != 0;
        const bool win   = ((::GetAsyncKeyState(VK_LWIN) & 0x8000) != 0) ||
                           ((::GetAsyncKeyState(VK_RWIN) & 0x8000) != 0);

        if (!alt && !ctrl && !shift && !win) return;
        if (std::chrono::steady_clock::now() >= deadline) return;
        ::Sleep(10);
    }
}

bool TextExtractor::ProbeSelectionExists(int timeout_ms) {
    auto state = std::make_shared<UiaProbeState>();
    if (!RunUiaWithTimeout(state, /*readText=*/false, timeout_ms)) {
        return false;  // 超时：保守地认为无选区
    }
    return state->found;
}

bool TextExtractor::TryGetViaUia(std::wstring& out, int timeout_ms) {
    out.clear();

    auto state = std::make_shared<UiaProbeState>();
    if (!RunUiaWithTimeout(state, /*readText=*/true, timeout_ms)) {
        LT_INFO(L"UIA 读取超时（" + std::to_wstring(timeout_ms) +
                L"ms 内未返回），转用剪贴板通道");
        return false;
    }
    if (!state->found || state->text.empty()) {
        // 关键诊断：记录焦点元素类名与失败环节。
        // 浏览器、Electron 应用常出现"焦点元素不支持 TextPattern"的情况，
        // 只有记下类名才能判断是取词策略问题还是真的没有选中文字。
        LT_INFO(L"UIA 未取到文本（" + state->diag + L"），转用剪贴板通道");
        return false;
    }

    out = str::Trim(state->text);
    return !out.empty();
}

bool TextExtractor::TryGetViaClipboard(std::wstring& out, std::wstring& error) {
    out.clear();
    error.clear();

    // 保护：控制台窗口中 Ctrl+C 是 SIGINT，会打断用户正在运行的程序
    if (IsForegroundConsole()) {
        error = L"当前窗口是命令行/终端，发送 Ctrl+C 会中断其中运行的程序，已跳过。"
                L"请改用鼠标拖拽选中文字后重试。";
        LT_WARN(L"跳过剪贴板通道：前台为控制台窗口");
        return false;
    }

    // 等待上一轮 UIA 探测结束。
    //
    // 原因：UIA 超时后其工作线程是 detached 的，仍在目标窗口上做跨进程遍历。
    // 此时若立刻注入 Ctrl+C，目标窗口的 UI 线程正忙于应付 UIA，
    // 来不及处理按键 —— 实测表现为单独测剪贴板成功(94ms)，
    // 但在"UIA 超时 → 剪贴板"的完整链路里失败（序列号不变）。
    // 上限 700ms，避免无限等待。
    if (g_uia_probe_running.load()) {
        const auto waitEnd = std::chrono::steady_clock::now() +
                             std::chrono::milliseconds(700);
        while (g_uia_probe_running.load() &&
               std::chrono::steady_clock::now() < waitEnd) {
            ::Sleep(15);
        }
        LT_INFO(L"剪贴板通道：等待 UIA 探测结束，剩余运行=" +
                std::wstring(g_uia_probe_running.load() ? L"是（超时放弃等待）" : L"否"));
    }

    ClipListener listener;
    if (!listener.EnsureCreated()) {
        error = L"无法创建剪贴板监听窗口。";
        return false;
    }
    listener.Bind();

    // 1) 备份原剪贴板
    ClipboardBackup backup;
    BackupClipboard(backup);

    // 2) 等待修饰键释放，避免 Ctrl+C 与用户按键组合成别的快捷键
    WaitForModifiersReleased(300);

    const DWORD seqBefore = ::GetClipboardSequenceNumber();

    listener.Reset();
    ::AddClipboardFormatListener(listener.Handle());

    // 3) 发送 Ctrl+C
    const bool injected = SendCtrlC();

    // 4) 等待剪贴板更新。
    //
    // 超时值说明：规范建议 150ms，但实测 Chromium（Chrome/Edge）处理 Ctrl+C
    // 并写入剪贴板常超过 150ms —— 日志显示"剪贴板更新=否，序列号 142 -> 143"
    // 却取不到内容，即等待窗口太短。这里放宽到 600ms，
    // 并以 10ms 步进轮询，实际命中后立即返回，不增加正常情况的延迟。
    constexpr int kClipboardWaitMs = 600;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(kClipboardWaitMs);
    bool updated = false;
    while (std::chrono::steady_clock::now() < deadline) {
        MSG msg{};
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        // 双重判据：格式监听消息 或 序列号变化
        if (listener.Updated() || ::GetClipboardSequenceNumber() != seqBefore) {
            // 序列号已变，但数据可能尚未写完，稍等片刻再读
            ::Sleep(30);
            updated = true;
            break;
        }
        ::Sleep(10);
    }

    ::RemoveClipboardFormatListener(listener.Handle());
    listener.Destroy();

    // 记录 Ctrl+C 之后、还原之前的序列号。
    //
    // 这一点很关键：RestoreClipboard 会调用 EmptyClipboard + SetClipboardData，
    // 它本身就会让序列号 +1。若在还原之后再取序列号，读到的是"还原动作"的结果，
    // 会把"Ctrl+C 完全没生效"误判成"Ctrl+C 生效了但读不到内容"。
    // 早期版本正是在还原后才记录，导致诊断信息自相矛盾。
    const DWORD seqAfterCopy = ::GetClipboardSequenceNumber();
    const bool  clipboardChanged = (seqAfterCopy != seqBefore);

    // 5) 读取新文本。
    // 即使监听消息没到也尝试读一次：某些应用只更新内容而不改变序列号
    // （或我们的监听窗口错过了消息），多读一次成本极低。
    bool got = false;
    if (updated || clipboardChanged) {
        if (OpenClipboardRetry(nullptr)) {
            if (HANDLE h = ::GetClipboardData(CF_UNICODETEXT)) {
                if (auto* p = static_cast<const wchar_t*>(::GlobalLock(h))) {
                    out = str::Trim(p);
                    got = !out.empty();
                    ::GlobalUnlock(h);
                }
            }
            ::CloseClipboard();
        }
    }

    // 6) 尽力还原原剪贴板
    const bool restored = RestoreClipboard(backup);
    if (!restored) {
        LT_WARN(L"剪贴板内容未能完全还原（原有格式含延迟渲染数据或位图）");
    }

    if (!got) {
        // 关键诊断：区分"按键没注入"、"对方没响应"与"复制到了空内容"，
        // 三者的排查方向完全不同。
        // 注意 seqAfterCopy 是在还原之前取的，能真实反映 Ctrl+C 是否生效。
        LT_WARN(L"剪贴板通道未取到文本：按键注入=" +
                std::wstring(injected ? L"成功" : L"失败") +
                L"，收到更新消息=" + std::wstring(updated ? L"是" : L"否") +
                L"，Ctrl+C 后序列号 " + std::to_wstring(seqBefore) + L" -> " +
                std::to_wstring(seqAfterCopy) +
                (clipboardChanged ? L"（已变化，但内容为空或非文本）"
                                  : L"（未变化，Ctrl+C 未生效）"));
        error = L"未检测到选中文字";
        return false;
    }
    return true;
}

SelectionText TextExtractor::GetSelectedText() {
    SelectionText result{};

    // ============ 通道顺序：剪贴板优先 ============
    //
    // 这是基于实测数据的决策，不是凭直觉：
    //
    //   通道            结果      耗时
    //   UIA（EDIT 控件）  超时      >800ms
    //   UIA（Edge）      失败      ~800ms（Chromium 不向 UIA 暴露文本节点）
    //   剪贴板（EDIT）    成功      94ms，剪贴板完整还原
    //
    // UIA 作为首选有三个硬伤：
    //   1. 慢 —— 跨进程 COM 调用，逐层遍历动辄数百毫秒，用户体验差
    //   2. 不可靠 —— Chromium 系（Chrome/Edge/Electron）根本不暴露文本节点
    //   3. 有副作用 —— 超时后工作线程是 detached 的，仍在目标窗口上遍历，
    //      会拖住目标 UI 线程，导致随后的 Ctrl+C 注入失效
    //      （实测：单独测剪贴板 94ms 成功；UIA 超时后走剪贴板则失败）
    //
    // 剪贴板通道快、通用、还原可靠，因此提到第一位。
    // UIA 保留为兜底：仅在剪贴板通道失败时尝试，
    // 用于那些"复制被禁用但 UIA 可读"的场景（如某些只读文档查看器）。

    std::wstring text;

    // ---- 通道 A（首选）：剪贴板借用 ----
    std::wstring err;
    if (TryGetViaClipboard(text, err)) {
        result.found  = true;
        result.text   = text;
        result.source = L"剪贴板";
        LT_INFO(L"通过剪贴板获取到选中文本，长度 " + std::to_wstring(text.size()));
        return result;
    }

    // ---- 通道 B（兜底）：UIA ----
    // 此时剪贴板已确认失败，UIA 的慢与副作用不再有额外代价。
    if (TryGetViaUia(text, 400)) {
        result.found  = true;
        result.text   = text;
        result.source = L"UIA";
        LT_INFO(L"通过 UIA 获取到选中文本，长度 " + std::to_wstring(text.size()));
        return result;
    }

    result.error = err.empty() ? L"未检测到选中文字" : err;
    LT_WARN(L"取词失败：剪贴板与 UIA 两条通道均未取到文本");
    return result;
}

} // namespace lt
