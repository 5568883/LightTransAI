#pragma once

#include <windows.h>

#include <string>

namespace lt {

struct SelectionText {
    bool         found = false;
    std::wstring text;
    std::wstring source;   // "UIA" 或 "剪贴板"
    std::wstring error;    // 失败原因（简体中文）
};

// 双通道获取选中文本。
//
// 通道 A（优先，零感知）：UIAutomation 读取焦点元素的 TextPattern 选区。
//   ⚠ UIA 是跨进程 COM 调用，遇到繁忙的应用可能阻塞数百毫秒甚至更久，
//     因此一律在工作线程执行并带超时，绝不放在钩子回调或 UI 线程上裸调。
//
// 通道 B（回退，剪贴板借用）：
//   备份剪贴板 -> 发 Ctrl+C -> 等 WM_CLIPBOARDUPDATE（≤150ms）-> 读文本 -> 尽力还原。
//   ⚠ 规范所称"无损"在技术上无法完全达成：CF_HDROP、HTML/RTF 等的延迟渲染
//     数据无法忠实备份还原，写回也会改变剪贴板属主。这里实现为"尽力还原"，
//     优先保证用户文本内容不丢失。
//   ⚠ 向控制台窗口发送 Ctrl+C 会被解释为 SIGINT，可能打断用户正在运行的程序，
//     因此对控制台类窗口直接跳过通道 B。
class TextExtractor {
public:
    // 完整链路：先试 UIA，失败再试剪贴板
    static SelectionText GetSelectedText();

    // 仅探测"当前是否存在文本选区"（正信号，不触碰剪贴板）。
    // 用于划词悬浮按钮的弹出判定：比工字光标判定可靠得多。
    static bool ProbeSelectionExists(int timeout_ms = 120);

    // 通道 A
    static bool TryGetViaUia(std::wstring& out, int timeout_ms = 250);

    // 通道 B
    static bool TryGetViaClipboard(std::wstring& out, std::wstring& error);

    // 当前前台窗口是否为控制台/终端（发送 Ctrl+C 会造成破坏）
    static bool IsForegroundConsole();

    // 检查并等待 Alt/Ctrl/Shift/Win 等修饰键释放，避免 Ctrl+C 与用户按键叠加
    static void WaitForModifiersReleased(int timeout_ms = 300);
};

} // namespace lt
