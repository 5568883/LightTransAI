#pragma once

#include <cstdint>
#include <string>

namespace lt {

struct MemSnapshot {
    uint64_t private_usage    = 0;  // 私有提交量（任务管理器"提交大小"）
    uint64_t working_set      = 0;  // 物理工作集（任务管理器"内存"列）
    uint64_t peak_working_set = 0;

    double PrivateMb() const { return static_cast<double>(private_usage) / 1048576.0; }
    double WorkingSetMb() const { return static_cast<double>(working_set) / 1048576.0; }
};

class PerfMonitor {
public:
    static MemSnapshot Capture();

    // 记录一条带标签的内存快照到日志，并返回该快照
    static MemSnapshot RecordToLog(const std::wstring& tag);

    // 通知内核回收空闲物理页。
    // 注意：这只把页面移到备用列表，私有提交量不会下降；下次唤醒需硬缺页。
    // 因此只在真正空闲时调用，避免拖慢"第一次翻译"的响应。
    static void TrimWorkingSet();

    // 让 CRT 堆把空闲段真正还给系统，**会降低私有提交量**。
    //
    // 用在框选翻译之后：该流程会临时申请约 29 MB 的全屏缓冲
    // （冻结画面 + 遮罩 DIB，各 2560x1440x4 ≈ 14.7 MB），
    // 单纯释放不会让提交量下降，必须整理堆才会还回去。
    static void ReleaseFreeHeap();
};

} // namespace lt
