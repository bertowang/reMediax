#ifdef __APPLE__

#import <AVFoundation/AVFoundation.h>

// 同 IOSScreenCapture.mm：避开 AVFoundation 的 `typedef NSString* AVMediaType`
// 与 ffmpeg 的 `enum AVMediaType` 同名冲突
#define AVMediaType TG_AVMediaType
#include "core/Clock.h"
#include "core/Logger.h"
#include "platform/ios/IOSAudioCapture.h"
#undef AVMediaType

namespace TGCapture::platform
{

IOSAudioCapture::IOSAudioCapture(AudioCapSourceType type) : capture::AudioSourceBase(type)
{
}

IOSAudioCapture::~IOSAudioCapture()
{
    Stop();
    if (m_engine) {
        AVAudioEngine* engine = (__bridge_transfer AVAudioEngine*)m_engine;
        [engine stop];
        m_engine = nullptr;
    }
}

int IOSAudioCapture::Open(int sampleRate, int channels)
{
    m_sampleRate = sampleRate > 0 ? sampleRate : 44100;
    m_channels = channels > 0 ? channels : 2;

    AVAudioSession* session = [AVAudioSession sharedInstance];
    NSError* error = nil;
    // 开播场景：录音 + 播放并存，走 playAndRecord，并按硬件采样率对齐
    if (![session setCategory:AVAudioSessionCategoryPlayAndRecord
                  withOptions:AVAudioSessionCategoryOptionDefaultToSpeaker |
                              AVAudioSessionCategoryOptionMixWithOthers
                        error:&error]) {
        TG_LOGE("IOSAudioCapture: setCategory 失败: %s", [[error localizedDescription] UTF8String]);
        return -1;
    }
    if (![session setPreferredSampleRate:static_cast<double>(m_sampleRate) error:&error]) {
        TG_LOGW("IOSAudioCapture: setPreferredSampleRate 失败: %s",
                [[error localizedDescription] UTF8String]);
    }
    if (![session setActive:YES error:&error]) {
        TG_LOGE("IOSAudioCapture: setActive 失败: %s", [[error localizedDescription] UTF8String]);
        return -2;
    }
    const double hardwareRate = session.sampleRate;
    if (hardwareRate > 0) {
        m_sampleRate = static_cast<int>(hardwareRate);
    }

    AVAudioEngine* engine = [[AVAudioEngine alloc] init];
    AVAudioInputNode* input = engine.inputNode;
    AVAudioFormat* format = [input outputFormatForBus:0];
    if (!format) {
        TG_LOGE("IOSAudioCapture: 拿不到输入格式");
        return -3;
    }

    const int targetChannels = m_channels;
    const int targetRate = m_sampleRate;
    __weak auto weakSelf = this;
    [input installTapOnBus:0
                bufferSize:4096
                    format:format
                     block:^(AVAudioPCMBuffer* buffer, AVAudioTime* when) {
                       if (!buffer) {
                           return;
                       }
                       // 统一转成交错 S16
                       AVAudioFormat* dst = [[AVAudioFormat alloc]
                           initWithCommonFormat:AVAudioPCMFormatInt16
                                     sampleRate:targetRate
                                       channels:static_cast<AVAudioChannelCount>(targetChannels)
                                    interleaved:YES];
                       AVAudioConverter* converter =
                           [[AVAudioConverter alloc] initFromFormat:format toFormat:dst];
                       if (!converter) {
                           return;
                       }
                       const AVAudioFrameCount outCapacity = 4096;
                       AVAudioPCMBuffer* outBuffer =
                           [[AVAudioPCMBuffer alloc] initWithPCMFormat:dst frameCapacity:outCapacity];
                       if (!outBuffer) {
                           return;
                       }
                       NSError* convError = nil;
                       __block BOOL consumed = NO;
                       AVAudioConverterInputBlock inputBlock =
                           ^AVAudioPCMBuffer*(AVAudioPacketCount inPackets, AVAudioConverterInputStatus* status) {
                             if (consumed) {
                                 *status = AVAudioConverterInputStatus_NoDataNow;
                                 return nil;
                             }
                             consumed = YES;
                             *status = AVAudioConverterInputStatus_HaveData;
                             return buffer;
                           };
                       [converter convertToBuffer:outBuffer
                                            error:&convError
                               withInputFromBlock:inputBlock];
                       if (convError || outBuffer.frameLength == 0) {
                           return;
                       }
                       const int64_t ptsUs = when && when.hostTime > 0
                                                 ? static_cast<int64_t>(when.hostTime / 1000)
                                                 : core::MonotonicClock::NowUs();
                       if (auto* self = weakSelf) {
                           self->OnPcm(reinterpret_cast<const int16_t*>(outBuffer.int16ChannelData[0]),
                                      static_cast<int>(outBuffer.frameLength), ptsUs);
                       }
                     }];

    NSError* startError = nil;
    if (![engine startAndReturnError:&startError]) {
        TG_LOGE("IOSAudioCapture: AVAudioEngine 启动失败: %s",
                [[startError localizedDescription] UTF8String]);
        return -4;
    }

    m_engine = (__bridge_retained void*)engine;
    m_started = true;
    TG_LOGI("IOSAudioCapture: 就绪 麦克风 %dHz %dch", m_sampleRate, m_channels);
    return 0;
}

void IOSAudioCapture::Loop(std::stop_token token)
{
    // PCM 由 AVAudioEngine 的 tap 线程回调，这里只保活
    while (!token.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

void IOSAudioCapture::OnPcm(const int16_t* data, int frames, int64_t ptsUs)
{
    if (!data || frames <= 0) {
        return;
    }
    const size_t bytes = static_cast<size_t>(frames) * m_channels * sizeof(int16_t);
    auto buffer = std::shared_ptr<uint8_t>(new uint8_t[bytes], std::default_delete<uint8_t[]>());
    if (m_muted.load()) {
        memset(buffer.get(), 0, bytes);
    } else {
        memcpy(buffer.get(), data, bytes);
    }

    auto frame = std::make_shared<core::Frame>();
    frame->kind = core::MediaKind::Audio;
    frame->sampleRate = m_sampleRate;
    frame->channels = m_channels;
    frame->sampleFmt = AV_SAMPLE_FMT_S16;
    frame->planar = false;
    frame->nbSamples = frames;
    frame->audioData = buffer;
    frame->audioBytes = bytes;
    frame->captureUs = ptsUs > 0 ? ptsUs : core::MonotonicClock::NowUs();
    frame->ptsUs = frame->captureUs;
    Emit(std::move(frame));
}

}  // namespace TGCapture::platform

#endif  // __APPLE__
