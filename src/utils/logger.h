#pragma once

#include <mutex>
#include <string>

namespace lt {

enum class LogLevel { Debug, Info, Warn, Error };

// 线程安全、自动脱敏（sk-**** / Bearer ****）、带大小上限轮转的简易日志器。
class Logger {
public:
    static Logger& Instance();

    void Init(const std::wstring& logFilePath);
    void Write(LogLevel lv, const std::wstring& message);

    void Info(const std::wstring& m) { Write(LogLevel::Info, m); }
    void Warn(const std::wstring& m) { Write(LogLevel::Warn, m); }
    void Error(const std::wstring& m) { Write(LogLevel::Error, m); }

    const std::wstring& Path() const { return path_; }

private:
    Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void RotateIfNeeded();

    std::wstring path_;
    std::mutex   mtx_;
    bool         inited_ = false;
};

} // namespace lt

#define LT_INFO(msg)  ::lt::Logger::Instance().Info(msg)
#define LT_WARN(msg)  ::lt::Logger::Instance().Warn(msg)
#define LT_ERROR(msg) ::lt::Logger::Instance().Error(msg)
