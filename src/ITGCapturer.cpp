#include "TGCapture/ITGCapturer.h"

#include "TGCapturer.h"
#include "core/Logger.h"

extern "C"
{
#include <libavformat/avformat.h>
}

namespace TGCapture
{

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
    TG_LOGI("TGCapture 初始化完成 logLevel=%d dir=%s", logLevel, logDir.c_str());
    return 0;
}

void ITGCapturerMgr::UnInit()
{
    avformat_network_deinit();
    TG_LOGI("TGCapture 反初始化完成");
}

const char* ITGCapturerMgr::GetVersion()
{
    return "1.0.0";
}

}  // namespace TGCapture
