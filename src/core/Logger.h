// 轻量日志：无第三方依赖，可设置回调转发到宿主
//
// 分级只有运行时一档（ITGCapturerMgr::Init 的 logLevel），没有编译期裁剪：
// Debug 日志在 Release 里也保留，好处是线上能临时打开排查，代价是格式串留在二进制里。
// 高频路径（丢帧、写包失败）一律用 TG_LOG*_THROTTLED，避免刷屏。
//
// 输出三选一/可叠加：
//   1. sink（宿主接管，优先级最高，宿主通常自己拼时间戳）
//   2. stdout（printLog 开关，默认开）
//   3. 文件（logDir 落盘，按大小轮转）
#ifndef TGCAPTURE_CORE_LOGGER_H
#define TGCAPTURE_CORE_LOGGER_H

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

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
        std::lock_guard<std::mutex> lock(m_outputMutex);
        m_sink = std::move(sink);
    }

    // 复位到出厂状态（UnInit 用）
    void Reset()
    {
        std::lock_guard<std::mutex> lock(m_outputMutex);
        m_level.store(LogLevel::Info);
        m_sink = nullptr;
        m_printLog = true;
        m_file.close();
        m_fileBytes = 0;
        m_fileIndex = 0;
    }

    // ---- 落盘 ----
    // dir 下生成 tgcapture_<时间戳>.log，超过 maxBytes 切 tgcapture_<时间戳>_N.log。
    // 目录不存在会尝试创建；失败返回 false 并保留"不落盘"状态（不会丢日志，只是不写文件）。
    bool OpenLogFile(const std::string& dir, size_t maxBytes = 32u * 1024u * 1024u);
    void CloseLogFile();
    bool IsLogFileOpen() const;

    // ---- stdout 开关 ----
    void SetPrintLog(bool enable) { m_printLog.store(enable); }
    bool PrintLog() const { return m_printLog.load(); }

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
            std::lock_guard<std::mutex> lock(m_outputMutex);
            sink = m_sink;
        }
        // 宿主接管时不再往 stdout 打（避免两份日志），但文件照写
        if (sink) {
            sink(level, msg);
        }
        WriteBuiltin(level, msg, sink != nullptr);
    }

private:
    void WriteBuiltin(LogLevel level, const char* msg, bool hasSink) const;
    static std::string MakePrefix(LogLevel level);

private:
    std::atomic<LogLevel> m_level{LogLevel::Info};
    std::atomic<bool> m_printLog{true};
    mutable std::mutex m_outputMutex;
    Sink m_sink;

    // 落盘状态：Write 是 const，所以这些成员必须是 mutable
    mutable std::ofstream m_file;
    mutable std::string m_fileStem;   // 不含序号与扩展名的前缀
    mutable size_t m_fileBytes = 0;
    mutable size_t m_maxFileBytes = 32u * 1024u * 1024u;
    mutable int m_fileIndex = 0;
};

inline bool Logger::OpenLogFile(const std::string& dir, size_t maxBytes)
{
    std::lock_guard<std::mutex> lock(m_outputMutex);
    if (dir.empty()) {
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec && !std::filesystem::is_directory(dir, ec)) {
        return false;
    }

    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tmBuf{};
#if defined(_WIN32)
    localtime_s(&tmBuf, &t);
#else
    localtime_r(&t, &tmBuf);
#endif
    char stamp[32];
    std::snprintf(stamp, sizeof(stamp), "%04d%02d%02d_%02d%02d%02d", tmBuf.tm_year + 1900,
                  tmBuf.tm_mon + 1, tmBuf.tm_mday, tmBuf.tm_hour, tmBuf.tm_min, tmBuf.tm_sec);

    m_fileStem = (std::filesystem::path(dir) / ("tgcapture_" + std::string(stamp))).string();
    m_maxFileBytes = maxBytes > 0 ? maxBytes : (32u * 1024u * 1024u);
    m_fileIndex = 0;
    m_fileBytes = 0;

    m_file.open(m_fileStem + ".log", std::ios::out | std::ios::trunc);
    if (!m_file.is_open()) {
        return false;
    }
    return true;
}

inline void Logger::CloseLogFile()
{
    std::lock_guard<std::mutex> lock(m_outputMutex);
    if (m_file.is_open()) {
        m_file.flush();
        m_file.close();
    }
    m_fileBytes = 0;
}

inline bool Logger::IsLogFileOpen() const
{
    std::lock_guard<std::mutex> lock(m_outputMutex);
    return m_file.is_open();
}

inline std::string Logger::MakePrefix(LogLevel level)
{
    const auto now = std::chrono::system_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch())
                        .count();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tmBuf{};
#if defined(_WIN32)
    localtime_s(&tmBuf, &t);
#else
    localtime_r(&t, &tmBuf);
#endif

    // 用 snprintf 而不是 ostringstream：前缀在每条日志的必经路径上，
    // ostringstream 的多次 operator<< 会明显拖慢（实测占单条耗时的相当一部分）
    char buf[96];
    std::snprintf(buf, sizeof(buf), "[TGCapture][%04d-%02d-%02d %02d:%02d:%02d.%03d][%u][%s] ",
                  tmBuf.tm_year + 1900, tmBuf.tm_mon + 1, tmBuf.tm_mday, tmBuf.tm_hour,
                  tmBuf.tm_min, tmBuf.tm_sec, static_cast<int>(ms % 1000),
                  static_cast<unsigned>(std::hash<std::thread::id>{}(std::this_thread::get_id()) %
                                       100000),
                  LogLevelName(level));
    return std::string(buf);
}

inline void Logger::WriteBuiltin(LogLevel level, const char* msg, bool hasSink) const
{
    std::lock_guard<std::mutex> lock(m_outputMutex);

    if (!hasSink && m_printLog.load()) {
        // 前缀只给内置输出加：宿主 sink 通常自己拼时间戳，
        // 而且它拿到的 msg 可能还要再加工，塞进固定前缀反而不好切分。
        const std::string line = MakePrefix(level) + msg;
        printf("%s\n", line.c_str());
        // 每条都 fflush 会把吞吐压到 ~5 千条/秒（实测），比写文件还慢 40 倍。
        // 只给 Warn/Error 立即 flush：关键日志不丢，普通日志交给 CRT 缓冲。
        if (level >= LogLevel::Warn) {
            fflush(stdout);
        }
    }

    if (m_file.is_open()) {
        m_file << MakePrefix(level) << msg << '\n';
        m_fileBytes += (msg ? strlen(msg) : 0) + 96;   // 含前缀的近似长度
        if (m_fileBytes >= m_maxFileBytes) {
            m_file.flush();
            m_file.close();
            ++m_fileIndex;
            m_file.open(m_fileStem + "_" + std::to_string(m_fileIndex) + ".log",
                        std::ios::out | std::ios::trunc);
            m_fileBytes = 0;
        }
    }
}

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
