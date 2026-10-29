#include "nodes/VideoNodes.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>

#include <libyuv.h>

#include "core/AVSyncClock.h"
#include "core/Clock.h"
#include "core/Logger.h"
#include "core/PauseFrame.h"
#include "core/TextOverlay.h"
#include "core/Watermark.h"

namespace TGCapture::nodes
{

// ================================================================== VideoSourceNode
VideoSourceNode::VideoSourceNode(std::string name) : core::BusNode(std::move(name)) {}

VideoSourceNode::~VideoSourceNode() = default;

void VideoSourceNode::SetSource(capture::VideoSourceBase* source)
{
    if (m_source == source) {
        return;
    }
    if (m_source) {
        m_source->SetEmitCallback(nullptr);
    }
    m_source = source;
    if (m_source) {
        m_source->SetEmitCallback([this](core::FramePtr frame) {
            if (frame) {
                m_emitted.fetch_add(1, std::memory_order_relaxed);
                Emit(0, std::move(frame));
            }
        });
    }
}

bool VideoSourceNode::NodeStart()
{
    if (m_source) {
        m_source->StartCapture();
    }
    return true;
}

void VideoSourceNode::NodeStop()
{
    // StopCapture 是真正的回调栅栏（关闸 + join + 等在飞回调），返回后下游拆除时不会再有采集线程闯进来
    if (m_source) {
        m_source->StopCapture();
    }
}

void VideoSourceNode::NodeClose()
{
    // 设备资源（句柄 / COM 对象 / D3D 纹理 / GDI 位图）在采集线程 join 之后释放
    if (m_source) {
        m_source->Close();
    }
}

bool VideoSourceNode::Push(core::FramePtr frame)
{
    if (!frame) {
        return false;
    }
    m_emitted.fetch_add(1, std::memory_order_relaxed);
    return Emit(0, std::move(frame));
}

std::string VideoSourceNode::DumpState() const
{
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%s emitted=%llu", m_source ? "native+external" : "external",
                  static_cast<unsigned long long>(m_emitted.load()));
    return buf;
}

// ================================================================== VideoCompositorNode
VideoCompositorNode::VideoCompositorNode() : core::BusNode("video_compositor") {}

void VideoCompositorNode::SetPauseLayer(std::shared_ptr<core::PauseFrame> pause)
{
    std::lock_guard<std::mutex> lock(m_layerMutex);
    m_pause = std::move(pause);
}

void VideoCompositorNode::SetWatermarkLayer(std::shared_ptr<core::Watermark> watermark)
{
    std::lock_guard<std::mutex> lock(m_layerMutex);
    m_watermark = std::move(watermark);
}

void VideoCompositorNode::SetTimestampLayer(std::shared_ptr<core::TimestampOverlay> timestamp)
{
    std::lock_guard<std::mutex> lock(m_layerMutex);
    m_timestamp = std::move(timestamp);
}

void VideoCompositorNode::SetOverlayScenes(core::OverlayScenePtr program, core::OverlayScenePtr preview)
{
    std::lock_guard<std::mutex> lock(m_layerMutex);
    m_programScene = std::move(program);
    m_previewScene = std::move(preview);
}

core::OverlayScenePtr VideoCompositorNode::ProgramScene() const
{
    std::lock_guard<std::mutex> lock(m_layerMutex);
    return m_programScene;
}

core::OverlayScenePtr VideoCompositorNode::PreviewScene() const
{
    std::lock_guard<std::mutex> lock(m_layerMutex);
    return m_previewScene;
}

uint64_t VideoCompositorNode::OverlayFramesOf(size_t port) const
{
    std::lock_guard<std::mutex> lock(m_inputMutex);
    const auto it = m_inputs.find(port);
    return it == m_inputs.end() ? 0 : it->second.frames;
}

void VideoCompositorNode::OnInputDetached(size_t port, const std::string& /*from*/)
{
    if (port != kMainInput) {
        std::lock_guard<std::mutex> lock(m_inputMutex);
        m_inputs.erase(port);
    }
}

bool VideoCompositorNode::NodeStart()
{
    m_running.store(true);
    return true;
}

void VideoCompositorNode::NodeStop()
{
    m_running.store(false);
    // 叠加输入的最新帧作废：下次开始时视频层等新帧到了再画，不会先闪一下上一轮的残帧
    std::lock_guard<std::mutex> lock(m_inputMutex);
    for (auto& [_, in] : m_inputs) {
        in.frame.reset();
    }
}

const VideoCompositorNode::ScaledLayer* VideoCompositorNode::Scaled(const core::OverlayLayer& layer, int w, int h)
{
    // 素材：图片层是预解码的图；视频层是该输入口的最新一帧（还没收到帧就不画）
    std::shared_ptr<const void> source;
    core::Nv12AView src;
    if (layer.desc.kind == OverlayKind::Image) {
        if (!layer.image || layer.image->width < 2) {
            return nullptr;
        }
        const auto& img = *layer.image;
        source = layer.image;
        src = {img.y.data(), img.width, img.uv.data(), img.width, img.a.empty() ? nullptr : img.a.data(), img.width,
               img.width, img.height};
    } else {
        core::FramePtr f;
        {
            std::lock_guard<std::mutex> lock(m_inputMutex);
            const auto it = m_inputs.find(layer.inputPort);
            if (it != m_inputs.end()) {
                f = it->second.frame;
            }
        }
        if (!f || !f->planes[0] || !f->planes[1] || f->pixFmt != AV_PIX_FMT_NV12 || f->width < 2 || f->height < 2) {
            return nullptr;
        }
        source = f;
        src = {f->planes[0].get(), f->strides[0] > 0 ? f->strides[0] : f->width, f->planes[1].get(),
               f->strides[1] > 0 ? f->strides[1] : f->width, nullptr, 0, f->width & ~1, f->height & ~1};
    }
    auto& cache = m_scaled[layer.desc.id + "@" + std::to_string(w) + "x" + std::to_string(h)];
    cache.lastUse = m_renderTick;
    if (cache.source == source && cache.width == w && cache.height == h) {
        return &cache;
    }
    cache.source = source;
    cache.width = w;
    cache.height = h;
    cache.y.resize(static_cast<size_t>(w) * h);
    cache.uv.resize(static_cast<size_t>(w) * h / 2);
    libyuv::NV12Scale(src.y, src.yStride, src.uv, src.uvStride, src.width, src.height, cache.y.data(), w,
                      cache.uv.data(), w, w, h, libyuv::kFilterBilinear);
    if (src.a) {
        cache.a.resize(static_cast<size_t>(w) * h);
        libyuv::ScalePlane(src.a, src.aStride, src.width, src.height, cache.a.data(), w, w, h,
                           libyuv::kFilterBilinear);
    } else {
        cache.a.clear();
    }
    return &cache;
}

void VideoCompositorNode::ComposeOverlays(core::Frame& frame, const core::OverlayScene& scene)
{
    for (const auto& layer : scene.layers) {
        const core::PixelRect r =
            core::ResolveOverlayRect(layer.desc, layer.srcWidth, layer.srcHeight, frame.width, frame.height);
        if (r.w < 2 || r.h < 2 || r.w > 8192 || r.h > 8192) {
            continue;
        }
        const ScaledLayer* s = Scaled(layer, r.w, r.h);
        if (!s) {
            continue;
        }
        const core::Nv12AView view{s->y.data(), s->width, s->uv.data(), s->width,
                                   s->a.empty() ? nullptr : s->a.data(), s->width, s->width, s->height};
        core::BlendNv12A(frame, r.x, r.y, view, static_cast<int>(std::lround(layer.desc.opacity * 255.0f)));
    }
}

namespace
{
// 深拷贝一帧 NV12（预览场景与推流场景不同时，预览要画在自己的一份像素上）
core::FramePtr CloneNv12(const core::Frame& src)
{
    const int w = src.width;
    const int h = src.height;
    const size_t ySize = static_cast<size_t>(w) * h;
    auto buffer = std::shared_ptr<uint8_t>(new uint8_t[ySize + ySize / 2], std::default_delete<uint8_t[]>());
    const int sY = src.strides[0] > 0 ? src.strides[0] : w;
    const int sUV = src.strides[1] > 0 ? src.strides[1] : w;
    for (int row = 0; row < h; ++row) {
        std::memcpy(buffer.get() + static_cast<size_t>(row) * w, src.planes[0].get() + static_cast<size_t>(row) * sY,
                    static_cast<size_t>(w));
    }
    for (int row = 0; row < h / 2; ++row) {
        std::memcpy(buffer.get() + ySize + static_cast<size_t>(row) * w,
                    src.planes[1].get() + static_cast<size_t>(row) * sUV, static_cast<size_t>(w));
    }
    auto out = std::make_shared<core::Frame>(src);
    for (auto& p : out->planes) {
        p.reset();
    }
    out->planes[0] = buffer;
    out->strides[0] = w;
    out->planes[1] = std::shared_ptr<uint8_t>(buffer, buffer.get() + ySize);
    out->strides[1] = w;
    return out;
}
}  // namespace

bool VideoCompositorNode::SubmitInput(size_t port, core::FramePtr frame)
{
    if (!frame || !m_running.load()) {
        return false;
    }
    if (port != kMainInput) {
        // 视频叠加层：只记下最新一帧，主画面到达时再画（叠加层的帧率不影响输出帧率）
        std::lock_guard<std::mutex> lock(m_inputMutex);
        auto& in = m_inputs[port];
        in.frame = std::move(frame);
        ++in.frames;
        return true;
    }
    // 看护判定的是"采集端还在不在产出"，而不是"这一帧最终有没有被编码"：
    // 下面的 Mute 丢帧、硬件帧无法改写等分支都会提前返回，但这些都不是采集故障，打点必须在它们之前
    if (m_onMain) {
        m_onMain(core::MonotonicClock::NowUs());
    }

    std::shared_ptr<core::PauseFrame> pause;
    std::shared_ptr<core::Watermark> watermark;
    std::shared_ptr<core::TimestampOverlay> timestamp;
    core::OverlayScenePtr program;
    core::OverlayScenePtr preview;
    {
        std::lock_guard<std::mutex> lock(m_layerMutex);
        pause = m_pause;
        watermark = m_watermark;
        timestamp = m_timestamp;
        program = m_programScene;
        preview = m_previewScene;
    }

    // ---- 暂停图层 ----
    if (m_muted.load()) {
        // 对齐 MediaX：MuteVideo 期间若设置了 pause 图片，用它替换像素但保留 PTS 继续推进；
        // 没设置图片就"冻结最后一帧"—— 当前 frame 本身即是最新一帧，拿去编码等效于冻结。
        // 没有 pauseFrame 且没有有效像素时才真丢弃（硬件直通路径下无法替换）。
        if (pause && pause->Valid() && frame->planes[0]) {
            if (!pause->ApplyTo(*frame)) {
                return false;
            }
        } else if (!frame->planes[0]) {
            return false;   // 硬件帧 + 无 pause 图片：无法替换，只能丢
        }
    }

    const bool cpuNv12 = frame->planes[0] && frame->planes[1] && frame->pixFmt == AV_PIX_FMT_NV12;
    const bool hasProgram = program && !program->layers.empty();
    const bool hasPreview = preview && !preview->layers.empty();
    const bool wantPreview = HasDownstream(kPreviewOutput);
    // 预览场景与推流场景不同（有未提交的修改）：预览画在自己的一份像素上
    core::FramePtr previewFrame;
    if (wantPreview && cpuNv12 && preview != program && (hasPreview || hasProgram)) {
        previewFrame = CloneNv12(*frame);
    }

    // ---- 叠加层：仅对 CPU 帧生效 ----
    if (cpuNv12 && (hasProgram || (previewFrame && hasPreview))) {
        std::lock_guard<std::mutex> lock(m_renderMutex);
        ++m_renderTick;
        if (hasProgram) {
            ComposeOverlays(*frame, *program);
        }
        if (previewFrame && hasPreview) {
            ComposeOverlays(*previewFrame, *preview);
        }
        // 缓存里 2s 没用过的（层已移除 / 尺寸已变）丢掉
        if (m_renderTick % 64 == 0) {
            for (auto it = m_scaled.begin(); it != m_scaled.end();) {
                it = m_renderTick - it->second.lastUse > 64 ? m_scaled.erase(it) : std::next(it);
            }
        }
    } else if (!cpuNv12 && frame->hwHandle && (hasProgram || hasPreview)) {
        TG_LOGW_THROTTLED(3000000, "TGCapturer: 硬件帧路径暂不支持叠加层，本帧叠加层已跳过");
    }

    // ---- 水印图层（最上层）：仅对 CPU 帧生效；硬件直通帧跳过并限频提示 ----
    if (watermark && !watermark->Empty()) {
        if (cpuNv12) {
            watermark->BlendOnto(*frame);
            if (previewFrame) {
                watermark->BlendOnto(*previewFrame);
            }
        } else if (frame->hwHandle) {
            TG_LOGW_THROTTLED(3000000, "TGCapturer: 硬件帧路径暂不支持水印，本帧水印已跳过");
        }
    }

    // ---- 时间水印（最上层）：时间取本帧的采集时刻（单调时钟）换算成墙钟，与编码排队无关 ----
    if (timestamp && timestamp->Enabled()) {
        if (cpuNv12) {
            const int64_t nowMono = core::MonotonicClock::NowUs();
            const int64_t age = frame->captureUs != AV_NOPTS_VALUE ? nowMono - frame->captureUs : 0;
            // 外灌帧的时间戳可能不在内核时钟域：偏差超过 5s 就按"现在"算
            const int64_t wallUs = core::WallClockNowUs() - (age >= 0 && age < 5000000 ? age : 0);
            timestamp->BlendOnto(*frame, wallUs);
            if (previewFrame) {
                timestamp->BlendOnto(*previewFrame, wallUs);
            }
        } else if (frame->hwHandle) {
            TG_LOGW_THROTTLED(3000000, "TGCapturer: 硬件帧路径暂不支持时间水印，本帧已跳过");
        }
    }

    m_composed.fetch_add(1, std::memory_order_relaxed);
    // 预览与推流共享像素时（场景相同），下游不得修改像素。预览给一份浅拷贝的帧头：
    // 推流链路会改写 ptsUs 等元数据，而预览在另一个线程上读，共用帧头会产生数据竞争
    if (wantPreview) {
        Emit(kPreviewOutput, previewFrame ? std::move(previewFrame) : std::make_shared<core::Frame>(*frame));
    }
    return Emit(kProgramOutput, std::move(frame));
}

std::string VideoCompositorNode::DumpState() const
{
    bool pauseValid = false;
    bool watermark = false;
    bool timestamp = false;
    size_t programLayers = 0;
    uint64_t programVersion = 0;
    bool previewDiffers = false;
    {
        std::lock_guard<std::mutex> lock(m_layerMutex);
        pauseValid = m_pause && m_pause->Valid();
        watermark = m_watermark && !m_watermark->Empty();
        timestamp = m_timestamp && m_timestamp->Enabled();
        if (m_programScene) {
            programLayers = m_programScene->layers.size();
            programVersion = m_programScene->version;
        }
        previewDiffers = m_previewScene != m_programScene;
    }
    char buf[208];
    std::snprintf(buf, sizeof(buf), "layers=[main%s,overlays=%zu(v%llu%s)%s%s] muted=%d composed=%llu",
                  pauseValid ? ",pause" : "", programLayers, static_cast<unsigned long long>(programVersion),
                  previewDiffers ? ",staged" : "", watermark ? ",watermark" : "", timestamp ? ",timestamp" : "",
                  m_muted.load() ? 1 : 0, static_cast<unsigned long long>(m_composed.load()));
    return buf;
}

// ================================================================== VideoSyncNode
VideoSyncNode::VideoSyncNode() : core::BusNode("video_sync") {}

void VideoSyncNode::SetConfig(const Config& config)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_config = config;
}

void VideoSyncNode::Reset()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_syncDropped.store(0);
    m_bpDropped.store(0);
    m_audioWaitStartUs = 0;
    m_lastVideoPtsUs = -1;
    m_lastVideoCaptureUs = -1;
    // 首帧标志必须复位，否则第二轮不再触发 EsCap_Notify_Fisrt_Frame_OK
    m_firstFrameSent.store(false);
}

bool VideoSyncNode::NodeStart()
{
    m_running.store(true);
    return true;
}

void VideoSyncNode::NodeStop()
{
    m_running.store(false);
}

bool VideoSyncNode::Submit(core::FramePtr frame)
{
    if (!frame || !m_running.load()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);

    // 背压：编码器积压过多则丢帧，保证直播实时性
    const size_t encoderPending = DownstreamPending(0);
    if (encoderPending >= m_config.maxEncoderPending) {
        const auto n = m_bpDropped.fetch_add(1) + 1;
        TG_LOGW_THROTTLED(2000000, "TGCapturer: 编码器积压 %zu，丢帧（编码跟不上采集，bpDrop=%llu）",
                          encoderPending, static_cast<unsigned long long>(n));
        return false;
    }

    // 调用方自带时间轴：直接用它给的 PTS，不再走内核的同步与时间轴生成
    if (frame->ptsLocked && frame->ptsUs >= 0) {
        return Deliver(std::move(frame), /*externalTimeline=*/true);
    }

    auto ctx = Context();
    std::shared_ptr<core::AudioClock> audioClock = ctx ? ctx->Get<core::AudioClock>() : nullptr;
    std::shared_ptr<core::VideoPtsGenerator> ptsGen = ctx ? ctx->Get<core::VideoPtsGenerator>() : nullptr;
    std::shared_ptr<core::VideoOnlyTimeline> timeline = ctx ? ctx->Get<core::VideoOnlyTimeline>() : nullptr;
    std::shared_ptr<core::AVSync> sync = ctx ? ctx->Get<core::AVSync>() : nullptr;
    if (!ptsGen || !timeline) {
        TG_LOGE_THROTTLED(3000000, "VideoSyncNode: 总线上下文缺少 VideoPtsGenerator / VideoOnlyTimeline");
        return false;
    }
    const int64_t audioTimeoutMs = m_audioTimeoutMs.load();

    // ---- 音画同步：视频 PTS 由音频时钟反推（不用采集 wall clock）----
    // 对齐 MediaX：每帧独立判定，不保留"已降级"状态 ——
    // 音频恢复后下一帧 IsFresh() 自然为 true，直接切回主时钟；
    // 两条时间轴切换时都从最后一个已输出帧接续（见 m_lastVideoPtsUs），保证 PTS 不回退。
    //
    // 关键：判活用 IsFresh(audioTimeoutMs) 而不是 Valid()。Valid() 是单向开关
    // （第一次 Advance/Reserve 后永远 true），音频长时间断流时 ReferencePtsUs()
    // 会一直返回最后一次推进的旧 PTS —— VideoPtsGenerator 的停滞判定虽然能
    // clamp 到 audio+maxLead，但那个 audio 已经是几秒钟前的值，视频 PTS 会相对
    // 墙钟永久落后一个 drift。IsFresh 按"最近 N ms 内有过真实推进"判活，断流后
    // 会自然走到下面的 wall-clock fallback 分支。
    //
    // 有音频时"视频等音频"：主时钟尚未产出第一拍就先不发帧（最多等 audioTimeoutMs）。
    // 等不到才走采集时间轴 —— 这是 wall-clock fallback 分支，不污染音频时钟域。
    int64_t audioRef = core::kInvalidPtsUs;
    bool useAudioClock = false;
    if (m_config.audioEnabled && audioClock && audioClock->IsFresh(audioTimeoutMs)) {
        useAudioClock = true;
        audioRef = audioClock->ReferencePtsUs();
        // 音频刚从异常恢复：重置等待起点；VideoPtsGenerator 接续 fallback 期间已输出的 PTS
        // （它的内部 PTS 还停在断流前，直接用会产出回退 PTS，被编码器整段丢弃）
        if (m_audioWaitStartUs != 0) {
            TG_LOGI("TGCapturer: 音频主时钟恢复，切回主时钟（曾等待/断流 %lldms，接续 pts=%lld）",
                    static_cast<long long>((core::MonotonicClock::NowUs() - m_audioWaitStartUs) / 1000),
                    static_cast<long long>(m_lastVideoPtsUs));
            m_audioWaitStartUs = 0;
            ptsGen->Rebase(m_lastVideoPtsUs);
            timeline->Reset();
        }
    } else if (m_config.audioEnabled) {
        const int64_t nowUs = core::MonotonicClock::NowUs();
        if (m_audioWaitStartUs == 0) {
            m_audioWaitStartUs = nowUs;
            // 区分"首帧前等待" vs "运行中断流"的日志，便于定位
            if (audioClock && audioClock->Valid()) {
                TG_LOGW("TGCapturer: 音频主时钟断流（超过 %lldms 未推进），进入 wall-clock fallback，接续 pts=%lld",
                        static_cast<long long>(audioTimeoutMs), static_cast<long long>(m_lastVideoPtsUs));
            } else {
                TG_LOGI("TGCapturer: 等待音频主时钟首帧（最多 %lldms）", static_cast<long long>(audioTimeoutMs));
            }
            // 运行中断流：wall-clock 时间轴从最后一个已输出帧接续（pts + 采集时刻差），
            // 而不是从 0 重起 —— 从 0 起会比已输出的 PTS 小，整段被编码器按"PTS 回退"丢掉
            if (m_lastVideoPtsUs >= 0 && m_lastVideoCaptureUs >= 0) {
                timeline->Rebase(m_lastVideoPtsUs, m_lastVideoCaptureUs);
            } else {
                timeline->Reset();
            }
        }
        // 首帧前（Valid()==false）要尊重 audioTimeoutMs 的等待窗口；
        // 运行中断流（Valid()==true 但 IsFresh()==false）直接 fallback，不再等。
        const bool firstFrameWait = audioClock && !audioClock->Valid();
        if (firstFrameWait && (nowUs - m_audioWaitStartUs < audioTimeoutMs * 1000)) {
            return false;   // 正常等待，不计入丢帧统计
        }
        TG_LOGW_THROTTLED(5000000, "TGCapturer: 音频未就绪/断流，本帧走采集时间轴");
        // 注意：不调 AVSync::Reset()。
        // 当前帧用 wall-clock PTS，但 AVSync 里的旧 audio ref 已被 VideoEncoder 的落后丢帧判决所依赖；
        // 恢复音频主时钟后，上面 IsFresh 分支会接管，AVSync 由 UpdateAudioClock 刷新即可。
    }

    if (useAudioClock) {
        const int64_t pts = ptsGen->Next(audioRef, m_config.frameIntervalUs, true, audioClock->IdleUs());
        if (pts == core::kInvalidPtsUs) {
            // 音频停滞已到超前上限 / 视频突发超前音频：本帧不编码
            const uint64_t dropped = m_syncDropped.fetch_add(1) + 1;
            TG_LOGW_THROTTLED(2000000, "TGCapturer: 视频超前音频（突发到达或音频停滞），丢帧（syncDrop=%llu）",
                              static_cast<unsigned long long>(dropped));
            return false;
        }
        frame->ptsUs = pts;
        if (sync) {
            sync->UpdateAudioClock(audioRef);   // 供 VideoEncoder 的"落后丢帧"兜底
        }
    } else {
        // 无音频 / 音频超时：走采集时间轴（首帧归零，单调递增）
        const int64_t pts = timeline->Next(frame->captureUs);
        frame->ptsUs = pts >= 0 ? pts : frame->captureUs;
    }
    return Deliver(std::move(frame), /*externalTimeline=*/false);
}

bool VideoSyncNode::Deliver(core::FramePtr frame, bool externalTimeline)
{
    // 注意：frame 交给下游后会被移动，之后不可再访问其成员
    const int width = frame->width;
    const int height = frame->height;
    const int64_t pts = frame->ptsUs;
    const int64_t captureUs = frame->captureUs;

    if (!Emit(0, std::move(frame))) {
        return false;
    }
    if (!externalTimeline && pts >= 0) {
        m_lastVideoPtsUs = pts;
        m_lastVideoCaptureUs = captureUs;
    }
    if (!m_firstFrameSent.exchange(true)) {
        TG_LOGI("TGCapturer: 首帧视频 %dx%d pts=%lld%s", width, height, static_cast<long long>(pts),
                externalTimeline ? "（外部时间轴）" : "");
        ReportNotify(EsCap_Notify_Fisrt_Frame_OK, "first frame", 0);
    }
    return true;
}

std::string VideoSyncNode::DumpState() const
{
    char buf[96];
    std::snprintf(buf, sizeof(buf), "syncDrop=%llu bpDrop=%llu",
                  static_cast<unsigned long long>(m_syncDropped.load()),
                  static_cast<unsigned long long>(m_bpDropped.load()));
    return buf;
}

// ================================================================== PreviewSinkNode
void PreviewSinkNode::SetCallback(Callback cb)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_callback = std::move(cb);
}

bool PreviewSinkNode::Submit(core::FramePtr frame)
{
    if (!frame || frame->IsControl()) {
        return true;
    }
    Callback cb;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        cb = m_callback;
    }
    if (!cb) {
        return true;
    }
    PreviewFrame pf;
    pf.width = frame->width;
    pf.height = frame->height;
    pf.captureUs = frame->captureUs;
    if (frame->planes[0] && frame->pixFmt == AV_PIX_FMT_NV12) {
        pf.format = PreviewPixelFormat::NV12;
        pf.y = frame->planes[0].get();
        pf.yStride = frame->strides[0];
        pf.uv = frame->planes[1].get();
        pf.uvStride = frame->strides[1];
    } else if (frame->hwHandle) {
        pf.format = PreviewPixelFormat::Hardware;
        pf.hwHandle = frame->hwHandle;
    } else {
        return true;   // 其它 CPU 格式（I420 / RGBA）暂不送预览
    }
    cb(pf);
    m_delivered.fetch_add(1, std::memory_order_relaxed);
    return true;
}

std::string PreviewSinkNode::DumpState() const
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "delivered=%llu", static_cast<unsigned long long>(m_delivered.load()));
    return buf;
}

}  // namespace TGCapture::nodes
