#import "TGLiveSession.h"

#include <atomic>
#include <cmath>
#include <thread>
#include <vector>

#include <mach/mach_time.h>

#include "TGCapture/ITGCapturer.h"

using namespace TGCapture;

namespace
{

// 内核时钟域：iOS 为 mach_absolute_time 换算的微秒（见 ITGCapturer.h 的 ptsUs 约定）
int64_t NowUs()
{
    static mach_timebase_info_data_t tb = [] {
        mach_timebase_info_data_t info;
        mach_timebase_info(&info);
        return info;
    }();
    return static_cast<int64_t>(mach_absolute_time() * tb.numer / tb.denom / 1000);
}

void SleepUntilUs(int64_t deadlineUs)
{
    const int64_t wait = deadlineUs - NowUs();
    if (wait > 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(wait));
    }
}

// 测试信号源：不依赖录屏 / 麦克风，经外部注入接口推帧（与 Android demo 的 TestSignalSource 一致）
//   视频：NV12 75% 彩条 + 水平移动的白块（肉眼判断卡顿 / 掉帧）
//   音频：1kHz 正弦，每秒响 0.5s（便于听出断音与音画不同步）
class TestSignals
{
public:
    TestSignals(ITGCapturer* capturer, TGLiveConfig* cfg)
        : m_capturer(capturer), m_width(cfg.width), m_height(cfg.height), m_fps(cfg.fps),
          m_sampleRate(cfg.sampleRate), m_channels(cfg.channels), m_video(!cfg.screenCapture), m_audio(cfg.testTone)
    {
    }

    ~TestSignals() { Stop(); }

    void Start()
    {
        m_running = true;
        if (m_video) {
            m_threads.emplace_back([this] { VideoLoop(); });
        }
        if (m_audio) {
            m_threads.emplace_back([this] { AudioLoop(); });
        }
    }

    void Stop()
    {
        m_running = false;
        for (auto& t : m_threads) {
            if (t.joinable()) {
                t.join();
            }
        }
        m_threads.clear();
    }

private:
    void VideoLoop()
    {
        const int w = m_width, h = m_height;
        static const uint8_t kBars[8][3] = {{180, 128, 128}, {162, 44, 142}, {131, 156, 44}, {112, 72, 58},
                                            {84, 184, 198},  {65, 100, 212}, {35, 212, 114}, {16, 128, 128}};
        std::vector<uint8_t> base(static_cast<size_t>(w) * h * 3 / 2);
        const int bar = (w + 7) / 8;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                base[static_cast<size_t>(y) * w + x] = kBars[x / bar][0];
            }
        }
        uint8_t* uv = base.data() + static_cast<size_t>(w) * h;
        for (int y = 0; y < h / 2; ++y) {
            for (int x = 0; x < w; x += 2) {
                uv[static_cast<size_t>(y) * w + x] = kBars[x / bar][1];
                uv[static_cast<size_t>(y) * w + x + 1] = kBars[x / bar][2];
            }
        }
        std::vector<uint8_t> frame(base.size());
        const int box = (std::min(w, h) / 6) & ~1;
        const int step = std::max(2, w / (m_fps * 4)) & ~1;
        const int top = ((h - box) / 2) & ~1;
        const int64_t interval = 1000000 / m_fps;
        int64_t next = NowUs();
        int bx = 0;
        while (m_running) {
            memcpy(frame.data(), base.data(), base.size());
            for (int y = top; y < top + box; ++y) {
                memset(frame.data() + static_cast<size_t>(y) * w + bx, 235, box);
            }
            for (int y = top / 2; y < (top + box) / 2; ++y) {
                memset(frame.data() + static_cast<size_t>(w) * h + static_cast<size_t>(y) * w + bx, 128, box);
            }
            bx += step;
            if (bx + box > w) {
                bx = 0;
            }
            m_capturer->PushExternalVideoFrame(frame.data(), w, h, w, NowUs());
            next += interval;
            if (next < NowUs()) {
                next = NowUs();
            }
            SleepUntilUs(next);
        }
    }

    void AudioLoop()
    {
        const int samples = m_sampleRate / 100;   // 10ms 一块
        std::vector<int16_t> pcm(static_cast<size_t>(samples) * m_channels);
        const int64_t start = NowUs();
        int64_t n = 0;
        int64_t chunk = 0;
        while (m_running) {
            for (int i = 0; i < samples; ++i, ++n) {
                const bool beep = (n % m_sampleRate) < m_sampleRate / 2;
                const auto v = beep ? static_cast<int16_t>(std::sin(2.0 * M_PI * 1000.0 * n / m_sampleRate) * 8000) : 0;
                for (int c = 0; c < m_channels; ++c) {
                    pcm[static_cast<size_t>(i) * m_channels + c] = v;
                }
            }
            // 采集时刻按墙钟推进（内核用它测漂移，不能传"按样本数累加"的时间）
            m_capturer->PushExternalAudioFrame(TypeOfGameAudio4Slave, reinterpret_cast<const uint8_t*>(pcm.data()),
                                               static_cast<int>(pcm.size() * sizeof(int16_t)), m_sampleRate,
                                               m_channels, NowUs());
            ++chunk;
            SleepUntilUs(start + chunk * 10000);
        }
    }

    ITGCapturer* m_capturer;
    const int m_width, m_height, m_fps, m_sampleRate, m_channels;
    const bool m_video, m_audio;
    std::atomic_bool m_running{false};
    std::vector<std::thread> m_threads;
};

}  // namespace

NSString* TGErrorName(int code)
{
    switch (code) {
        case 0: return @"OK";
        case -1: return @"InvalidState";
        case -2: return @"NotConfigured";
        case -3: return @"OpenVideoEncoder";
        case -4: return @"OpenAudioEncoder";
        case -5: return @"OpenMuxer";
        case -6: return @"OpenVideoSource";
        case -7: return @"StartFailed";
        case -20: return @"InvalidArgument";
        case -21: return @"NotSupported";
        case -22: return @"NotImplemented";
        case -23: return @"Internal";
        case -101: return @"MuxWriteFailed";
        case -102: return @"VideoEncodeFailed";
        case -103: return @"AudioEncodeFailed";
        case -104: return @"VideoCaptureStalled";
        case -105: return @"AudioCaptureStalled";
        case -106: return @"MuxTrailerFailed";
        default: return @"?";
    }
}

NSString* TGNotifyName(int type)
{
    switch (type) {
        case 0: return @"FirstFrameOK";
        case 1: return @"VideoMetaChanged";
        case 10: return @"AudioMetaChanged";
        case 20: return @"SystemFreeze";
        case 21: return @"LocalFileEOF";
        case 30: return @"VideoRecovered";
        case 31: return @"AudioRecovered";
        case 40: return @"EncoderFallback";
        default: return [NSString stringWithFormat:@"type=%d", type];
    }
}

@implementation TGLiveSession {
    __weak id<TGLiveSessionDelegate> _delegate;
    dispatch_queue_t _queue;
    ITGCapturer* _capturer;
    std::unique_ptr<TestSignals> _signals;
    BOOL _finished;
}

- (instancetype)initWithConfig:(TGLiveConfig*)config delegate:(id<TGLiveSessionDelegate>)delegate
{
    if ((self = [super init])) {
        _config = config;
        _delegate = delegate;
        _queue = dispatch_queue_create("tgcapture.demo.session", DISPATCH_QUEUE_SERIAL);
    }
    return self;
}

- (void)dealloc
{
    // 兜底：正常路径已在 stop 里释放
    if (_capturer) {
        _signals.reset();
        ITGCapturer::Destroy(_capturer);
    }
}

- (void)start
{
    dispatch_async(_queue, ^{
      [self doStart];
    });
}

- (void)stop
{
    dispatch_async(_queue, ^{
      [self teardown:0];
    });
}

- (void)muteAudio:(BOOL)mute
{
    dispatch_async(_queue, ^{
      if (self->_capturer) self->_capturer->MuteAudio(mute);
    });
}

- (void)muteVideo:(BOOL)mute
{
    dispatch_async(_queue, ^{
      if (self->_capturer) self->_capturer->MuteVideo(mute);
    });
}

- (void)queryStats:(void (^)(float, NSString*))completion
{
    dispatch_async(_queue, ^{
      if (!self->_capturer) return;
      const float bps = self->_capturer->GetTrafficStats();
      NSString* info = [NSString stringWithUTF8String:self->_capturer->GetStatsInfo(StatsInfoType::bitrateInfo).c_str()];
      dispatch_async(dispatch_get_main_queue(), ^{
        completion(bps, info ?: @"");
      });
    });
}

// ------------------------------------------------------------------ 队列内
- (void)doStart
{
    TGLiveConfig* cfg = self.config;
    _capturer = ITGCapturer::Create();
    if (!_capturer) {
        [self log:@"ITGCapturer::Create 失败"];
        [self teardown:-1];
        return;
    }
    [self installCallbacks];

    if (cfg.screenCapture) {
        IVideoSource* screen = _capturer->CreateScreenCapture(0, ReplayKit);
        if (!screen || _capturer->AddVideoSource(screen) != 0) {
            [self log:@"创建 ReplayKit 屏幕采集失败"];
            [self teardown:-1];
            return;
        }
    }
    if (cfg.microphone) {
        // 失败不致命：少一路声音，内核会对缺数据上报断流
        [self check:@"EnableMicrophoneCapture" rc:_capturer->EnableMicrophoneCapture(false)];
    }

    VideoEncoderParams v;
    v.width = cfg.width;
    v.height = cfg.height;
    v.fps = static_cast<float>(cfg.fps);
    v.vBitrate = cfg.videoBitrateKbps * 1000;
    v.vBitrateMaxFactor = 1.5f;
    v.gop = cfg.gop;
    v.vCodec = cfg.videoCodec.UTF8String;
    v.perfMode = cfg.gpuMode ? PerformanceMode::Gpu : PerformanceMode::Cpu;

    AudioEncoderParams a;
    a.sampleRate = cfg.sampleRate;
    a.channels = cfg.channels;
    a.aBitrate = cfg.audioBitrateKbps * 1000;
    a.aCodec = cfg.audioCodec.UTF8String;
    a.enabled = cfg.audioEnabled;

    MuxerParams m;
    m.muxer = cfg.muxerFormat.UTF8String;
    NSString* output = [cfg resolvedOutput];
    m.muxerOutput = output.UTF8String;
    [self log:[NSString stringWithFormat:@"输出 -> %@", output]];

    if (![self check:@"SetupVideoEncoder" rc:_capturer->SetupVideoEncoder(v)] ||
        ![self check:@"SetupAudioEncoder" rc:_capturer->SetupAudioEncoder(a)] ||
        ![self check:@"SetupMuxer" rc:_capturer->SetupMuxer(m)] ||
        ![self check:@"SetAVTimeout" rc:_capturer->SetAVTimeout(cfg.avTimeoutMs, cfg.avTimeoutMs)] ||
        ![self check:@"Prepare" rc:_capturer->Prepare()] || ![self check:@"Start" rc:_capturer->Start()]) {
        [self teardown:-1];
        return;
    }
    _signals = std::make_unique<TestSignals>(_capturer, cfg);
    _signals->Start();
    dispatch_async(dispatch_get_main_queue(), ^{
      [self->_delegate liveSessionDidStart:self];
    });
}

- (void)installCallbacks
{
    __weak TGLiveSession* weakSelf = self;
    _capturer->SetOnPreparedCallback([weakSelf](ITGCapturer*) { [weakSelf log:@"onPrepared"]; });
    _capturer->SetOnStartCallback([weakSelf]() { [weakSelf log:@"onStarted"]; });
    _capturer->SetOnStopCallback([weakSelf](int32_t err) {
        [weakSelf log:[NSString stringWithFormat:@"onStopped err=%d (%@)", err, TGErrorName(err)]];
    });
    _capturer->SetOnErrorCallback([weakSelf](int32_t what, int32_t extra, const char* msg) {
        [weakSelf log:[NSString stringWithFormat:@"onError what=%d (%@) extra=%d %s", what, TGErrorName(what), extra,
                                                 msg ? msg : ""]];
        // 推流断开后 FFmpeg 的 I/O 错误是粘性的，内核不会自动重连：停播，由用户重新开始
        if (what == TGErr_MuxWriteFailed) {
            [weakSelf stop];
        }
    });
    _capturer->SetOnNotifyCallback([weakSelf](NotifyMsgType type, const std::string& content, int64_t data) {
        [weakSelf log:[NSString stringWithFormat:@"onNotify %@ data=%lld %s", TGNotifyName(type), data,
                                                 content.c_str()]];
    });
}

- (void)teardown:(int)err
{
    if (_finished) {
        return;
    }
    _finished = YES;
    _signals.reset();
    if (_capturer) {
        _capturer->Stop();
        ITGCapturer::Destroy(_capturer);
        _capturer = nullptr;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
      [self->_delegate liveSession:self didFinishWithError:err];
    });
}

- (BOOL)check:(NSString*)what rc:(int)rc
{
    if (rc != 0) {
        [self log:[NSString stringWithFormat:@"%@ 失败 rc=%d (%@)", what, rc, TGErrorName(rc)]];
    }
    return rc == 0;
}

- (void)log:(NSString*)line
{
    dispatch_async(dispatch_get_main_queue(), ^{
      [self->_delegate liveSession:self log:line];
    });
}

@end
