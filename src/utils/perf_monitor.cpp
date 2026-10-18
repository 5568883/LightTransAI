#include "utils/perf_monitor.h"

#include <windows.h>
#include <psapi.h>

#pragma comment(lib, "psapi.lib")

#include <cstdio>
#include <malloc.h>   // _heapmin

#include "utils/logger.h"

namespace lt {

MemSnapshot PerfMonitor::Capture() {
    MemSnapshot snap{};
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (::GetProcessMemoryInfo(::GetCurrentProcess(),
                               reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                               sizeof(pmc))) {
        snap.private_usage    = pmc.PrivateUsage;
        snap.working_set      = pmc.WorkingSetSize;
        snap.peak_working_set = pmc.PeakWorkingSetSize;
    }
    return snap;
}

MemSnapshot PerfMonitor::RecordToLog(const std::wstring& tag) {
    const MemSnapshot s = Capture();
    wchar_t buf[256]{};
    swprintf_s(buf, L"[内存] %s -> 私有提交 %.2f MB / 工作集 %.2f MB / 峰值 %.2f MB",
               tag.c_str(), s.PrivateMb(), s.WorkingSetMb(),
               static_cast<double>(s.peak_working_set) / 1048576.0);
    LT_INFO(buf);
    return s;
}

void PerfMonitor::TrimWorkingSet() {
    // 把工作集里可回收的页面换出，降低"内存"列显示值。
    // 注意：这只影响物理内存驻留，**不减少提交量**。
    ::SetProcessWorkingSetSize(::GetCurrentProcess(),
                               static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
}

void PerfMonitor::ReleaseFreeHeap() {
    // 让 CRT 堆把空闲块真正还给系统。
    //
    // 为什么需要它：框选翻译会临时申请两块全屏缓冲
    // （冻结画面 2560x1440x4 ≈ 14.7 MB + 遮罩 DIB 同尺寸），合计约 29 MB。
    // 释放后这些块留在 CRT 的空闲链表里，**进程提交量不会下降** ——
    // 表现为"用完一次框选，提交量就从 2 MB 跳到 40 MB 且再不回落"。
    //
    // _heapmin() 会整理堆段并把完全空闲的段 VirtualFree 还给系统。
    // 它是 CRT 的公开接口（malloc.h），比直接调 HeapCompact 更稳妥，
    // 因为同时覆盖了 debug 堆与 CRT 自己的段管理。
    //
    // 代价：一次堆整理，耗时在毫秒级；只在框选流程结束后调用，不在热路径上。
    ::_heapmin();
}

} // namespace lt
