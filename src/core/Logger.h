// 轻量日志：无第三方依赖，可设置回调转发到宿主
//
// 分级只有运行时一档（ITGCapturerMgr::Init 的 logLevel），没有编译期裁剪：
// Debug 日志在 Release 里也保留，好处是线上能临时打开排查，代价是格式串留在二进制里。
// 高频路径（丢帧、写包失败）一律用 TG_LOG*_THROTTLED，避免刷屏。
#ifndef TGCAPTURE_CORE_LOGGER_H
#define TGCAPTURE_CORE_LOGGER_H

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
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

constexpr const char* LogLevelName(LogLevel level) noexcept
{
    switch (level) {
        case LogLevel::Debug:
            return "D";
        case LogLevel::Info:
            return "I";
        case LogLevel::Warn:
            return "W";
        case LogLevel::Error:
            return "E";
        default:
            return "-";
    }
}

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
    LogLevel Level() const { return m_level.load(); }

    // 线程安全：sink 的读写都加锁；回调时先拷一份再解锁，
    // 避免持锁调用宿主代码（宿主的日志系统反过来调回来会死锁）。
    void SetSink(Sink sink)
    {
        std::lock_guard<std::mutex> lock(m_sinkMutex);
        m_sink = std::move(sink);
    }

    // 复位到出厂状态（UnInit 用）
    void Reset()
    {
        m_level.store(LogLevel::Info);
        std::lock_guard<std::mutex> lock(m_sinkMutex);
        m_sink = nullptr;
    }

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
        Write(level, buffer);
    }

    // 已格式化好的消息直接输出（av_log 回调等场景）。
    // 不二次格式化：ffmpeg 的消息里常带 '}' '%' 等字符，再走一遍 vsnprintf 会被吞掉。
    void Write(LogLevel level, const char* msg) const
    {
        if (level < m_level.load()) {
            return;
        }
        Sink sink;
        {
            std::lock_guard<std::mutex> lock(m_sinkMutex);
            sink = m_sink;
        }
        if (sink) {
            sink(level, msg);
        } else {
            std::lock_guard<std::mutex> lock(m_sinkMutex);
            printf("[TGCapture][%s] %s\n", LogLevelName(level), msg);
            fflush(stdout);
        }
    }

private:
    std::atomic<LogLevel> m_level{LogLevel::Info};
    mutable std::mutex m_sinkMutex;
    Sink m_sink;
};

}  // namespace TGCapture::core

#define TG_LOGD(fmt, ...) TGCapture::core::Logger::Instance().Log(TGCapture::core::LogLevel::Debug, fmt, ##__VA_ARGS__)
#define TG_LOGI(fmt, ...) TGCapture::core::Logger::Instance().Log(TGCapture::core::LogLevel::Info, fmt, ##__VA_ARGS__)
#define TG_LOGW(fmt, ...) TGCapture::core::Logger::Instance().Log(TGCapture::core::LogLevel::Warn, fmt, ##__VA_ARGS__)
#define TG_LOGE(fmt, ...) TGCapture::core::Logger::Instance().Log(TGCapture::core::LogLevel::Error, fmt, ##__VA_ARGS__)

// ---- 限频日志：同一调用点在 intervalUs 内最多输出一条 ----
// 用于丢帧 / 写包失败 / 混流取不到数据这类每帧都会命中的路径：
// 既不静默（第一次一定打出来），也不刷屏（后续按时间窗口合并）。
//
// 状态放在 lambda 的 static 局部量里：每个调用点是一个独立的闭包类型，各有一份状态。
// 不用 __LINE__ 拼变量名 —— MSVC 的传统预处理器在宏内多行展开时 __LINE__ 取值不一致。
#define TG_THROTTLE_GATE(intervalUs)                                                       \
    [] {                                                                                   \
        static std::atomic<int64_t> lastUs{0};                                             \
        const int64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(       \
                                  std::chrono::steady_clock::now().time_since_epoch())     \
                                  .count();                                                \
        int64_t last = lastUs.load(std::memory_order_relaxed);                             \
        return (nowUs - last >= (intervalUs)) && lastUs.compare_exchange_strong(last, nowUs); \
    }()

#define TG_LOGW_THROTTLED(intervalUs, fmt, ...) \
    do {                                        \
        if (TG_THROTTLE_GATE(intervalUs)) {     \
            TG_LOGW(fmt, ##__VA_ARGS__);        \
        }                                       \
    } while (0)

#define TG_LOGE_THROTTLED(intervalUs, fmt, ...) \
    do {                                        \
        if (TG_THROTTLE_GATE(intervalUs)) {     \
            TG_LOGE(fmt, ##__VA_ARGS__);        \
        }                                       \
    } while (0)

#endif  // TGCAPTURE_CORE_LOGGER_H
