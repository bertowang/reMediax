#include "nodes/VideoNodes.h"

#include <cstdio>

#include "core/AVSyncClock.h"
#include "core/Clock.h"
#include "core/Logger.h"
#include "core/PauseFrame.h"
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

bool VideoCompositorNode::NodeStart()
{
    m_running.store(true);
    return true;
}

void VideoCompositorNode::NodeStop()
{
    m_running.store(false);
}

bool VideoCompositorNode::SubmitInput(size_t port, core::FramePtr frame)
{
    if (!frame || !m_running.load()) {
        return false;
    }
    if (port != kMainInput) {
        return false;   // 叠加层输入（后续版本）
    }
    // 看护判定的是"采集端还在不在产出"，而不是"这一帧最终有没有被编码"：
    // 下面的 Mute 丢帧、硬件帧无法改写等分支都会提前返回，但这些都不是采集故障，打点必须在它们之前
    if (m_onMain) {
        m_onMain(core::MonotonicClock::NowUs());
    }

    std::shared_ptr<core::PauseFrame> pause;
    std::shared_ptr<core::Watermark> watermark;
    {
        std::lock_guard<std::mutex> lock(m_layerMutex);
        pause = m_pause;
        watermark = m_watermark;
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

    // ---- 水印图层：仅对 CPU 帧生效；硬件直通帧跳过并限频提示 ----
    if (watermark && !watermark->Empty()) {
        if (frame->planes[0] && frame->pixFmt == AV_PIX_FMT_NV12) {
            watermark->BlendOnto(*frame);
        } else if (frame->hwHandle) {
            TG_LOGW_THROTTLED(3000000, "TGCapturer: 硬件帧路径暂不支持水印，本帧水印已跳过");
        }
    }

    m_composed.fetch_add(1, std::memory_order_relaxed);
    // 预览与推流共享像素（下游不得修改像素）。预览给一份浅拷贝的帧头：
    // 推流链路会改写 ptsUs 等元数据，而预览在另一个线程上读，共用帧头会产生数据竞争
    if (HasDownstream(kPreviewOutput)) {
        Emit(kPreviewOutput, std::make_shared<core::Frame>(*frame));
    }
    return Emit(kProgramOutput, std::move(frame));
}

std::string VideoCompositorNode::DumpState() const
{
    bool pauseValid = false;
    bool watermark = false;
    {
        std::lock_guard<std::mutex> lock(m_layerMutex);
        pauseValid = m_pause && m_pause->Valid();
        watermark = m_watermark && !m_watermark->Empty();
    }
    char buf[128];
    std::snprintf(buf, sizeof(buf), "layers=[main%s%s] muted=%d composed=%llu",
                  pauseValid ? ",pause" : "", watermark ? ",watermark" : "", m_muted.load() ? 1 : 0,
                  static_cast<unsigned long long>(m_composed.load()));
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
