// 文件视频源与视频叠加层（暂存 / 提交）：
//   FileVideoSource：按时间戳实时出帧、内容对得上、无缝循环、播完 EOF 后重复最后一帧、Close 后重开从头播；
//   合成器：推流场景 / 预览场景分开画（暂存的修改只进预览）、透明度 / alpha / 层级 / 视频输入口 / 越界裁剪 / 尺寸跟随；
//   门面：参数校验、预览先看到暂存的修改、提交原子生效（同一帧里两层同时出现 / 消失）、视频层（只改几何时沿用内容）、
//         EOF 通知、本地文件作主画面
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "TGCapturer.h"
#include "capture/FileVideoSource.h"
#include "core/Bus.h"
#include "core/Clock.h"
#include "core/Overlay.h"
#include "nodes/VideoNodes.h"
#include "test_media.h"
#include "tg_test.h"

using namespace TGCapture;
using namespace std::chrono_literals;

namespace
{

std::string TmpPath(const char* name)
{
    return (std::filesystem::temp_directory_path() / name).string();
}

core::FramePtr MakeNv12(int w, int h, uint8_t y)
{
    const size_t ySize = static_cast<size_t>(w) * h;
    auto buffer = std::shared_ptr<uint8_t>(new uint8_t[ySize + ySize / 2], std::default_delete<uint8_t[]>());
    std::fill_n(buffer.get(), ySize, y);
    std::fill_n(buffer.get() + ySize, ySize / 2, uint8_t{128});
    auto f = std::make_shared<core::Frame>();
    f->kind = core::MediaKind::Video;
    f->width = w;
    f->height = h;
    f->pixFmt = AV_PIX_FMT_NV12;
    f->planes[0] = buffer;
    f->strides[0] = w;
    f->planes[1] = std::shared_ptr<uint8_t>(buffer, buffer.get() + ySize);
    f->strides[1] = w;
    f->captureUs = core::MonotonicClock::NowUs();
    return f;
}

int LumaOf(const core::FramePtr& f, int x, int y)
{
    return f ? f->planes[0].get()[static_cast<size_t>(y) * f->strides[0] + x] : -1;
}

class SinkNode : public core::BusNode
{
public:
    explicit SinkNode(std::string name) : core::BusNode(std::move(name)) {}
    bool Submit(core::FramePtr frame) override
    {
        std::lock_guard<std::mutex> lock(mutex);
        last = std::move(frame);
        return true;
    }
    size_t Pending() const override { return 0; }
    core::FramePtr Last()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return last;
    }
    std::mutex mutex;
    core::FramePtr last;
};

// 合成器 + 两个收帧节点（推流口 / 预览口都是 Direct 连线，Submit 返回时帧已到）
struct CompositorRig
{
    core::Bus bus;
    std::shared_ptr<nodes::VideoCompositorNode> comp = std::make_shared<nodes::VideoCompositorNode>();
    std::shared_ptr<SinkNode> program = std::make_shared<SinkNode>("program");
    std::shared_ptr<SinkNode> preview = std::make_shared<SinkNode>("preview");

    CompositorRig()
    {
        bus.Begin()
            .Register("video_compositor", comp)
            .Register("program", program)
            .Register("preview", preview)
            .Link("video_compositor", nodes::VideoCompositorNode::kProgramOutput, "program", 0)
            .Link("video_compositor", nodes::VideoCompositorNode::kPreviewOutput, "preview", 0)
            .Commit();
        comp->StartNode();
    }
    ~CompositorRig() { comp->StopNode(); }

    void Feed(int w = 320, int h = 240, uint8_t y = 50) { comp->Submit(MakeNv12(w, h, y)); }
};

std::shared_ptr<const core::Nv12AImage> SolidImage(int w, int h, uint8_t y, bool leftHalfTransparent = false)
{
    auto img = std::make_shared<core::Nv12AImage>();
    img->width = w;
    img->height = h;
    img->y.assign(static_cast<size_t>(w) * h, y);
    img->uv.assign(static_cast<size_t>(w) * h / 2, 128);
    if (leftHalfTransparent) {
        img->a.resize(static_cast<size_t>(w) * h);
        for (int row = 0; row < h; ++row)
            for (int col = 0; col < w; ++col) img->a[static_cast<size_t>(row) * w + col] = col < w / 2 ? 0 : 255;
    }
    return img;
}

core::OverlayLayer ImageLayer(const std::string& id, std::shared_ptr<const core::Nv12AImage> img, float x, float y,
                              float w, float h, float opacity = 1.0f, int z = 0)
{
    core::OverlayLayer l;
    l.desc.id = id;
    l.desc.kind = OverlayKind::Image;
    l.desc.path = "mem";
    l.desc.x = x;
    l.desc.y = y;
    l.desc.width = w;
    l.desc.height = h;
    l.desc.opacity = opacity;
    l.desc.zOrder = z;
    l.srcWidth = img->width;
    l.srcHeight = img->height;
    l.image = std::move(img);
    return l;
}

core::OverlayScenePtr Scene(std::vector<core::OverlayLayer> layers)
{
    auto s = std::make_shared<core::OverlayScene>();
    s->layers = std::move(layers);
    return s;
}

// FileVideoSource 直连：记录每帧的时刻与中心亮度
struct VideoCollector
{
    std::mutex mutex;
    std::vector<int64_t> captureUs;
    std::vector<int> luma;
    std::vector<int> widths;
    std::atomic<int> eofs{0};
    std::atomic<size_t> framesAtEof{0};

    void Attach(capture::FileVideoSource& src)
    {
        src.SetEmitCallback([this](core::FramePtr f) {
            std::lock_guard<std::mutex> lock(mutex);
            captureUs.push_back(f->captureUs);
            luma.push_back(LumaOf(f, f->width / 2, f->height / 2));
            widths.push_back(f->width);
        });
        src.SetEofCallback([this] {
            std::lock_guard<std::mutex> lock(mutex);
            framesAtEof.store(luma.size());
            eofs.fetch_add(1);
        });
    }
    int64_t MaxGapUs()
    {
        int64_t g = 0;
        for (size_t i = 1; i < captureUs.size(); ++i) g = std::max(g, captureUs[i] - captureUs[i - 1]);
        return g;
    }
};

// 推流用的门面：纯视频，flv 落文件
void SetupVideoOnly(TGCapturer& c, const std::string& out)
{
    VideoEncoderParams vp;
    vp.width = 320;
    vp.height = 240;
    vp.fps = 30;
    vp.gop = 30;
    vp.vBitrate = 1500000;
    TG_ASSERT(c.SetupVideoEncoder(vp) == TGErr_OK);
    AudioEncoderParams ap;
    ap.enabled = false;
    TG_ASSERT(c.SetupAudioEncoder(ap) == TGErr_OK);
    MuxerParams mp;
    mp.muxer = "flv";
    mp.muxerOutput = out;
    TG_ASSERT(c.SetupMuxer(mp) == TGErr_OK);
}

struct Notifies
{
    std::mutex mutex;
    std::vector<std::string> eofs;
    int committed = 0;
    int64_t lastCommittedCount = -1;

    void Attach(TGCapturer& c)
    {
        c.SetOnNotifyCallback([this](NotifyMsgType t, const std::string& content, int64_t data) {
            std::lock_guard<std::mutex> lock(mutex);
            if (t == EsCap_Notify_LocalFile_EOF) {
                eofs.push_back(content);
            } else if (t == EsCap_Notify_Overlay_Committed) {
                ++committed;
                lastCommittedCount = data;
            }
        });
    }
};

// 预览回调：记录最新一帧在若干取样点的亮度
struct PreviewProbe
{
    std::mutex mutex;
    std::vector<std::pair<double, double>> points;
    std::vector<int> luma;
    std::atomic<int> frames{0};

    void Attach(TGCapturer& c, std::vector<std::pair<double, double>> pts)
    {
        points = std::move(pts);
        luma.assign(points.size(), -1);
        c.SetOnPreviewFrameCallback([this](const PreviewFrame& f) {
            if (f.format != PreviewPixelFormat::NV12 || !f.y) return;
            std::lock_guard<std::mutex> lock(mutex);
            for (size_t i = 0; i < points.size(); ++i) {
                const int x = std::clamp(static_cast<int>(points[i].first * f.width), 0, f.width - 1);
                const int y = std::clamp(static_cast<int>(points[i].second * f.height), 0, f.height - 1);
                luma[i] = f.y[static_cast<size_t>(y) * f.yStride + x];
            }
            frames.fetch_add(1);
        });
    }
    int At(size_t i)
    {
        std::lock_guard<std::mutex> lock(mutex);
        return luma[i];
    }
};

// 以 33ms 节拍推纯色外灌帧，直到 until（从 start 算起）
void PushUntil(TGCapturer& c, std::chrono::steady_clock::time_point start, std::chrono::milliseconds until,
               uint8_t y = 50)
{
    std::vector<uint8_t> nv12(320 * 240 * 3 / 2, 128);
    std::fill_n(nv12.begin(), 320 * 240, y);
    while (std::chrono::steady_clock::now() - start < until) {
        c.PushExternalVideoFrame(nv12.data(), 320, 240, 320, -1);
        std::this_thread::sleep_for(33ms);
    }
}

}  // namespace

// ================================================================== FileVideoSource
TG_TEST(FileVideo, ProbeRejectsBadFiles)
{
    std::string why;
    TG_EXPECT_EQ(capture::FileVideoSource::Probe(TmpPath("tg_no_such_video.mp4"), nullptr, &why),
                 int{TGErr_InvalidArgument});
    TG_EXPECT(!why.empty());
    const std::string junk = TmpPath("tg_junk_video.flv");
    {
        FILE* f = std::fopen(junk.c_str(), "wb");
        TG_ASSERT(f);
        std::fputs("definitely not a media file", f);
        std::fclose(f);
    }
    TG_EXPECT(capture::FileVideoSource::Probe(junk) != TGErr_OK);
    std::remove(junk.c_str());

    const std::string ok = TmpPath("tg_probe_video.flv");
    TG_ASSERT(tgmedia::WriteTestVideo(ok, 160, 96, 30, 15, [](int) { return 100; }));
    capture::FileVideoSource::Info info;
    TG_EXPECT_EQ(capture::FileVideoSource::Probe(ok, &info), int{TGErr_OK});
    TG_EXPECT_EQ(info.width, 160);
    TG_EXPECT_EQ(info.height, 96);
    std::remove(ok.c_str());
}

TG_TEST(FileVideo, RealtimePacingAndContent)
{
    const std::string path = TmpPath("tg_file_video_ramp.flv");
    TG_ASSERT(tgmedia::WriteTestVideo(path, 160, 96, 30, 30, [](int i) { return 30 + 6 * i; }));
    capture::FileVideoSource src(path, false);
    VideoCollector c;
    c.Attach(src);
    TG_ASSERT(src.Open(1280, 720, 30) == TGErr_OK);
    TG_EXPECT_EQ(src.FrameWidth(), 160);   // 按原生尺寸出帧，不理会请求尺寸
    TG_ASSERT(src.StartCapture());
    std::this_thread::sleep_for(700ms);
    src.StopCapture();
    std::lock_guard<std::mutex> lock(c.mutex);
    std::printf("    frames=%zu maxGap=%.1fms\n", c.luma.size(), c.MaxGapUs() / 1000.0);
    TG_EXPECT(c.luma.size() >= 18 && c.luma.size() <= 24);   // 0.7s × 30fps
    TG_EXPECT(c.MaxGapUs() < 45000);
    for (size_t i = 1; i < c.captureUs.size(); ++i) {
        const int64_t d = c.captureUs[i] - c.captureUs[i - 1];
        TG_EXPECT(d > 30000 && d < 37000);   // 时间戳按文件的 1/30s 推进
    }
    for (size_t i = 0; i < c.luma.size(); ++i) {
        TG_EXPECT(std::abs(c.luma[i] - (30 + 6 * static_cast<int>(i))) <= 4);   // 第 i 帧就是文件的第 i 帧
    }
    TG_EXPECT(std::all_of(c.widths.begin(), c.widths.end(), [](int w) { return w == 160; }));
    TG_EXPECT_EQ(c.eofs.load(), 0);
    std::remove(path.c_str());
}

TG_TEST(FileVideo, LoopIsSeamlessAndEofRepeatsLastFrame)
{
    const std::string path = TmpPath("tg_file_video_short.flv");
    TG_ASSERT(tgmedia::WriteTestVideo(path, 160, 96, 30, 10, [](int i) { return 40 + 15 * i; }));   // 0.33s
    // 循环：1.1s 至少循环 2 次，帧间隔不出现空洞，回绕后从第 0 帧开始
    {
        capture::FileVideoSource src(path, true);
        VideoCollector c;
        c.Attach(src);
        TG_ASSERT(src.Open(0, 0, 30) == TGErr_OK);
        TG_ASSERT(src.StartCapture());
        std::this_thread::sleep_for(1100ms);
        src.StopCapture();
        TG_EXPECT(src.Loops() >= 2u);
        TG_EXPECT(!src.Ended());
        std::lock_guard<std::mutex> lock(c.mutex);
        std::printf("    loop frames=%zu loops=%u maxGap=%.1fms\n", c.luma.size(), src.Loops(), c.MaxGapUs() / 1000.0);
        TG_EXPECT(c.luma.size() >= 30 && c.luma.size() <= 36);
        TG_EXPECT(c.MaxGapUs() < 45000);
        for (size_t i = 0; i < c.luma.size(); ++i) {
            TG_EXPECT(std::abs(c.luma[i] - (40 + 15 * static_cast<int>(i % 10))) <= 5);
        }
    }
    // 不循环：EOF 一次，之后按帧率重复最后一帧
    {
        capture::FileVideoSource src(path, false);
        VideoCollector c;
        c.Attach(src);
        TG_ASSERT(src.Open(0, 0, 30) == TGErr_OK);
        TG_ASSERT(src.StartCapture());
        std::this_thread::sleep_for(800ms);
        src.StopCapture();
        TG_EXPECT(src.Ended());
        TG_EXPECT_EQ(c.eofs.load(), 1);
        std::lock_guard<std::mutex> lock(c.mutex);
        std::printf("    eof frames=%zu (at eof %zu)\n", c.luma.size(), c.framesAtEof.load());
        TG_EXPECT_EQ(c.framesAtEof.load(), size_t{10});
        TG_EXPECT(c.luma.size() >= 21);   // EOF 后仍在出帧
        TG_EXPECT(c.MaxGapUs() < 45000);
        for (size_t i = 10; i < c.luma.size(); ++i) {
            TG_EXPECT(std::abs(c.luma[i] - 175) <= 5);
        }
    }
    // Close 之后重新开始采集：采集线程里重新打开，从头播放
    {
        capture::FileVideoSource src(path, false);
        VideoCollector c;
        c.Attach(src);
        TG_ASSERT(src.Open(0, 0, 30) == TGErr_OK);
        src.Close();
        TG_ASSERT(src.StartCapture());
        std::this_thread::sleep_for(150ms);
        src.StopCapture();
        std::lock_guard<std::mutex> lock(c.mutex);
        TG_ASSERT(!c.luma.empty());
        TG_EXPECT(std::abs(c.luma[0] - 40) <= 5);
    }
    std::remove(path.c_str());
}

// ================================================================== 合成器
TG_TEST(Overlay, ValidateAndResolveRect)
{
    OverlayDesc d;
    d.id = "logo";
    d.path = "x.png";
    std::string why;
    TG_EXPECT(core::ValidateOverlayDesc(d, &why));
    auto bad = [&](auto mutate) {
        OverlayDesc b = d;
        mutate(b);
        return !core::ValidateOverlayDesc(b, nullptr);
    };
    TG_EXPECT(bad([](OverlayDesc& b) { b.id.clear(); }));
    TG_EXPECT(bad([](OverlayDesc& b) { b.id = "a b"; }));
    TG_EXPECT(bad([](OverlayDesc& b) { b.path.clear(); }));
    TG_EXPECT(bad([](OverlayDesc& b) { b.width = 0; }));
    TG_EXPECT(bad([](OverlayDesc& b) { b.height = -0.1f; }));
    TG_EXPECT(bad([](OverlayDesc& b) { b.x = 1.5f; }));
    TG_EXPECT(bad([](OverlayDesc& b) { b.opacity = 1.5f; }));
    TG_EXPECT(bad([](OverlayDesc& b) { b.opacity = std::nanf(""); }));

    d.x = 0.5f;
    d.y = 0.25f;
    d.width = 0.25f;
    d.height = 0.0f;
    const auto r = core::ResolveOverlayRect(d, 100, 50, 320, 240);   // 按素材宽高比
    TG_EXPECT_EQ(r.x, 160);
    TG_EXPECT_EQ(r.y, 60);
    TG_EXPECT_EQ(r.w, 80);
    TG_EXPECT_EQ(r.h, 40);
    d.height = 0.5f;
    TG_EXPECT_EQ(core::ResolveOverlayRect(d, 100, 50, 320, 240).h, 120);
}

TG_TEST(Overlay, CompositorProgramAndPreviewScenes)
{
    CompositorRig rig;
    auto white = SolidImage(32, 32, 235);
    // 暂存（只进预览）：推流画面不变，预览画在自己的一份像素上
    auto staged = Scene({ImageLayer("w", white, 0.5f, 0.0f, 0.25f, 0.25f)});   // x 160..240, y 0..60
    rig.comp->SetOverlayScenes(Scene({}), staged);
    rig.Feed();
    auto prog = rig.program->Last();
    auto prev = rig.preview->Last();
    TG_ASSERT(prog && prev);
    TG_EXPECT_EQ(LumaOf(prog, 200, 30), 50);
    TG_EXPECT_EQ(LumaOf(prev, 200, 30), 235);
    TG_EXPECT_EQ(LumaOf(prev, 10, 10), 50);
    TG_EXPECT_EQ(LumaOf(prev, 200, 70), 50);
    TG_EXPECT(prog->planes[0].get() != prev->planes[0].get());

    // 提交后两边同一个场景：预览共享推流画面的像素
    rig.comp->SetOverlayScenes(staged, staged);
    rig.Feed();
    prog = rig.program->Last();
    prev = rig.preview->Last();
    TG_EXPECT_EQ(LumaOf(prog, 200, 30), 235);
    TG_EXPECT(prog->planes[0].get() == prev->planes[0].get());

    // 透明度
    rig.comp->SetOverlayScenes(Scene({ImageLayer("w", white, 0.5f, 0.0f, 0.25f, 0.25f, 0.5f)}), nullptr);
    rig.Feed();
    TG_EXPECT(std::abs(LumaOf(rig.program->Last(), 200, 30) - 142) <= 2);

    // alpha：左半透明、右半不透明
    auto halfAlpha = SolidImage(32, 32, 235, true);
    rig.comp->SetOverlayScenes(Scene({ImageLayer("a", halfAlpha, 0.5f, 0.0f, 0.25f, 0.25f)}), nullptr);
    rig.Feed();
    TG_EXPECT_EQ(LumaOf(rig.program->Last(), 170, 30), 50);
    TG_EXPECT_EQ(LumaOf(rig.program->Last(), 230, 30), 235);

    // 层级：zOrder 大的在上
    auto gray = SolidImage(16, 16, 100);
    rig.comp->SetOverlayScenes(Scene({ImageLayer("top", gray, 0.5f, 0.0f, 0.25f, 0.25f, 1.0f, 1),
                                      ImageLayer("bottom", white, 0.5f, 0.0f, 0.25f, 0.25f, 1.0f, 0)}),
                               nullptr);
    // 场景里的顺序由门面排好（下层在前）；这里故意倒序给，验证合成器按给定顺序画：后画的在上
    rig.Feed();
    TG_EXPECT_EQ(LumaOf(rig.program->Last(), 200, 30), 235);

    // 部分移出画面：裁掉越界部分
    rig.comp->SetOverlayScenes(Scene({ImageLayer("edge", white, 0.9f, -0.1f, 0.25f, 0.25f)}), nullptr);
    rig.Feed();
    TG_EXPECT_EQ(LumaOf(rig.program->Last(), 319, 10), 235);
    TG_EXPECT_EQ(LumaOf(rig.program->Last(), 300, 50), 50);   // 上移 24 像素后只剩 y 0..36

    // 画布尺寸变化：按比例跟随
    rig.comp->SetOverlayScenes(staged, staged);
    rig.Feed(640, 480);
    TG_EXPECT_EQ(LumaOf(rig.program->Last(), 330, 10), 235);
    TG_EXPECT_EQ(LumaOf(rig.program->Last(), 330, 130), 50);
}

TG_TEST(Overlay, CompositorVideoInput)
{
    CompositorRig rig;
    core::OverlayLayer l;
    l.desc.id = "pip";
    l.desc.kind = OverlayKind::VideoFile;
    l.desc.path = "mem";
    l.desc.width = 0.5f;   // 160 宽，高按 160x96 的宽高比 = 96
    l.srcWidth = 160;
    l.srcHeight = 96;
    l.inputPort = 1;
    auto scene = Scene({l});
    rig.comp->SetOverlayScenes(scene, scene);
    rig.Feed();
    TG_EXPECT_EQ(LumaOf(rig.program->Last(), 10, 10), 50);   // 还没收到视频帧：不画

    TG_EXPECT(rig.comp->SubmitInput(1, MakeNv12(160, 96, 200)));
    rig.Feed();
    TG_EXPECT_EQ(LumaOf(rig.program->Last(), 10, 10), 200);
    TG_EXPECT_EQ(LumaOf(rig.program->Last(), 150, 90), 200);
    TG_EXPECT_EQ(LumaOf(rig.program->Last(), 10, 100), 50);
    TG_EXPECT_EQ(rig.comp->OverlayFramesOf(1), uint64_t{1});

    TG_EXPECT(rig.comp->SubmitInput(1, MakeNv12(160, 96, 120)));   // 只保留最新一帧
    rig.Feed();
    TG_EXPECT_EQ(LumaOf(rig.program->Last(), 10, 10), 120);

    // 停止后残帧作废
    rig.comp->StopNode();
    rig.comp->StartNode();
    rig.Feed();
    TG_EXPECT_EQ(LumaOf(rig.program->Last(), 10, 10), 50);
}

// ================================================================== 门面
TG_TEST(Overlay, FacadeValidation)
{
    TGCapturer c(GameID::GAME_ID_DEFAULT);
    Notifies n;
    n.Attach(c);
    OverlayDesc d;
    d.id = "logo";
    d.path = TmpPath("tg_no_such_logo.png");
    TG_EXPECT_EQ(c.StageOverlay(d), int{TGErr_InvalidArgument});   // 文件不存在：同步报错
    d.id = "bad id";
    TG_EXPECT_EQ(c.StageOverlay(d), int{TGErr_InvalidArgument});
    d.id = "logo";
    d.kind = OverlayKind::VideoFile;
    TG_EXPECT_EQ(c.StageOverlay(d), int{TGErr_InvalidArgument});
    TG_EXPECT_EQ(c.StageRemoveOverlay("nope"), int{TGErr_InvalidArgument});
    TG_EXPECT_EQ(c.CommitOverlays(), int{TGErr_OK});   // 没有改动：直接成功，不发通知

    const std::string png = TmpPath("tg_overlay_logo.png");
    TG_ASSERT(tgmedia::WriteSolidPng(png, 16, 16, 255, 255, 255));
    d.kind = OverlayKind::Image;
    d.path = png;
    TG_EXPECT_EQ(c.StageOverlay(d), int{TGErr_OK});
    std::vector<OverlayDesc> list;
    TG_EXPECT_EQ(c.GetOverlays(list, true), int{TGErr_OK});
    TG_EXPECT_EQ(list.size(), size_t{1});
    TG_EXPECT_EQ(c.GetOverlays(list, false), int{TGErr_OK});
    TG_EXPECT(list.empty());
    TG_EXPECT_EQ(c.DiscardOverlayChanges(), int{TGErr_OK});
    c.GetOverlays(list, true);
    TG_EXPECT(list.empty());

    // 提交后可读回；按 zOrder 排序
    d.zOrder = 5;
    TG_EXPECT_EQ(c.StageOverlay(d), int{TGErr_OK});
    OverlayDesc d2 = d;
    d2.id = "under";
    d2.zOrder = -1;
    TG_EXPECT_EQ(c.StageOverlay(d2), int{TGErr_OK});
    TG_EXPECT_EQ(c.CommitOverlays(), int{TGErr_OK});
    c.GetOverlays(list, false);
    TG_ASSERT(list.size() == 2u);
    TG_EXPECT_EQ(list[0].id, std::string("under"));
    TG_EXPECT_EQ(list[1].id, std::string("logo"));
    std::this_thread::sleep_for(50ms);   // 通知是异步投递的
    {
        std::lock_guard<std::mutex> lock(n.mutex);
        TG_EXPECT_EQ(n.committed, 1);
        TG_EXPECT_EQ(n.lastCommittedCount, int64_t{2});
    }
    std::remove(png.c_str());
}

TG_TEST(Overlay, StagedInPreviewCommittedAtomicallyInProgram)
{
    const std::string png = TmpPath("tg_overlay_white.png");
    const std::string out = TmpPath("tg_overlay_out.flv");
    TG_ASSERT(tgmedia::WriteSolidPng(png, 32, 32, 255, 255, 255));
    Notifies n;
    bool previewSawStaged = false;
    {
        TGCapturer c(GameID::GAME_ID_DEFAULT);
        n.Attach(c);
        PreviewProbe probe;
        probe.Attach(c, {{0.12, 0.12}, {0.88, 0.88}});
        SetupVideoOnly(c, out);
        TG_ASSERT(c.StartPreview() == TGErr_OK);
        TG_ASSERT(c.Prepare() == TGErr_OK);
        TG_ASSERT(c.Start() == TGErr_OK);
        const auto start = std::chrono::steady_clock::now();
        PushUntil(c, start, 500ms);
        // 同时暂存两层（左上 / 右下），先不提交
        OverlayDesc a;
        a.id = "a";
        a.path = png;
        a.width = 0.25f;
        a.height = 0.25f;
        OverlayDesc b = a;
        b.id = "b";
        b.x = 0.75f;
        b.y = 0.75f;
        TG_ASSERT(c.StageOverlay(a) == TGErr_OK);
        TG_ASSERT(c.StageOverlay(b) == TGErr_OK);
        PushUntil(c, start, 900ms);
        previewSawStaged = probe.At(0) > 200 && probe.At(1) > 200;
        TG_ASSERT(c.CommitOverlays() == TGErr_OK);
        PushUntil(c, start, 1500ms);
        // 同时去掉两层
        TG_ASSERT(c.StageRemoveOverlay("a") == TGErr_OK);
        TG_ASSERT(c.StageRemoveOverlay("b") == TGErr_OK);
        PushUntil(c, start, 1700ms);
        TG_EXPECT(probe.At(0) < 80);   // 预览先看到移除
        TG_ASSERT(c.CommitOverlays() == TGErr_OK);
        PushUntil(c, start, 2300ms);
        c.Stop();
        c.StopPreview();
    }
    TG_EXPECT(previewSawStaged);
    {
        std::lock_guard<std::mutex> lock(n.mutex);
        TG_EXPECT_EQ(n.committed, 2);
    }
    const auto frames = tgmedia::DecodeVideo(out);
    TG_ASSERT(frames.size() > 40u);
    int mixed = 0, before = 0, with = 0, after = 0, segments = 0;
    bool prev = false;
    for (const auto& f : frames) {
        const bool fa = f.LumaAt(0.12, 0.12) > 150;
        const bool fb = f.LumaAt(0.88, 0.88) > 150;
        mixed += fa != fb;
        if (fa && !prev) ++segments;
        prev = fa;
        (fa ? with : (with == 0 ? before : after))++;
    }
    std::printf("    frames=%zu before=%d with=%d after=%d mixed=%d segments=%d\n", frames.size(), before, with, after,
                mixed, segments);
    TG_EXPECT_EQ(mixed, 0);         // 原子：两层总在同一帧里一起出现 / 消失
    TG_EXPECT_EQ(segments, 1);      // 只在提交与移除提交之间出现
    TG_EXPECT(before >= 20);        // 暂存期间（0.5 ~ 0.9s）推流画面没有它
    TG_EXPECT(with >= 12 && with <= 24);
    TG_EXPECT(after >= 10);
    std::remove(png.c_str());
    std::remove(out.c_str());
}

TG_TEST(Overlay, VideoLayerPlaysAndKeepsContentOnGeometryChange)
{
    const std::string media = TmpPath("tg_overlay_pip.flv");
    const std::string out = TmpPath("tg_overlay_pip_out.flv");
    TG_ASSERT(tgmedia::WriteTestVideo(media, 160, 96, 30, 30, [](int i) { return 100 + 4 * i; }));   // 1s
    Notifies n;
    int previewPip = -1;
    {
        TGCapturer c(GameID::GAME_ID_DEFAULT);
        n.Attach(c);
        PreviewProbe probe;
        probe.Attach(c, {{0.75, 0.7}});
        SetupVideoOnly(c, out);
        TG_ASSERT(c.StartPreview() == TGErr_OK);
        TG_ASSERT(c.Prepare() == TGErr_OK);
        TG_ASSERT(c.Start() == TGErr_OK);
        const auto start = std::chrono::steady_clock::now();
        PushUntil(c, start, 300ms);
        OverlayDesc pip;
        pip.id = "pip";
        pip.kind = OverlayKind::VideoFile;
        pip.path = media;
        pip.loop = false;
        pip.x = 0.5f;
        pip.y = 0.5f;
        pip.width = 0.5f;   // 160x96 → x 160..320，y 120..216
        TG_ASSERT(c.StageOverlay(pip) == TGErr_OK);
        PushUntil(c, start, 650ms);
        previewPip = probe.At(0);
        TG_ASSERT(c.CommitOverlays() == TGErr_OK);
        PushUntil(c, start, 1600ms);   // 视频层 1s，在 ~1.3s 播完
        // 只挪位置：沿用内容（已播完、停在最后一帧），不会从头再播
        pip.x = 0.0f;
        TG_ASSERT(c.StageOverlay(pip) == TGErr_OK);
        TG_ASSERT(c.CommitOverlays() == TGErr_OK);
        PushUntil(c, start, 2000ms);
        c.Stop();
        c.StopPreview();
    }
    std::printf("    preview pip luma before commit=%d\n", previewPip);
    TG_EXPECT(previewPip > 90);   // 暂存时预览里就在播
    {
        std::lock_guard<std::mutex> lock(n.mutex);
        TG_ASSERT(n.eofs.size() == 1u);
        TG_EXPECT_EQ(n.eofs[0], std::string("overlay:pip"));
    }
    const auto frames = tgmedia::DecodeVideo(out);
    TG_ASSERT(frames.size() > 40u);
    const int64_t t0 = frames.front().ptsMs;
    int64_t firstPip = -1;
    std::vector<double> pipLuma;
    for (const auto& f : frames) {
        const double l = f.LumaAt(0.75, 0.7);
        if (l > 90) {
            if (firstPip < 0) firstPip = f.ptsMs - t0;
            pipLuma.push_back(l);
        }
    }
    std::printf("    first pip frame at %lldms, pip frames=%zu, last=%.0f/%.0f\n", static_cast<long long>(firstPip),
                pipLuma.size(), frames.back().LumaAt(0.25, 0.7), frames.back().LumaAt(0.75, 0.7));
    TG_EXPECT(firstPip >= 500);   // 推流画面要等提交（~0.65s）才有画中画
    TG_ASSERT(pipLuma.size() > 10u);
    TG_EXPECT(pipLuma.back() > pipLuma.front() + 20);   // 在播：亮度随帧上升
    TG_EXPECT(frames.back().LumaAt(0.25, 0.7) > 180);   // 挪到左边，仍是最后一帧（~216）
    TG_EXPECT(frames.back().LumaAt(0.75, 0.7) < 70);
    std::remove(media.c_str());
    std::remove(out.c_str());
}

TG_TEST(FileVideo, MainSourceFromFileAndEof)
{
    const std::string media = TmpPath("tg_main_file.flv");
    const std::string out = TmpPath("tg_main_file_out.flv");
    TG_ASSERT(tgmedia::WriteTestVideo(media, 160, 96, 30, 15, [](int i) { return 60 + 8 * i; }));   // 0.5s
    Notifies n;
    {
        TGCapturer c(GameID::GAME_ID_DEFAULT);
        n.Attach(c);
        TG_EXPECT(c.CreateLocalFileCapture(0, TmpPath("tg_no_such_main.flv"), false) == nullptr);
        TG_EXPECT(c.CreateLocalFileCapture(0, "", false) == nullptr);
        IVideoSource* src = c.CreateLocalFileCapture(0, media, false);
        TG_ASSERT(src != nullptr);
        TG_EXPECT_EQ(static_cast<int>(src->GetVideoCapSourceType()), int{TypeOfLocalFile});
        SetupVideoOnly(c, out);
        TG_ASSERT(c.Prepare() == TGErr_OK);
        TG_ASSERT(c.Start() == TGErr_OK);
        std::this_thread::sleep_for(1100ms);
        c.Stop();
    }
    {
        std::lock_guard<std::mutex> lock(n.mutex);
        TG_ASSERT(n.eofs.size() == 1u);
        TG_EXPECT_EQ(n.eofs[0], std::string("video:main"));
    }
    const auto frames = tgmedia::DecodeVideo(out);
    std::printf("    frames=%zu first=%.0f last=%.0f\n", frames.size(), frames.empty() ? 0.0 : frames.front().LumaAt(0.5, 0.5),
                frames.empty() ? 0.0 : frames.back().LumaAt(0.5, 0.5));
    TG_ASSERT(frames.size() >= 25u);   // 播完后仍按帧率出帧
    TG_EXPECT_EQ(frames.front().width, 320);   // 编码器按编码分辨率缩放
    TG_EXPECT(frames.front().LumaAt(0.5, 0.5) < 90);
    TG_EXPECT(std::abs(frames.back().LumaAt(0.5, 0.5) - 172) < 8);
    std::remove(media.c_str());
    std::remove(out.c_str());
}
