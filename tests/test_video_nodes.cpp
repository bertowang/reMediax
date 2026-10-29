// 视频链路节点：vsrc.* → video_compositor → video_sync → (编码器)
// 用一个收帧节点代替编码器，验证：合成器的启停闸 / 暂停图层 / 预览口扇出，
// video_sync 的背压丢帧 / 外部时间轴 / 无音频时的采集时间轴 / 等音频首帧 / 首帧通知
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include "core/AVSyncClock.h"
#include "core/Bus.h"
#include "nodes/VideoNodes.h"
#include "tg_test.h"

using namespace TGCapture;
using namespace TGCapture::core;

namespace
{

class SinkNode : public BusNode
{
public:
    explicit SinkNode(std::string name) : BusNode(std::move(name)) {}

    bool Submit(FramePtr frame) override
    {
        std::lock_guard<std::mutex> lock(mutex);
        frames.push_back(std::move(frame));
        return true;
    }
    size_t Pending() const override { return pending.load(); }

    size_t Count()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return frames.size();
    }
    FramePtr At(size_t i)
    {
        std::lock_guard<std::mutex> lock(mutex);
        return frames.at(i);
    }

    std::mutex mutex;
    std::vector<FramePtr> frames;
    std::atomic<size_t> pending{0};
};

FramePtr CpuFrame(int64_t captureUs)
{
    auto f = std::make_shared<Frame>();
    f->kind = MediaKind::Video;
    f->width = 16;
    f->height = 16;
    f->pixFmt = AV_PIX_FMT_NV12;
    auto buf = std::shared_ptr<uint8_t>(new uint8_t[16 * 24](), std::default_delete<uint8_t[]>());
    f->planes[0] = buf;
    f->strides[0] = 16;
    f->planes[1] = std::shared_ptr<uint8_t>(buf, buf.get() + 16 * 16);
    f->strides[1] = 16;
    f->captureUs = captureUs;
    return f;
}

FramePtr HwFrame(int64_t captureUs)
{
    auto f = std::make_shared<Frame>();
    f->kind = MediaKind::Video;
    f->width = 16;
    f->height = 16;
    static int dummy = 0;
    f->hwHandle = &dummy;
    f->hwKind = HwKind::CVPixelBuffer;
    f->captureUs = captureUs;
    return f;
}

// 整条视频链路 + 收帧节点（代替编码器）+ 可选的预览收帧节点
struct Chain
{
    Bus bus;
    std::shared_ptr<nodes::VideoSourceNode> src = std::make_shared<nodes::VideoSourceNode>("vsrc.main");
    std::shared_ptr<nodes::VideoCompositorNode> comp = std::make_shared<nodes::VideoCompositorNode>();
    std::shared_ptr<nodes::VideoSyncNode> sync = std::make_shared<nodes::VideoSyncNode>();
    std::shared_ptr<SinkNode> sink = std::make_shared<SinkNode>("encoder");
    std::shared_ptr<SinkNode> preview = std::make_shared<SinkNode>("preview");
    std::shared_ptr<AudioClock> audioClock = std::make_shared<AudioClock>();
    std::shared_ptr<VideoPtsGenerator> ptsGen = std::make_shared<VideoPtsGenerator>();
    std::shared_ptr<VideoOnlyTimeline> timeline = std::make_shared<VideoOnlyTimeline>();

    explicit Chain(bool withPreview = false, bool audioEnabled = false)
    {
        bus.Context().Provide(audioClock);
        bus.Context().Provide(ptsGen);
        bus.Context().Provide(timeline);
        auto tx = bus.Begin();
        tx.Register("vsrc.main", src)
            .Register("video_compositor", comp)
            .Register("video_sync", sync)
            .Register("encoder", sink)
            .Link("vsrc.main", 0, "video_compositor", 0)
            .Link("video_compositor", 0, "video_sync", 0)
            .Link("video_sync", "encoder");
        if (withPreview) {
            tx.Register("preview", preview).Link("video_compositor", 1, "preview", 0);
        }
        TG_ASSERT(tx.Commit());
        nodes::VideoSyncNode::Config cfg;
        cfg.audioEnabled = audioEnabled;
        cfg.frameIntervalUs = 33333;
        sync->SetConfig(cfg);
        sync->Reset();
    }

    void StartAll()
    {
        TG_ASSERT(bus.StartGroup(NodeGroup::Output).ok);
        TG_ASSERT(bus.StartGroup(NodeGroup::Preview).ok);
    }

    void StopAll()
    {
        bus.StopGroup(NodeGroup::Preview);
        bus.StopGroup(NodeGroup::Output);
    }
};

}  // namespace

TG_TEST(VideoNodes, FramesFlowOnlyWhileRunning)
{
    Chain c;
    // 未启动：合成器拒收
    TG_EXPECT(!c.src->Push(CpuFrame(1000)));
    TG_EXPECT_EQ(c.sink->Count(), 0u);

    c.StartAll();
    TG_EXPECT(c.src->Push(CpuFrame(1000)));
    TG_EXPECT(c.src->Push(CpuFrame(34333)));
    TG_EXPECT_EQ(c.sink->Count(), 2u);
    // 无音频：采集时间轴，首帧归零、单调递增
    TG_EXPECT_EQ(c.sink->At(0)->ptsUs, 0);
    TG_EXPECT(c.sink->At(1)->ptsUs > c.sink->At(0)->ptsUs);

    c.StopAll();
    // 源节点是受控输出：停止后关闸
    TG_EXPECT(!c.src->Push(CpuFrame(67666)));
    TG_EXPECT_EQ(c.sink->Count(), 2u);
}

TG_TEST(VideoNodes, PreviewOutputRunsWithoutOutputGroup)
{
    Chain c(/*withPreview=*/true);
    // 只启动 preview 组（= 开播前预览）：预览口有帧，video_sync 未启动，编码侧收不到
    TG_ASSERT(c.bus.StartGroup(NodeGroup::Preview).ok);
    c.src->Push(CpuFrame(1000));
    c.src->Push(CpuFrame(2000));
    TG_EXPECT_EQ(c.preview->Count(), 2u);
    TG_EXPECT_EQ(c.sink->Count(), 0u);

    // 再启动 output 组：两路都有
    TG_ASSERT(c.bus.StartGroup(NodeGroup::Output).ok);
    c.src->Push(CpuFrame(3000));
    TG_EXPECT_EQ(c.preview->Count(), 3u);
    TG_EXPECT_EQ(c.sink->Count(), 1u);
    c.StopAll();
}

TG_TEST(VideoNodes, MutedWithoutPauseImage)
{
    Chain c;
    c.StartAll();
    c.comp->SetMuted(true);
    // 没有暂停图：CPU 帧原样输出（冻结语义），硬件帧无法替换只能丢
    TG_EXPECT(c.src->Push(CpuFrame(1000)));
    c.src->Push(HwFrame(2000));
    TG_EXPECT_EQ(c.sink->Count(), 1u);
    c.comp->SetMuted(false);
    c.src->Push(HwFrame(3000));
    TG_EXPECT_EQ(c.sink->Count(), 2u);
    c.StopAll();
}

TG_TEST(VideoNodes, SyncBackpressureDrops)
{
    Chain c;
    c.StartAll();
    c.sink->pending.store(8);
    c.src->Push(CpuFrame(1000));
    TG_EXPECT_EQ(c.sink->Count(), 0u);
    TG_EXPECT_EQ(c.sync->BackpressureDropped(), 1u);
    c.sink->pending.store(7);
    c.src->Push(CpuFrame(2000));
    TG_EXPECT_EQ(c.sink->Count(), 1u);
    c.sync->Reset();
    TG_EXPECT_EQ(c.sync->BackpressureDropped(), 0u);
    c.StopAll();
}

TG_TEST(VideoNodes, ExternalTimelineKeepsPts)
{
    Chain c;
    c.StartAll();
    auto f = CpuFrame(1000);
    f->ptsUs = 123456;
    f->ptsLocked = true;
    c.src->Push(f);
    TG_ASSERT(c.sink->Count() == 1u);
    TG_EXPECT_EQ(c.sink->At(0)->ptsUs, 123456);
    c.StopAll();
}

TG_TEST(VideoNodes, WaitsForAudioClockFirstTick)
{
    Chain c(/*withPreview=*/false, /*audioEnabled=*/true);
    c.sync->SetAudioTimeoutMs(5000);
    c.StartAll();
    // 音频主时钟尚未产出第一拍：等待窗口内不发帧，也不计丢帧
    c.src->Push(CpuFrame(1000));
    TG_EXPECT_EQ(c.sink->Count(), 0u);
    TG_EXPECT_EQ(c.sync->SyncDropped(), 0u);
    // 主时钟就绪后按音频时钟出帧
    c.audioClock->AdvanceTo(100000);
    c.src->Push(CpuFrame(2000));
    TG_EXPECT_EQ(c.sink->Count(), 1u);
    c.StopAll();
}

TG_TEST(VideoNodes, FirstFrameNotifyOncePerRound)
{
    Chain c;
    std::atomic<int> firstFrame{0};
    c.bus.Subscribe([&](const BusEvent& e) {
        if (e.type == BusEventType::Notify && e.code == EsCap_Notify_Fisrt_Frame_OK) {
            ++firstFrame;
        }
    });
    c.StartAll();
    c.src->Push(CpuFrame(1000));
    c.src->Push(CpuFrame(2000));
    c.bus.Events().Drain();
    TG_EXPECT_EQ(firstFrame.load(), 1);
    // 下一轮复位后再触发一次
    c.StopAll();
    c.sync->Reset();
    c.StartAll();
    c.src->Push(CpuFrame(3000));
    c.bus.Events().Drain();
    TG_EXPECT_EQ(firstFrame.load(), 2);
    c.StopAll();
    c.bus.Events().Shutdown();
}
