// 错误码契约：
//   1) 码值全局唯一、分段正确（同一个数值在任何位置只有一个含义）
//   2) 各接口失败时返回的就是枚举里的码（不是裸魔数）
//   3) 失败都能被宿主观测到：返回值与 OnError 的 what 一致；OnStop 带本轮最后一个错误码
//   4) 断流看护真的会触发，恢复后有 Notify
#include <atomic>
#include <cstdio>
#include <chrono>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "TGCapturer.h"
#include "tg_test.h"

extern "C" {
#include <libavformat/avformat.h>
}

using namespace TGCapture;
using namespace std::chrono_literals;

namespace
{

struct Recorder
{
    std::mutex mutex;
    std::vector<std::pair<int32_t, int32_t>> errors;   // (what, extra)
    std::vector<std::pair<int32_t, int64_t>> notifies;  // (type, data)
    std::atomic<int> prepared{0};
    std::atomic<int> started{0};
    std::atomic<int> stopped{0};
    std::atomic<int32_t> stopCode{12345};

    void Attach(ITGCapturer& c)
    {
        c.SetOnErrorCallback([this](int32_t what, int32_t extra, const char*) {
            std::lock_guard<std::mutex> lock(mutex);
            errors.emplace_back(what, extra);
        });
        c.SetOnNotifyCallback([this](NotifyMsgType type, const std::string&, int64_t data) {
            std::lock_guard<std::mutex> lock(mutex);
            notifies.emplace_back(static_cast<int32_t>(type), data);
        });
        c.SetOnPreparedCallback([this](ITGCapturer*) { ++prepared; });
        c.SetOnStartCallback([this] { ++started; });
        c.SetOnStopCallback([this](int32_t err) {
            stopCode.store(err);
            ++stopped;
        });
    }

    bool HasError(int32_t what)
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& e : errors) {
            if (e.first == what) return true;
        }
        return false;
    }
    bool HasNotify(int32_t type)
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& n : notifies) {
            if (n.first == type) return true;
        }
        return false;
    }
    size_t ErrorCount()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return errors.size();
    }
    // 等到某个错误码出现（断流判定依赖墙钟，不能靠固定 sleep）
    bool WaitError(int32_t what, std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (HasError(what)) return true;
            std::this_thread::sleep_for(20ms);
        }
        return false;
    }
    bool WaitNotify(int32_t type, std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (HasNotify(type)) return true;
            std::this_thread::sleep_for(20ms);
        }
        return false;
    }
};

void ConfigureValid(ITGCapturer& c, const std::string& out)
{
    VideoEncoderParams vp;
    vp.width = 320;
    vp.height = 240;
    vp.fps = 30;
    vp.vBitrate = 400000;
    TG_EXPECT_EQ(c.SetupVideoEncoder(vp), int{TGErr_OK});

    AudioEncoderParams ap;
    TG_EXPECT_EQ(c.SetupAudioEncoder(ap), int{TGErr_OK});

    MuxerParams mp;
    mp.muxer = "flv";
    mp.muxerOutput = out;
    TG_EXPECT_EQ(c.SetupMuxer(mp), int{TGErr_OK});
}

std::vector<uint8_t> Nv12Frame(int w, int h)
{
    return std::vector<uint8_t>(static_cast<size_t>(w) * h * 3 / 2, 16);
}

// 可观测的假采集源：记录 Open / Close / 线程启停，并能在 Stop 之后尝试继续产帧，
// 用来验证"StopCapture 返回后回调确实被挡住了"。
class ProbeVideoSource : public TGCapture::capture::VideoSourceBase
{
public:
    ProbeVideoSource()
        : VideoSourceBase(TypeOfGameEngine4Slave, VideoScaleStrategy::SCALE_FIT_V0) {}

    int Open(int width, int height, float fps) override
    {
        m_width = width;
        m_height = height;
        m_fps = fps;
        ++opened;
        return 0;
    }

    void Close() override { ++closed; }

    // 模拟平台侧的"外来线程回调"：Stop 之后再调一次，应当被栅栏丢弃
    void EmitOne()
    {
        auto frame = std::make_shared<TGCapture::core::Frame>();
        frame->kind = TGCapture::core::MediaKind::Video;
        frame->width = m_width;
        frame->height = m_height;
        Emit(std::move(frame));
    }

    std::atomic<int> opened{0};
    std::atomic<int> closed{0};

protected:
    void Loop(std::stop_token token) override
    {
        while (!token.stop_requested()) {
            std::this_thread::sleep_for(5ms);
        }
    }
};

// 读回产出文件里视频流声明的分辨率。用来验证"这一轮 Prepare 的参数真的生效了"，
// 而不是复用了上一轮残留的编码器上下文。
bool ProbeVideoSize(const std::string& path, int& width, int& height)
{
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) {
        return false;
    }
    bool found = false;
    if (avformat_find_stream_info(fmt, nullptr) >= 0) {
        for (unsigned i = 0; i < fmt->nb_streams; ++i) {
            if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                width = fmt->streams[i]->codecpar->width;
                height = fmt->streams[i]->codecpar->height;
                found = true;
                break;
            }
        }
    }
    avformat_close_input(&fmt);
    return found;
}

}  // namespace

TG_TEST(ErrorCodes, ValuesAreUniqueAndNegative)
{
    const std::vector<int32_t> all = {
        TGErr_InvalidState,      TGErr_NotConfigured,       TGErr_OpenVideoEncoder,
        TGErr_OpenAudioEncoder,  TGErr_OpenMuxer,           TGErr_OpenVideoSource,
        TGErr_StartFailed,       TGErr_InvalidArgument,     TGErr_NotSupported,
        TGErr_NotImplemented,    TGErr_Internal,            TGErr_MuxWriteFailed,
        TGErr_VideoEncodeFailed, TGErr_AudioEncodeFailed,   TGErr_VideoCaptureStalled,
        TGErr_AudioCaptureStalled, TGErr_MuxTrailerFailed};
    std::set<int32_t> unique(all.begin(), all.end());
    TG_EXPECT_EQ(unique.size(), all.size());   // 同一数值只能有一个含义
    for (int32_t code : all) {
        TG_EXPECT(code < 0);
    }
    TG_EXPECT_EQ(static_cast<int32_t>(TGErr_OK), 0);
}

TG_TEST(ErrorCodes, RuntimeCodesAreBelowCallLevelCodes)
{
    // 宿主按"≤ -100 即运行期故障"粗分类，分段不能互相渗透
    const std::vector<int32_t> runtime = {TGErr_MuxWriteFailed,      TGErr_VideoEncodeFailed,
                                          TGErr_AudioEncodeFailed,   TGErr_VideoCaptureStalled,
                                          TGErr_AudioCaptureStalled, TGErr_MuxTrailerFailed};
    for (int32_t code : runtime) {
        TG_EXPECT(code <= -100);
    }
    const std::vector<int32_t> callLevel = {TGErr_InvalidState,    TGErr_NotConfigured,
                                            TGErr_OpenVideoEncoder, TGErr_OpenAudioEncoder,
                                            TGErr_OpenMuxer,       TGErr_OpenVideoSource,
                                            TGErr_StartFailed,     TGErr_InvalidArgument,
                                            TGErr_NotSupported,    TGErr_NotImplemented,
                                            TGErr_Internal};
    for (int32_t code : callLevel) {
        TG_EXPECT(code > -100);
    }
}

TG_TEST(ErrorCodes, SetupRejectsBadParamsAndKeepsNotConfigured)
{
    TGCapturer c(GameID::GAME_ID_DEFAULT);
    Recorder rec;
    rec.Attach(c);

    VideoEncoderParams vp;
    vp.width = 0;   // 非法
    TG_EXPECT_EQ(c.SetupVideoEncoder(vp), int{TGErr_InvalidArgument});
    vp.width = 320;
    vp.height = 240;
    vp.fps = 0.0f;   // 非法
    TG_EXPECT_EQ(c.SetupVideoEncoder(vp), int{TGErr_InvalidArgument});

    AudioEncoderParams ap;
    ap.sampleRate = 0;
    TG_EXPECT_EQ(c.SetupAudioEncoder(ap), int{TGErr_InvalidArgument});
    ap.enabled = false;   // 纯视频模式：采样率不参与校验
    TG_EXPECT_EQ(c.SetupAudioEncoder(ap), int{TGErr_OK});

    MuxerParams mp;   // muxerOutput 为空
    TG_EXPECT_EQ(c.SetupMuxer(mp), int{TGErr_InvalidArgument});

    // 被拒的参数不会被记下来 → 仍然是"没配过"
    TG_EXPECT_EQ(c.Prepare(), int{TGErr_NotConfigured});
    TG_EXPECT(rec.HasError(TGErr_NotConfigured));   // 返回值与回调 what 一致
}

TG_TEST(ErrorCodes, CallLevelCodesAreDistinct)
{
    TGCapturer c(GameID::GAME_ID_DEFAULT);
    // 未 Start：时序问题
    TG_EXPECT_EQ(c.PushExternalVideoFrame(nullptr, 320, 240, 320, 0), int{TGErr_InvalidState});
    TG_EXPECT_EQ(c.PushExternalAudioFrame(TypeOfGameAudio4Slave, nullptr, 0, 44100, 2, 0),
                 int{TGErr_InvalidState});
    // 未 Prepare 就 Start
    TG_EXPECT_EQ(c.Start(), int{TGErr_InvalidState});
    // 参数问题
    TG_EXPECT_EQ(c.AddAudioSource(nullptr), int{TGErr_InvalidArgument});
    TG_EXPECT_EQ(c.AddVideoSource(nullptr), int{TGErr_InvalidArgument});
    TG_EXPECT_EQ(c.SetAVTimeout(0, 0), int{TGErr_InvalidArgument});
    // 未实现 / 不支持的能力
    TG_EXPECT(c.CreateLocalFileCapture(0, "x.mp4", false) == nullptr);
}

TG_TEST(ErrorCodes, PushRejectsBadArgumentsWhileRunning)
{
    TGCapturer c(GameID::GAME_ID_DEFAULT);
    Recorder rec;
    rec.Attach(c);
    ConfigureValid(c, "/tmp/tg_test_err_push.flv");
    TG_ASSERT(c.Prepare() == TGErr_OK);
    TG_ASSERT(c.Start() == TGErr_OK);

    // 运行中：空数据 / 非法尺寸要和"未 Start"区分开
    TG_EXPECT_EQ(c.PushExternalVideoFrame(nullptr, 320, 240, 320, 0), int{TGErr_InvalidArgument});
    const auto pixels = Nv12Frame(320, 240);
    TG_EXPECT_EQ(c.PushExternalVideoFrame(pixels.data(), 0, 240, 320, 0),
                 int{TGErr_InvalidArgument});
    TG_EXPECT_EQ(c.PushExternalVideoFrame(pixels.data(), 320, 240, 16, 0),
                 int{TGErr_InvalidArgument});   // stride < width
    TG_EXPECT_EQ(c.PushExternalVideoFrame(pixels.data(), 320, 240, 320, 0), int{TGErr_OK});

    const std::vector<uint8_t> pcm(1024 * 2 * 2, 0);
    TG_EXPECT_EQ(c.PushExternalAudioFrame(TypeOfGameAudio4Slave, pcm.data(),
                                          static_cast<int>(pcm.size()), 0, 2, -1),
                 int{TGErr_InvalidArgument});
    TG_EXPECT_EQ(c.PushExternalAudioFrame(TypeOfGameAudio4Slave, pcm.data(),
                                          static_cast<int>(pcm.size()), 44100, 2, -1),
                 int{TGErr_OK});
    c.Stop();
}

TG_TEST(ErrorCodes, DuplicatePrepareIsReportedNotSilent)
{
    TGCapturer c(GameID::GAME_ID_DEFAULT);
    Recorder rec;
    rec.Attach(c);
    ConfigureValid(c, "/tmp/tg_test_err_dup.flv");
    TG_ASSERT(c.Prepare() == TGErr_OK);
    TG_EXPECT_EQ(rec.prepared.load(), 1);
    TG_EXPECT_EQ(rec.ErrorCount(), size_t{0});

    // 回归：重复 Prepare 过去只返回裸 -1，既不在枚举里也不上报
    TG_EXPECT_EQ(c.Prepare(), int{TGErr_InvalidState});
    TG_EXPECT(rec.HasError(TGErr_InvalidState));
    c.Stop();
}

TG_TEST(ErrorCodes, CleanStopReportsZeroToHost)
{
    TGCapturer c(GameID::GAME_ID_DEFAULT);
    Recorder rec;
    rec.Attach(c);
    ConfigureValid(c, "/tmp/tg_test_err_clean.flv");
    TG_ASSERT(c.Prepare() == TGErr_OK);
    TG_ASSERT(c.Start() == TGErr_OK);
    const auto pixels = Nv12Frame(320, 240);
    for (int i = 0; i < 5; ++i) {
        c.PushExternalVideoFrame(pixels.data(), 320, 240, 320, -1);
        std::this_thread::sleep_for(10ms);
    }
    c.Stop();
    TG_EXPECT_EQ(rec.started.load(), 1);
    TG_EXPECT_EQ(rec.stopped.load(), 1);
    TG_EXPECT_EQ(rec.stopCode.load(), int32_t{0});   // 全程无错误
}

TG_TEST(ErrorCodes, CaptureStallIsReportedAndRecoveryNotified)
{
    // 这条覆盖最难观测的一类故障：采集"不再有数据"（设备丢失 / 权限被撤销 / 上层停推）。
    // 此前没有任何探测，宿主只能看到黑屏。
    TGCapturer c(GameID::GAME_ID_DEFAULT);
    Recorder rec;
    rec.Attach(c);
    ConfigureValid(c, "/tmp/tg_test_err_stall.flv");
    TG_EXPECT_EQ(c.SetAVTimeout(300, 300), int{TGErr_OK});   // 缩短阈值，别让单测跑太久
    TG_ASSERT(c.Prepare() == TGErr_OK);
    TG_ASSERT(c.Start() == TGErr_OK);

    // 不推任何帧：视频必然断流
    TG_EXPECT(rec.WaitError(TGErr_VideoCaptureStalled, 3000ms));
    TG_EXPECT(rec.HasError(TGErr_AudioCaptureStalled));

    // 恢复：重新推帧后应收到恢复通知
    const auto pixels = Nv12Frame(320, 240);
    for (int i = 0; i < 10; ++i) {
        c.PushExternalVideoFrame(pixels.data(), 320, 240, 320, -1);
        std::this_thread::sleep_for(10ms);
    }
    TG_EXPECT(rec.WaitNotify(EsCap_Notify_VideoCapture_Recovered, 2000ms));

    c.Stop();
    // Stop 带出本轮最后一个错误码，宿主据此知道这一轮不是干净收尾
    TG_EXPECT(rec.stopCode.load() < 0);
}

TG_TEST(ErrorCodes, OpenMuxerFailureCarriesUnderlyingCode)
{
    TGCapturer c(GameID::GAME_ID_DEFAULT);
    Recorder rec;
    rec.Attach(c);
    // 不存在的目录：avio_open 必失败
    ConfigureValid(c, "/tmp/tg_no_such_dir_xyz/out.flv");
    TG_EXPECT_EQ(c.Prepare(), int{TGErr_OpenMuxer});
    TG_EXPECT(rec.HasError(TGErr_OpenMuxer));
    std::lock_guard<std::mutex> lock(rec.mutex);
    bool hasUnderlying = false;
    for (const auto& e : rec.errors) {
        if (e.first == TGErr_OpenMuxer && e.second != 0) {
            hasUnderlying = true;   // extra = 底层 AVERROR，不再丢失诊断信息
        }
    }
    TG_EXPECT(hasUnderlying);
}

TG_TEST(ErrorCodes, PrepareFailureLeavesObjectReusable)
{
    // 失败后必须能原地重配重试，否则宿主只能销毁重建
    TGCapturer c(GameID::GAME_ID_DEFAULT);
    Recorder rec;
    rec.Attach(c);
    ConfigureValid(c, "/tmp/tg_no_such_dir_xyz/out.flv");
    TG_ASSERT(c.Prepare() == TGErr_OpenMuxer);
    ConfigureValid(c, "/tmp/tg_test_err_retry.flv");
    TG_EXPECT_EQ(c.Prepare(), int{TGErr_OK});
    c.Stop();
}

TG_TEST(ErrorCodes, PrepareFailureRollsBackOpenedEncoders)
{
    // 回归：Prepare 在 muxer 这一步失败时，前面已经打开的编码器必须被关掉。
    // 不回滚的话，编码器的 Open() 开头 `if (m_opened) return 0` 会让下一轮静默复用上一轮的
    // 上下文 —— 第二次 Prepare 返回成功，但新设的分辨率根本没生效（产出文件仍是旧尺寸）。
    const std::string out = "/tmp/tg_test_err_rollback.flv";
    std::remove(out.c_str());

    TGCapturer c(GameID::GAME_ID_DEFAULT);
    Recorder rec;
    rec.Attach(c);

    // 第一轮：320x240，输出目录不存在 → 卡在 muxer，此时两个编码器已经打开
    ConfigureValid(c, "/tmp/tg_no_such_dir_xyz/out.flv");
    TG_ASSERT(c.Prepare() == TGErr_OpenMuxer);

    // 第二轮：换成 640x480 且输出可写
    VideoEncoderParams vp;
    vp.width = 640;
    vp.height = 480;
    vp.fps = 30;
    vp.vBitrate = 400000;
    // 用外部时间轴：本用例不接音频源，走音频主时钟的话帧会被同步逻辑丢掉，写不出关键帧
    vp.videoUseExternalPts = true;
    TG_EXPECT_EQ(c.SetupVideoEncoder(vp), int{TGErr_OK});
    AudioEncoderParams ap;
    TG_EXPECT_EQ(c.SetupAudioEncoder(ap), int{TGErr_OK});
    MuxerParams mp;
    mp.muxer = "flv";
    mp.muxerOutput = out;
    TG_EXPECT_EQ(c.SetupMuxer(mp), int{TGErr_OK});

    TG_ASSERT(c.Prepare() == TGErr_OK);
    TG_ASSERT(c.Start() == TGErr_OK);
    // flv 的分辨率写在首个关键帧里，只写 header 探不出来，必须真的推几帧
    const auto pixels = Nv12Frame(640, 480);
    for (int i = 0; i < 10; ++i) {
        TG_EXPECT_EQ(c.PushExternalVideoFrame(pixels.data(), 640, 480, 640, i * 33333),
                     int{TGErr_OK});
        std::this_thread::sleep_for(10ms);
    }
    c.Stop();

    int w = 0;
    int h = 0;
    TG_ASSERT(ProbeVideoSize(out, w, h));
    TG_EXPECT_EQ(w, 640);   // 复用残留编码器的话这里会是 320
    TG_EXPECT_EQ(h, 480);
    std::remove(out.c_str());
}

TG_TEST(ErrorCodes, StopClosesCaptureSourceAndBlocksLateCallbacks)
{
    // 两条新契约：
    //   1) Stop 会调采集源的 Close()，设备资源不再留到析构
    //   2) StopCapture 返回后，平台侧迟到的回调会被栅栏挡住，不会再进下游
    ProbeVideoSource src;
    const std::string out = "/tmp/tg_test_err_srcclose.flv";
    {
        TGCapturer c(GameID::GAME_ID_DEFAULT);
        Recorder rec;
        rec.Attach(c);
        TG_ASSERT(c.AddVideoSource(&src) == TGErr_OK);
        ConfigureValid(c, out);
        TG_ASSERT(c.Prepare() == TGErr_OK);
        TG_EXPECT_EQ(src.opened.load(), 1);
        TG_EXPECT_EQ(src.closed.load(), 0);
        TG_ASSERT(c.Start() == TGErr_OK);
        std::this_thread::sleep_for(50ms);

        c.Stop();
        TG_EXPECT_EQ(src.closed.load(), 1);   // Stop 必须释放采集源

        // 模拟"Stop 之后才到达的平台回调"：应当被丢弃，且不崩
        const uint64_t before = src.EmittedFrames();
        src.EmitOne();
        TG_EXPECT_EQ(src.EmittedFrames(), before);
    }
    std::remove(out.c_str());
}

TG_TEST(ErrorCodes, PrepareFailureClosesAlreadyOpenedSource)
{
    // 采集源先 Open 成功、后续步骤失败：回滚必须把它关掉，
    // 否则设备一直被占着，而宿主没有任何接口能释放（Stop 在 Idle 态是空操作）
    ProbeVideoSource src;
    TGCapturer c(GameID::GAME_ID_DEFAULT);
    Recorder rec;
    rec.Attach(c);
    TG_ASSERT(c.AddVideoSource(&src) == TGErr_OK);
    ConfigureValid(c, "/tmp/tg_no_such_dir_xyz/out.flv");
    TG_ASSERT(c.Prepare() == TGErr_OpenMuxer);
    TG_EXPECT_EQ(src.opened.load(), 1);
    TG_EXPECT_EQ(src.closed.load(), 1);
}

TG_TEST(ErrorCodes, ConcurrentStopIsSerializedAndSettled)
{
    // 并发 Stop：返回时必须保证停机（含事件收尾）真的完成了 ——
    // 否则调用方转头 delete，而另一个线程还在 PublishSync 里回调宿主
    TGCapturer c(GameID::GAME_ID_DEFAULT);
    Recorder rec;
    rec.Attach(c);
    ConfigureValid(c, "/tmp/tg_test_err_concurrent.flv");
    TG_ASSERT(c.Prepare() == TGErr_OK);
    TG_ASSERT(c.Start() == TGErr_OK);

    std::vector<std::thread> threads;
    threads.reserve(4);
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&c] { c.Stop(); });
    }
    for (auto& t : threads) {
        t.join();
    }
    // 只有真正执行停机的那一个会发 Stopped，其余等它收尾后看到 Idle 直接返回
    TG_EXPECT_EQ(rec.stopped.load(), 1);
    std::remove("/tmp/tg_test_err_concurrent.flv");
}

TG_TEST(ErrorCodes, StopFromStopCallbackDoesNotDeadlock)
{
    // 宿主在 OnStop 里再调一次 Stop（常见的"确保停干净"写法）：
    // 停机线程自己重入必须直接放行，不能卡在等自己收尾上
    TGCapturer c(GameID::GAME_ID_DEFAULT);
    std::atomic<int> stopped{0};
    c.SetOnStopCallback([&](int32_t) {
        if (++stopped == 1) {
            c.Stop();   // 重入
        }
    });
    ConfigureValid(c, "/tmp/tg_test_err_reentrant.flv");
    TG_ASSERT(c.Prepare() == TGErr_OK);
    TG_ASSERT(c.Start() == TGErr_OK);
    c.Stop();
    TG_EXPECT_EQ(stopped.load(), 1);   // 重入那次直接返回，不会再发一遍 Stopped
    std::remove("/tmp/tg_test_err_reentrant.flv");
}

TG_TEST(ErrorCodes, StopAfterFailedPrepareIsCleanNoop)
{
    // 宿主拿到 Prepare 失败后通常会顺手 Stop 一次：这不该再报错、也不该触发 Stopped 回调，
    // 因为失败分支已经自己回滚到 Idle 了
    TGCapturer c(GameID::GAME_ID_DEFAULT);
    Recorder rec;
    rec.Attach(c);
    ConfigureValid(c, "/tmp/tg_no_such_dir_xyz/out.flv");
    TG_ASSERT(c.Prepare() == TGErr_OpenMuxer);
    const size_t before = rec.ErrorCount();
    c.Stop();
    TG_EXPECT_EQ(rec.ErrorCount(), before);
    TG_EXPECT_EQ(rec.stopped.load(), 0);
}
