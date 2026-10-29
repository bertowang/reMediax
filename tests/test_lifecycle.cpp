// 生命周期与开播前预览：
//   Idle ─StartPreview→ Previewing ─Prepare→ Prepared ─Start→ Running ─Stop→ Previewing ─StopPreview→ Idle
// 重点验证：预览不依赖推流；停播回到预览时视频源不重开、预览不中断；预览中 Prepare 失败不打断预览；
// 只有预览时 Stop 是空操作；推流中关预览不影响推流
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
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

std::string TmpPath(const char* name)
{
    return (std::filesystem::temp_directory_path() / name).string();
}

// 外部注入型假源：记录 Open / Close / 启停次数（帧由测试经 PushExternalVideoFrame 送入）
class CountingSource : public capture::VideoSourceBase
{
public:
    CountingSource() : VideoSourceBase(TypeOfGameEngine4Slave, VideoScaleStrategy::SCALE_FIT_V0) {}
    int Open(int width, int height, float fps) override
    {
        m_width = width;
        m_height = height;
        m_fps = fps;
        ++opened;
        return 0;
    }
    void Close() override { ++closed; }
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

struct PreviewProbe
{
    std::atomic<int> frames{0};
    std::atomic<int> lastWidth{0};
    std::atomic<int> lastHeight{0};
    std::atomic<int> badFrames{0};

    void Attach(ITGCapturer& c)
    {
        c.SetOnPreviewFrameCallback([this](const PreviewFrame& f) {
            if (f.format != PreviewPixelFormat::NV12 || !f.y || !f.uv || f.yStride < f.width) {
                ++badFrames;
            }
            lastWidth.store(f.width);
            lastHeight.store(f.height);
            ++frames;
        });
    }
    bool WaitFrames(int atLeast, std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (frames.load() >= atLeast) return true;
            std::this_thread::sleep_for(5ms);
        }
        return frames.load() >= atLeast;
    }
};

void SetupVideo(ITGCapturer& c, int w = 320, int h = 240)
{
    VideoEncoderParams vp;
    vp.width = w;
    vp.height = h;
    vp.fps = 30;
    vp.vBitrate = 400000;
    TG_EXPECT_EQ(c.SetupVideoEncoder(vp), int{TGErr_OK});
}

void SetupRest(ITGCapturer& c, const std::string& out, bool audio = false)
{
    AudioEncoderParams ap;
    ap.enabled = audio;
    TG_EXPECT_EQ(c.SetupAudioEncoder(ap), int{TGErr_OK});
    MuxerParams mp;
    mp.muxer = "flv";
    mp.muxerOutput = out;
    TG_EXPECT_EQ(c.SetupMuxer(mp), int{TGErr_OK});
}

// 推 n 帧（33ms 间隔不需要真实等待：纯视频走采集时间轴，但 PTS 要单调，按真实到达时刻）
int PushFrames(ITGCapturer& c, int n, int w = 320, int h = 240)
{
    std::vector<uint8_t> nv12(static_cast<size_t>(w) * h * 3 / 2, 64);
    int ok = 0;
    for (int i = 0; i < n; ++i) {
        if (c.PushExternalVideoFrame(nv12.data(), w, h, w, -1) == TGErr_OK) {
            ++ok;
        }
        std::this_thread::sleep_for(3ms);
    }
    return ok;
}

int CountVideoPackets(const std::string& path)
{
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) {
        return -1;
    }
    int count = 0;
    if (avformat_find_stream_info(fmt, nullptr) >= 0) {
        AVPacket* pkt = av_packet_alloc();
        while (av_read_frame(fmt, pkt) >= 0) {
            if (fmt->streams[pkt->stream_index]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                ++count;
            }
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);
    }
    avformat_close_input(&fmt);
    return count;
}

}  // namespace

TG_TEST(Lifecycle, PreviewRequiresVideoParams)
{
    TGCapturer c(GameID::GAME_ID_DEFAULT);
    TG_EXPECT_EQ(c.StartPreview(), int{TGErr_NotConfigured});
    TG_EXPECT(!c.IsPreviewing());
}

TG_TEST(Lifecycle, PreviewWithoutBroadcast)
{
    CountingSource src;
    TGCapturer c(GameID::GAME_ID_DEFAULT);
    PreviewProbe probe;
    probe.Attach(c);
    TG_ASSERT(c.AddVideoSource(&src) == TGErr_OK);
    SetupVideo(c);

    // 预览之前注入视频：InvalidState
    std::vector<uint8_t> nv12(320 * 240 * 3 / 2, 64);
    TG_EXPECT_EQ(c.PushExternalVideoFrame(nv12.data(), 320, 240, 320, -1), int{TGErr_InvalidState});

    TG_ASSERT(c.StartPreview() == TGErr_OK);
    TG_EXPECT(c.IsPreviewing());
    TG_EXPECT_EQ(src.opened.load(), 1);
    TG_EXPECT_EQ(c.StartPreview(), int{TGErr_OK});   // 重复开：幂等
    TG_EXPECT_EQ(src.opened.load(), 1);

    TG_EXPECT_EQ(PushFrames(c, 5), 5);
    TG_EXPECT(probe.WaitFrames(1, 1000ms));
    TG_EXPECT_EQ(probe.lastWidth.load(), 320);
    TG_EXPECT_EQ(probe.lastHeight.load(), 240);
    TG_EXPECT_EQ(probe.badFrames.load(), 0);

    // 只有预览时 Stop 是空操作，预览继续
    c.Stop();
    TG_EXPECT(c.IsPreviewing());
    TG_EXPECT_EQ(src.closed.load(), 0);

    c.StopPreview();
    TG_EXPECT(!c.IsPreviewing());
    TG_EXPECT_EQ(src.closed.load(), 1);
    const int before = probe.frames.load();
    TG_EXPECT_EQ(c.PushExternalVideoFrame(nv12.data(), 320, 240, 320, -1), int{TGErr_InvalidState});
    std::this_thread::sleep_for(20ms);
    TG_EXPECT_EQ(probe.frames.load(), before);
}

TG_TEST(Lifecycle, PreviewThenBroadcastThenBackToPreview)
{
    CountingSource src;
    const std::string out1 = TmpPath("tg_test_lifecycle_1.flv");
    const std::string out2 = TmpPath("tg_test_lifecycle_2.flv");
    {
        TGCapturer c(GameID::GAME_ID_DEFAULT);
        PreviewProbe probe;
        probe.Attach(c);
        std::atomic<int> firstFrame{0};
        std::atomic<int> stopped{0};
        c.SetOnNotifyCallback([&](NotifyMsgType t, const std::string&, int64_t) {
            if (t == EsCap_Notify_Fisrt_Frame_OK) ++firstFrame;
        });
        c.SetOnStopCallback([&](int32_t) { ++stopped; });
        TG_ASSERT(c.AddVideoSource(&src) == TGErr_OK);
        SetupVideo(c);
        SetupRest(c, out1);

        TG_ASSERT(c.StartPreview() == TGErr_OK);
        PushFrames(c, 3);
        TG_EXPECT(probe.WaitFrames(1, 1000ms));

        // 预览中开播：视频源不重开
        TG_ASSERT(c.Prepare() == TGErr_OK);
        TG_ASSERT(c.Start() == TGErr_OK);
        TG_EXPECT_EQ(src.opened.load(), 1);
        const int previewBefore = probe.frames.load();
        TG_EXPECT_EQ(PushFrames(c, 20), 20);
        TG_EXPECT(probe.WaitFrames(previewBefore + 1, 1000ms));   // 推流中预览照常

        // 停播：回到预览，视频源不关
        c.Stop();
        TG_EXPECT_EQ(stopped.load(), 1);
        TG_EXPECT(c.IsPreviewing());
        TG_EXPECT_EQ(src.closed.load(), 0);
        const int afterStop = probe.frames.load();
        TG_EXPECT_EQ(PushFrames(c, 3), 3);
        TG_EXPECT(probe.WaitFrames(afterStop + 1, 1000ms));

        // 第二轮：仍从预览开播，首帧通知再来一次
        SetupRest(c, out2);
        TG_ASSERT(c.Prepare() == TGErr_OK);
        TG_ASSERT(c.Start() == TGErr_OK);
        TG_EXPECT_EQ(PushFrames(c, 20), 20);
        c.Stop();
        TG_EXPECT_EQ(stopped.load(), 2);
        TG_EXPECT_EQ(src.opened.load(), 1);
        TG_EXPECT_EQ(firstFrame.load(), 2);

        c.StopPreview();
        TG_EXPECT_EQ(src.closed.load(), 1);
    }
    TG_EXPECT(CountVideoPackets(out1) >= 15);
    TG_EXPECT(CountVideoPackets(out2) >= 15);
    std::remove(out1.c_str());
    std::remove(out2.c_str());
}

TG_TEST(Lifecycle, StopPreviewWhileBroadcasting)
{
    CountingSource src;
    const std::string out = TmpPath("tg_test_lifecycle_3.flv");
    {
        TGCapturer c(GameID::GAME_ID_DEFAULT);
        PreviewProbe probe;
        probe.Attach(c);
        TG_ASSERT(c.AddVideoSource(&src) == TGErr_OK);
        SetupVideo(c);
        SetupRest(c, out);
        TG_ASSERT(c.Prepare() == TGErr_OK);
        TG_ASSERT(c.Start() == TGErr_OK);
        // 推流中开预览
        TG_ASSERT(c.StartPreview() == TGErr_OK);
        PushFrames(c, 10);
        TG_EXPECT(probe.WaitFrames(1, 1000ms));
        // 推流中关预览：推流不受影响
        c.StopPreview();
        TG_EXPECT(!c.IsPreviewing());
        TG_EXPECT_EQ(src.closed.load(), 0);
        TG_EXPECT_EQ(PushFrames(c, 10), 10);
        c.Stop();
        TG_EXPECT_EQ(src.closed.load(), 1);   // 预览已关：停播即关源
        TG_EXPECT_EQ(c.PushExternalVideoFrame(nullptr, 320, 240, 320, -1), int{TGErr_InvalidState});
    }
    TG_EXPECT(CountVideoPackets(out) >= 15);
    std::remove(out.c_str());
}

TG_TEST(Lifecycle, PrepareFailureKeepsPreview)
{
    CountingSource src;
    CountingSource other;   // 源必须比 capturer 活得久（capturer 析构时会解绑源回调）
    TGCapturer c(GameID::GAME_ID_DEFAULT);
    PreviewProbe probe;
    probe.Attach(c);
    TG_ASSERT(c.AddVideoSource(&src) == TGErr_OK);
    SetupVideo(c);
    SetupRest(c, TmpPath("tg_no_such_dir_xyz/out.flv"));
    TG_ASSERT(c.StartPreview() == TGErr_OK);
    TG_EXPECT_EQ(c.Prepare(), int{TGErr_OpenMuxer});
    // 回到预览：源不关，预览继续出帧
    TG_EXPECT(c.IsPreviewing());
    TG_EXPECT_EQ(src.closed.load(), 0);
    const int before = probe.frames.load();
    TG_EXPECT_EQ(PushFrames(c, 3), 3);
    TG_EXPECT(probe.WaitFrames(before + 1, 1000ms));
    // 不能在预览中换主画面源
    TG_EXPECT_EQ(c.AddVideoSource(&other), int{TGErr_InvalidState});
    c.StopPreview();
    TG_EXPECT_EQ(src.closed.load(), 1);
    TG_EXPECT_EQ(c.AddVideoSource(&other), int{TGErr_OK});
    c.SetOnPreviewFrameCallback(nullptr);
}

TG_TEST(Lifecycle, DestroyWhilePreviewingAndBroadcasting)
{
    CountingSource src;
    const std::string out = TmpPath("tg_test_lifecycle_4.flv");
    {
        TGCapturer c(GameID::GAME_ID_DEFAULT);
        PreviewProbe probe;
        probe.Attach(c);
        TG_ASSERT(c.AddVideoSource(&src) == TGErr_OK);
        SetupVideo(c);
        SetupRest(c, out);
        TG_ASSERT(c.StartPreview() == TGErr_OK);
        TG_ASSERT(c.Prepare() == TGErr_OK);
        TG_ASSERT(c.Start() == TGErr_OK);
        PushFrames(c, 5);
        // 直接析构：必须停播 + 关预览 + 关源，且不崩
    }
    TG_EXPECT_EQ(src.closed.load(), 1);
    std::remove(out.c_str());
}
