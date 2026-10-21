#include "TGCapture/ITGCapturer.h"

#include <cstring>

#include "TGCapturer.h"
#include "core/Logger.h"

extern "C"
{
#include <libavformat/avformat.h>
}

namespace TGCapture
{

namespace
{
// FFmpeg 自己的日志默认打 stderr：不进统一日志系统、在 Android 上等于丢弃。
// 这里接管后按级别转发到 Logger，前缀 [ffmpeg] 便于区分。
void AvLogCallback(void* /*ptr*/, int level, const char* fmt, va_list vl)
{
    if (level > av_log_get_level()) {
        return;
    }
    core::LogLevel lv;
    if (level <= AV_LOG_ERROR) {
        lv = core::LogLevel::Error;
    } else if (level <= AV_LOG_WARNING) {
        lv = core::LogLevel::Warn;
    } else if (level <= AV_LOG_INFO) {
        lv = core::LogLevel::Info;
    } else {
        lv = core::LogLevel::Debug;
    }

    char msg[1024];
    vsnprintf(msg, sizeof(msg), fmt, vl);
    // 去掉 ffmpeg 自带的换行，避免和 Logger 的换行叠成空行
    size_t len = strlen(msg);
    while (len > 0 && (msg[len - 1] == '\n' || msg[len - 1] == '\r')) {
        msg[--len] = '\0';
    }

    char line[1100];
    snprintf(line, sizeof(line), "[ffmpeg] %s", msg);
    core::Logger::Instance().Write(lv, line);
}
}  // namespace

ITGCapturer* ITGCapturer::Create(GameID gameId)
{
    return new TGCapturer(gameId);
}

void ITGCapturer::Destroy(ITGCapturer* capturer)
{
    delete capturer;
}

int ITGCapturerMgr::Init(int logLevel, const std::string& logDir, bool printLog)
{
    avformat_network_init();
    switch (logLevel) {
        case 0:
            core::Logger::Instance().SetLevel(core::LogLevel::Debug);
            break;
        case 2:
            core::Logger::Instance().SetLevel(core::LogLevel::Warn);
            break;
        case 3:
            core::Logger::Instance().SetLevel(core::LogLevel::Error);
            break;
        default:
            core::Logger::Instance().SetLevel(core::LogLevel::Info);
            break;
    }
    av_log_set_level(logLevel >= 2 ? AV_LOG_ERROR : AV_LOG_INFO);
    av_log_set_callback(AvLogCallback);
    TG_LOGI("TGCapture 初始化完成 logLevel=%d dir=%s printLog=%d", logLevel, logDir.c_str(),
            printLog ? 1 : 0);
    return 0;
}

void ITGCapturerMgr::UnInit()
{
    avformat_network_deinit();
    TG_LOGI("TGCapture 反初始化完成");
    av_log_set_callback(nullptr);
    core::Logger::Instance().Reset();
}

const char* ITGCapturerMgr::GetVersion()
{
    return "1.0.0";
}

}  // namespace TGCapture
