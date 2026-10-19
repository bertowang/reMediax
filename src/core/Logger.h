// 轻量日志：无第三方依赖，可设置回调转发到宿主
#ifndef TGCAPTURE_CORE_LOGGER_H
#define TGCAPTURE_CORE_LOGGER_H

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <functional>
#include <string>

namespace TGCapture::core
{

enum class LogLevel
{
    Debug = 0,
    Info = 1,
    Warn = 2,
    Error = 3,
    None = 4,
};

class Logger
{
public:
    using Sink = std::function<void(LogLevel, const char* msg)>;

    static Logger& Instance()
    {
        static Logger instance;
        return instance;
    }

    void SetLevel(LogLevel level) { m_level.store(level); }
    void SetSink(Sink sink) { m_sink = std::move(sink); }

    void Log(LogLevel level, const char* fmt, ...) const
    {
        if (level < m_level.load()) {
            return;
        }
        char buffer[1024];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buffer, sizeof(buffer), fmt, args);
        va_end(args);

        if (m_sink) {
            m_sink(level, buffer);
        } else {
            printf("[TGCapture][%d] %s\n", static_cast<int>(level), buffer);
            fflush(stdout);
        }
    }

private:
    std::atomic<LogLevel> m_level{LogLevel::Info};
    Sink m_sink;
};

}  // namespace TGCapture::core

#define TG_LOGD(fmt, ...) TGCapture::core::Logger::Instance().Log(TGCapture::core::LogLevel::Debug, fmt, ##__VA_ARGS__)
#define TG_LOGI(fmt, ...) TGCapture::core::Logger::Instance().Log(TGCapture::core::LogLevel::Info, fmt, ##__VA_ARGS__)
#define TG_LOGW(fmt, ...) TGCapture::core::Logger::Instance().Log(TGCapture::core::LogLevel::Warn, fmt, ##__VA_ARGS__)
#define TG_LOGE(fmt, ...) TGCapture::core::Logger::Instance().Log(TGCapture::core::LogLevel::Error, fmt, ##__VA_ARGS__)

#endif  // TGCAPTURE_CORE_LOGGER_H
