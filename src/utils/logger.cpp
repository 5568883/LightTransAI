#include "utils/logger.h"

#include <windows.h>

#include <cstdio>

#include "utils/string_utils.h"

namespace lt {

namespace {
constexpr unsigned long long kMaxLogBytes = 2ULL * 1024 * 1024;  // 2 MB 后轮转

const wchar_t* LevelTag(LogLevel lv) {
    switch (lv) {
        case LogLevel::Debug: return L"DEBUG";
        case LogLevel::Info:  return L"INFO ";
        case LogLevel::Warn:  return L"WARN ";
        case LogLevel::Error: return L"ERROR";
    }
    return L"?????";
}
} // namespace

Logger& Logger::Instance() {
    static Logger inst;
    return inst;
}

void Logger::Init(const std::wstring& logFilePath) {
    std::lock_guard<std::mutex> lock(mtx_);
    path_   = logFilePath;
    inited_ = !path_.empty();
    if (inited_) {
        RotateIfNeeded();
    }
}

void Logger::RotateIfNeeded() {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (::GetFileAttributesExW(path_.c_str(), GetFileExInfoStandard, &fad)) {
        const unsigned long long size =
            (static_cast<unsigned long long>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
        if (size > kMaxLogBytes) {
            const std::wstring oldPath = path_ + L".old";
            ::DeleteFileW(oldPath.c_str());
            ::MoveFileW(path_.c_str(), oldPath.c_str());
        }
    }
}

void Logger::Write(LogLevel lv, const std::wstring& message) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!inited_ || path_.empty()) return;

    SYSTEMTIME st{};
    ::GetLocalTime(&st);

    wchar_t head[64]{};
    swprintf_s(head, L"%04u-%02u-%02u %02u:%02u:%02u.%03u [%s] ",
               st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
               LevelTag(lv));

    // 脱敏在 UTF-8 域进行，避免多字节边界问题
    const std::string masked =
        str::MaskSecrets(str::ToUtf8(std::wstring(head) + message + L"\r\n"));

    HANDLE h = ::CreateFileW(path_.c_str(), FILE_APPEND_DATA,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;

    DWORD written = 0;
    ::WriteFile(h, masked.data(), static_cast<DWORD>(masked.size()), &written, nullptr);
    ::CloseHandle(h);
}

} // namespace lt
