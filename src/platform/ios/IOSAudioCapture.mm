#ifdef __APPLE__

#import <AVFoundation/AVFoundation.h>

// 同 IOSScreenCapture.mm：避开 AVFoundation 的 `typedef NSString* AVMediaType`
// 与 ffmpeg 的 `enum AVMediaType` 同名冲突
#define AVMediaType TG_AVMediaType
#include "core/Clock.h"
#include "core/Logger.h"
#include "platform/ios/IOSAudioCapture.h"
#undef AVMediaType

// ------------------------------------------------------------------
// AVAudioSession 中断/路由变更 observer
// 持弱引用到 IOSAudioCapture（void* 透传），只负责转发事件
// ------------------------------------------------------------------
@interface TGIOSAudioObserver : NSObject
{
    @public
    TGCapture::platform::IOSAudioCapture* _owner;   // 非拥有指针，Stop 内已注销不会被野回调
}
- (instancetype)initWithOwner:(TGCapture::platform::IOSAudioCapture*)owner;
- (void)attach;
- (void)detach;
@end

@implementation TGIOSAudioObserver

- (instancetype)initWithOwner:(TGCapture::platform::IOSAudioCapture*)owner
{
    if ((self = [super init])) {
        _owner = owner;
    }
    return self;
}

- (void)attach
{
    NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
    [center addObserver:self selector:@selector(onInterruption:)
                   name:AVAudioSessionInterruptionNotification
                 object:[AVAudioSession sharedInstance]];
    [center addObserver:self selector:@selector(onRouteChange:)
                   name:AVAudioSessionRouteChangeNotification
                 object:[AVAudioSession sharedInstance]];
}

- (void)detach
{
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    _owner = nullptr;
}

- (void)onInterruption:(NSNotification*)notification
{
    if (!_owner) return;
    NSDictionary* info = notification.userInfo;
    NSNumber* typeNum = info[AVAudioSessionInterruptionTypeKey];
    NSNumber* optNum  = info[AVAudioSessionInterruptionOptionKey];
    const NSUInteger type = typeNum ? [typeNum unsignedIntegerValue]
                                     : (NSUInteger)AVAudioSessionInterruptionTypeEnded;
    const bool shouldResume = optNum &&
        ([optNum unsignedIntegerValue] & AVAudioSessionInterruptionOptionShouldResume) != 0;

    if (type == AVAudioSessionInterruptionTypeBegan) {
        _owner->NotifyAudioInterrupted(true, false);
    } else {
        _owner->NotifyAudioInterrupted(false, shouldResume);
    }
}

- (void)onRouteChange:(NSNotification*)notification
{
    if (!_owner) return;
    NSNumber* reasonNum = notification.userInfo[AVAudioSessionRouteChangeReasonKey];
    const int reason = reasonNum ? (int)[reasonNum intValue] : 0;
    _owner->NotifyAudioRouteChanged(reason);
}

@end

namespace TGCapture::platform
{

IOSAudioCapture::IOSAudioCapture(AudioCapSourceType type) : capture::AudioSourceBase(type)
{
}

IOSAudioCapture::~IOSAudioCapture()
{
    Stop();
    if (m_observer) {
        TGIOSAudioObserver* observer = (__bridge_transfer TGIOSAudioObserver*)m_observer;
        [observer detach];
        m_observer = nullptr;
    }
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

    // 预建 converter/dst-format：每帧都 alloc 一个 AVAudioConverter 会在高负载下吃 CPU（~1-2%），
    // 这里一次 Open 建一次，供 tap block 复用。__block 保持 ARC 强引用在 block 生命期内有效。
    AVAudioFormat* __block dstFormat = [[AVAudioFormat alloc]
        initWithCommonFormat:AVAudioPCMFormatInt16
                  sampleRate:targetRate
                    channels:static_cast<AVAudioChannelCount>(targetChannels)
                 interleaved:YES];
    AVAudioConverter* __block converter =
        [[AVAudioConverter alloc] initFromFormat:format toFormat:dstFormat];
    if (!converter || !dstFormat) {
        TG_LOGE("IOSAudioCapture: AVAudioConverter 初始化失败");
        return -5;
    }

    __weak auto weakSelf = this;
    [input installTapOnBus:0
                bufferSize:4096
                    format:format
                     block:^(AVAudioPCMBuffer* buffer, AVAudioTime* when) {
                       if (!buffer || !converter || !dstFormat) {
                           return;
                       }
                       const AVAudioFrameCount outCapacity = 4096;
                       AVAudioPCMBuffer* outBuffer =
                           [[AVAudioPCMBuffer alloc] initWithPCMFormat:dstFormat frameCapacity:outCapacity];
                       if (!outBuffer) {
                           return;
                       }
                       NSError* convError = nil;
                       __block BOOL consumed = NO;
                       AVAudioConverterInputBlock inputBlock =
                           ^AVAudioPCMBuffer*(AVAudioPacketCount, AVAudioConverterInputStatus* status) {
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

    // 注册中断/路由变更监听（放到 engine 启动成功之后，避免启动失败路径还要反注册）
    if (!m_observer) {
        TGIOSAudioObserver* observer = [[TGIOSAudioObserver alloc] initWithOwner:this];
        [observer attach];
        m_observer = (__bridge_retained void*)observer;
    }

    TG_LOGI("IOSAudioCapture: 就绪 麦克风 %dHz %dch", m_sampleRate, m_channels);
    return 0;
}

void IOSAudioCapture::Loop(std::stop_token token)
{
    // 正常情况下 PCM 由 AVAudioEngine 的 tap 线程回调；
    // 中断/路由异常期间 tap 不触发，这里推 10ms 静音心跳保持音频时间轴连续。
    const int silenceFrames = m_sampleRate > 0 ? m_sampleRate / 100 : 441;
    while (!token.stop_requested()) {
        if (m_interrupted.load()) {
            if (silenceFrames > 0 && m_channels > 0) {
                const size_t bytes = static_cast<size_t>(silenceFrames) * m_channels * sizeof(int16_t);
                auto buffer = std::shared_ptr<uint8_t>(new uint8_t[bytes](), std::default_delete<uint8_t[]>());
                auto frame = std::make_shared<core::Frame>();
                frame->kind = core::MediaKind::Audio;
                frame->sampleRate = m_sampleRate;
                frame->channels = m_channels;
                frame->sampleFmt = AV_SAMPLE_FMT_S16;
                frame->planar = false;
                frame->nbSamples = silenceFrames;
                frame->audioData = buffer;
                frame->audioBytes = bytes;
                frame->captureUs = core::MonotonicClock::NowUs();
                frame->ptsUs = frame->captureUs;
                Emit(std::move(frame));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
}

bool IOSAudioCapture::RestartEngine()
{
    if (!m_engine) return false;
    AVAudioEngine* engine = (__bridge AVAudioEngine*)m_engine;
    AVAudioSession* session = [AVAudioSession sharedInstance];
    NSError* err = nil;
    // 中断/路由变化后 session 可能被系统 deactivate，这里重新激活
    if (![session setActive:YES error:&err]) {
        TG_LOGW("IOSAudioCapture: session setActive 失败: %s",
                [[err localizedDescription] UTF8String]);
    }
    NSError* startErr = nil;
    if (![engine startAndReturnError:&startErr]) {
        TG_LOGW("IOSAudioCapture: engine 重启失败: %s",
                [[startErr localizedDescription] UTF8String]);
        return false;
    }
    TG_LOGI("IOSAudioCapture: engine 已重启");
    return true;
}

void IOSAudioCapture::NotifyAudioInterrupted(bool begin, bool shouldResume)
{
    if (begin) {
        TG_LOGI("IOSAudioCapture: 中断开始，转静音心跳");
        m_interrupted.store(true);
        if (m_engine) {
            AVAudioEngine* engine = (__bridge AVAudioEngine*)m_engine;
            [engine pause];
        }
        return;
    }
    TG_LOGI("IOSAudioCapture: 中断结束 shouldResume=%d", shouldResume ? 1 : 0);
    // 系统允许恢复才尝试重启；不允许恢复则继续心跳，等待下次系统事件/用户动作
    if (shouldResume && RestartEngine()) {
        m_interrupted.store(false);
    }
}

void IOSAudioCapture::NotifyAudioRouteChanged(int reason)
{
    // 常见 reason：
    //   1 NewDeviceAvailable    2 OldDeviceUnavailable    3 CategoryChange
    //   4 Override              6 WakeFromSleep           7 NoSuitableRouteForCategory
    // 设备拔出/路由不可用：先转心跳，再尝试重启 engine 跟新路由（如喇叭回退）
    if (reason == 2 /*OldDeviceUnavailable*/ || reason == 7 /*NoSuitableRouteForCategory*/) {
        TG_LOGI("IOSAudioCapture: 路由变更 reason=%d → 转心跳并重启 engine", reason);
        m_interrupted.store(true);
        if (RestartEngine()) {
            m_interrupted.store(false);
        }
    } else {
        TG_LOGD("IOSAudioCapture: 路由变更 reason=%d (忽略)", reason);
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
