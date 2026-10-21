#include "codec/VideoEncoder.h"

#include <algorithm>
#include <limits>

#include "core/AvError.h"
#include "core/Logger.h"
#include "mux/Muxer.h"

#ifdef _WIN32
#include <d3d11.h>
#endif

extern "C"
{
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#ifdef _WIN32
#include <libavutil/hwcontext_d3d11va.h>
#endif
}

namespace TGCapture::codec
{

namespace
{
// 编码器名 -> 期望的硬件输入格式（用于零拷贝判断）
AVPixelFormat HwInputFormat(const std::string& name)
{
    if (name == "h264_nvenc" || name == "h264_mf" || name == "h264_amf" || name == "hevc_nvenc") {
        return AV_PIX_FMT_D3D11;
    }
    if (name == "h264_videotoolbox") {
        return AV_PIX_FMT_VIDEOTOOLBOX;
    }
    if (name == "h264_mediacodec") {
        return AV_PIX_FMT_MEDIACODEC;
    }
    return AV_PIX_FMT_NONE;
}

// 编码器是否接受 BGRA 硬件帧（决定 D3D11 帧池的 sw_format，避免多余的格式转换）
bool CodecAcceptsBgra(const std::string& name)
{
    return name == "h264_nvenc" || name == "hevc_nvenc";
}

AVHWDeviceType HwDeviceType(AVPixelFormat fmt)
{
    switch (fmt) {
        case AV_PIX_FMT_D3D11:
            return AV_HWDEVICE_TYPE_D3D11VA;
        case AV_PIX_FMT_VIDEOTOOLBOX:
            return AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
        case AV_PIX_FMT_MEDIACODEC:
            return AV_HWDEVICE_TYPE_MEDIACODEC;
        default:
            return AV_HWDEVICE_TYPE_NONE;
    }
}
}  // namespace

VideoEncoder::VideoEncoder(size_t queueCapacity)
    : core::BusNode("video_encoder"),
      core::Worker("video_encoder"),
      m_queue(std::make_unique<core::BoundedQueue<core::FramePtr>>(queueCapacity, core::DropPolicy::DropOldest))
{
}

VideoEncoder::~VideoEncoder()
{
    Stop();
    Close();
}

int VideoEncoder::Open(const VideoEncoderParams& params, const core::VideoMediaType& srcType)
{
    if (m_opened.load()) {
        return 0;
    }
    // 重开：队列在 Close() 时被永久关闭，必须显式重开，否则这一轮的 Submit 全部失败
    m_queue->Reopen();
    m_encoded.store(0);
    m_dropped.store(0);
    m_srcType = srcType;
    m_codecName = params.vCodec.empty() ? "libopenh264" : params.vCodec;

    const AVCodec* codec = avcodec_find_encoder_by_name(m_codecName.c_str());
    if (!codec) {
        TG_LOGE("VideoEncoder: 找不到编码器 %s", m_codecName.c_str());
        return -1;
    }

    m_ctx = avcodec_alloc_context3(codec);
    if (!m_ctx) {
        return -2;
    }

    m_ctx->width = params.width > 0 ? params.width : srcType.Width();
    m_ctx->height = params.height > 0 ? params.height : srcType.Height();
    m_ctx->time_base = AVRational{1, 1000000};  // 微秒时间基，简化 PTS 换算
    m_ctx->framerate = srcType.FrameRate();
    m_ctx->gop_size = params.gop > 0 ? params.gop : 15;
    m_ctx->max_b_frames = 0;  // 直播低延迟（ffmpeg 6.x 字段名为 max_b_frames）
    m_ctx->bit_rate = params.vBitrate;
    m_ctx->rc_max_rate = static_cast<int64_t>(params.vBitrate * (params.vBitrateMaxFactor > 0 ? params.vBitrateMaxFactor : 1.0f));
    // rc_buffer_size 是 int，rc_max_rate 是 int64_t：夹到 int 上界再转，避免 C4244 静默截断
    m_ctx->rc_buffer_size = static_cast<int>(
        std::min<int64_t>(m_ctx->rc_max_rate * 2, std::numeric_limits<int>::max()));
    m_ctx->thread_count = params.threads > 0 ? params.threads : 4;
    m_ctx->pix_fmt = AV_PIX_FMT_NONE;

    // GPU 模式：采集帧留在显存，直接喂硬编（CPU 不做像素搬运）
    // CPU 模式：一律走系统内存帧（NV12），保留既有链路
    AVPixelFormat hwFmt = (m_perfMode == PerformanceMode::Gpu) ? HwInputFormat(m_codecName) : AV_PIX_FMT_NONE;
    if (hwFmt != AV_PIX_FMT_NONE && InitHardwareContext(hwFmt) && InitHardwareFrames(hwFmt)) {
        m_ctx->pix_fmt = hwFmt;
        m_useHwFrame = true;
        TG_LOGI("VideoEncoder: GPU 链路就绪（硬件帧直通，pix_fmt=%d）", static_cast<int>(hwFmt));
    } else {
        // 软编或硬编降级：优先 NV12（硬编友好），否则 YUV420P
        m_ctx->pix_fmt = AV_PIX_FMT_NV12;
        if (!codec->pix_fmts || !avcodec_find_best_pix_fmt_of_list(codec->pix_fmts, AV_PIX_FMT_NV12, 0, nullptr)) {
            m_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
        }
        m_swPixFmt = m_ctx->pix_fmt;
    }

    // 低延迟/实时参数
    const bool isNvenc = m_codecName.find("nvenc") != std::string::npos;
    av_opt_set(m_ctx->priv_data, "preset", isNvenc ? "p1" : "veryfast", 0);
    // nvenc 没有 tune 选项，强行设置会打 "Unable to parse option value" 告警
    if (!isNvenc) {
        av_opt_set(m_ctx->priv_data, "tune", "zerolatency", 0);
    }
    av_opt_set(m_ctx->priv_data, "rc", "cbr", 0);
    if (!params.vExtParams.empty()) {
        av_opt_set(m_ctx->priv_data, "x264-params", params.vExtParams.c_str(), 0);
    }

    m_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    int ret = avcodec_open2(m_ctx, codec, nullptr);
    if (ret < 0) {
        TG_LOGE("VideoEncoder: avcodec_open2 失败 %d (%s) codec=%s", ret, core::AvErr(ret),
                m_codecName.c_str());
        avcodec_free_context(&m_ctx);
        return -3;
    }

    m_dstType.SetWidth(m_ctx->width);
    m_dstType.SetHeight(m_ctx->height);
    m_dstType.SetPixelFormat(m_ctx->pix_fmt);
    m_dstType.SetFrameRate(m_ctx->framerate.num, m_ctx->framerate.den);

    m_opened.store(true);
    TG_LOGI("VideoEncoder: %s %dx%d fps=%.2f hw=%d",
            m_codecName.c_str(), m_ctx->width, m_ctx->height, m_dstType.FrameRateFloat(), m_useHwFrame ? 1 : 0);
    return 0;
}

bool VideoEncoder::InitHardwareContext(AVPixelFormat srcFmt)
{
    AVHWDeviceType type = HwDeviceType(srcFmt);
    if (type == AV_HWDEVICE_TYPE_NONE) {
        return false;
    }

#ifdef _WIN32
    // Windows GPU 链路：复用采集侧的 D3D11 设备，这样采集纹理可以只在显存里拷贝，
    // 既不需要 GPU→CPU 回读，也不会出现跨设备纹理无法 CopyResource 的问题。
    if (type == AV_HWDEVICE_TYPE_D3D11VA && m_externalDevice) {
        auto* device = static_cast<ID3D11Device*>(m_externalDevice);
        m_hwDeviceCtx = av_hwdevice_ctx_alloc(type);
        if (m_hwDeviceCtx) {
            auto* deviceCtx = reinterpret_cast<AVHWDeviceContext*>(m_hwDeviceCtx->data);
            auto* d3d11Ctx = static_cast<AVD3D11VADeviceContext*>(deviceCtx->hwctx);
            d3d11Ctx->device = device;
            device->AddRef();
            device->GetImmediateContext(&d3d11Ctx->device_context);
            m_d3dContext = d3d11Ctx->device_context;
            if (av_hwdevice_ctx_init(m_hwDeviceCtx) < 0) {
                TG_LOGW("VideoEncoder: 复用采集 D3D11 设备失败，改用独立设备");
                av_buffer_unref(&m_hwDeviceCtx);
                m_d3dContext = nullptr;
            }
        }
    }
#endif

    if (!m_hwDeviceCtx && av_hwdevice_ctx_create(&m_hwDeviceCtx, type, nullptr, nullptr, 0) < 0) {
        TG_LOGW("VideoEncoder: hwdevice 创建失败，回退 CPU 路径");
        m_hwDeviceCtx = nullptr;
        return false;
    }
    m_ctx->hw_device_ctx = av_buffer_ref(m_hwDeviceCtx);
    TG_LOGI("VideoEncoder: 硬件上下文就绪 type=%d", static_cast<int>(type));
    return true;
}

bool VideoEncoder::InitHardwareFrames(AVPixelFormat hwFmt)
{
    if (!m_hwDeviceCtx) {
        return false;
    }
    AVBufferRef* frames = av_hwframe_ctx_alloc(m_hwDeviceCtx);
    if (!frames) {
        return false;
    }
    auto* framesCtx = reinterpret_cast<AVHWFramesContext*>(frames->data);
    framesCtx->format = hwFmt;
    // D3D11 链路：采集纹理是 BGRA，硬件帧也按 BGRA 分配，显存内 CopySubresourceRegion 才能直拷；
    // 若编码器不接受 BGRA（如 AMF 只吃 nv12），则按 NV12 分配，由采集侧转好格式再拷。
    framesCtx->sw_format = (hwFmt == AV_PIX_FMT_D3D11 && m_ctx->pix_fmt != AV_PIX_FMT_NV12 &&
                            CodecAcceptsBgra(m_codecName))
                               ? AV_PIX_FMT_BGRA
                               : AV_PIX_FMT_NV12;
    framesCtx->width = m_ctx->width;
    framesCtx->height = m_ctx->height;
    framesCtx->initial_pool_size = 16;

    if (av_hwframe_ctx_init(frames) < 0) {
        TG_LOGW("VideoEncoder: hw_frames_ctx 初始化失败，回退 CPU 路径");
        av_buffer_unref(&frames);
        return false;
    }
    m_hwFramesCtx = frames;
    m_ctx->hw_frames_ctx = av_buffer_ref(frames);
    TG_LOGI("VideoEncoder: 硬件帧池就绪 fmt=%d sw=%d %dx%d",
            static_cast<int>(framesCtx->format), static_cast<int>(framesCtx->sw_format),
            framesCtx->width, framesCtx->height);
    return true;
}

#ifdef _WIN32
bool VideoEncoder::CopyFromD3D11Texture(const core::FramePtr& frame, AVFrame* dst)
{
    if (!dst || !m_d3dContext || !frame || !frame->hwHandle) {
        return false;
    }
    auto* src = static_cast<ID3D11Texture2D*>(frame->hwHandle);
    auto* dstTex = reinterpret_cast<ID3D11Texture2D*>(dst->data[0]);
    if (!dstTex) {
        return false;
    }
    const UINT subResource = static_cast<UINT>(reinterpret_cast<uintptr_t>(dst->data[1]));
    // 纯显存拷贝：无 CPU 回读、无像素搬运
    m_d3dContext->CopySubresourceRegion(dstTex, subResource, 0, 0, 0, src, 0, nullptr);
    return true;
}
#endif

void VideoEncoder::Close()
{
    if (m_ctx) {
        avcodec_free_context(&m_ctx);
    }
    if (m_sws) {
        sws_freeContext(m_sws);
        m_sws = nullptr;
    }
    if (m_hwFramesCtx) {
        av_buffer_unref(&m_hwFramesCtx);
        m_hwFramesCtx = nullptr;
    }
    if (m_hwDeviceCtx) {
        av_buffer_unref(&m_hwDeviceCtx);
        m_hwDeviceCtx = nullptr;
    }
    m_queue->Close();
    m_opened.store(false);
    // 关键：PTS 单调保护的状态必须随流复位。
    // 否则 Stop→Start 重开时，新一轮时间轴已从 0 起，而 m_lastPtsUs 还是上一轮的末值，
    // 所有视频帧都会被判成"PTS 回退"而丢弃，第二个文件只有音频甚至 0 字节。
    m_lastPtsUs = -1;
}

bool VideoEncoder::Submit(core::FramePtr frame)
{
    if (!m_opened.load() || !frame) {
        return false;
    }
    return m_queue->Push(std::move(frame));
}

size_t VideoEncoder::Pending() const
{
    return m_queue ? m_queue->Size() : 0;
}

size_t VideoEncoder::DownstreamPending() const
{
    return m_muxer ? m_muxer->VideoPending() : 0;
}

void VideoEncoder::Loop(std::stop_token token)
{
    while (!token.stop_requested()) {
        auto item = m_queue->TryPop(std::chrono::milliseconds(50));
        if (!item.has_value()) {
            continue;
        }
        if (EncodeOne(*item) < 0) {
            TG_LOGW("VideoEncoder: 编码一帧失败");
        }
    }
    DrainLocked();
}

void VideoEncoder::OnThreadExit()
{
    TG_LOGI("VideoEncoder: 线程退出 encoded=%llu dropped=%llu",
            static_cast<unsigned long long>(m_encoded.load()),
            static_cast<unsigned long long>(m_dropped.load()));
}

int VideoEncoder::EncodeOne(const core::FramePtr& frame)
{
    if (!m_ctx || !frame) {
        TG_LOGW_THROTTLED(1000000, "VideoEncoder: 编码器未打开或空帧，丢弃（ctx=%d frame=%d）",
                          m_ctx ? 1 : 0, frame ? 1 : 0);
        return -1;
    }

    // 音画同步：视频落后音频过多则丢弃
    if (m_sync && frame->ptsUs >= 0) {
        // 领先/落后阈值都用同一套配置，不在这里硬编码魔法数字
        auto decision = m_sync->Judge(frame->ptsUs, m_maxLagUs, m_maxLagUs, nullptr);
        if (decision == core::AVSync::Decision::Drop) {
            m_dropped.fetch_add(1);
            TG_LOGW_THROTTLED(1000000, "VideoEncoder: 视频落后音频 %lldms 超阈值，丢帧",
                              static_cast<long long>(m_maxLagUs / 1000));
            return 0;
        }
    }
    // PTS 回退保护：dts 必须单调递增，否则 flv/ts 封装会直接报错写不出文件
    if (frame->ptsUs >= 0 && frame->ptsUs <= m_lastPtsUs) {
        m_dropped.fetch_add(1);
        TG_LOGW_THROTTLED(1000000, "VideoEncoder: PTS 回退 %lld <= %lld，丢帧",
                          static_cast<long long>(frame->ptsUs), static_cast<long long>(m_lastPtsUs));
        return 0;
    }
    if (frame->ptsUs >= 0) {
        m_lastPtsUs = frame->ptsUs;
    }

    const uint64_t frameNo = m_encoded.load();
    const bool trace = frameNo < 5;
    if (trace) {
        TG_LOGD("VideoEncoder[#%llu]: 编码 %dx%d pts=%lld", static_cast<unsigned long long>(frameNo),
                frame->width, frame->height, static_cast<long long>(frame->ptsUs));
    }
    auto avFrame = BuildAVFrame(frame);
    if (!avFrame) {
        TG_LOGW("VideoEncoder: BuildAVFrame 返回空");
        return -2;
    }
    if (trace) {
        TG_LOGD("VideoEncoder[#%llu]: AVFrame fmt=%d %dx%d", static_cast<unsigned long long>(frameNo),
                avFrame->format, avFrame->width, avFrame->height);
    }

    int ret = avcodec_send_frame(m_ctx, avFrame.get());
    if (ret < 0) {
        TG_LOGE_THROTTLED(1000000, "VideoEncoder: send_frame 失败 %d (%s)", ret, core::AvErr(ret));
        return -3;
    }
    if (trace) {
        TG_LOGD("VideoEncoder[#%llu]: send ok", static_cast<unsigned long long>(frameNo));
    }

    while (ret >= 0) {
        core::AVPacketPtr packet = core::MakeAVPacket();
        ret = avcodec_receive_packet(m_ctx, packet.get());
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            TG_LOGE_THROTTLED(1000000, "VideoEncoder: receive_packet 失败 %d (%s)", ret, core::AvErr(ret));
            return -4;
        }
        packet->flags |= (packet->flags & AV_PKT_FLAG_KEY) ? AV_PKT_FLAG_KEY : 0;
        DeliverPacket(std::move(packet));
    }

    m_encoded.fetch_add(1);
    return 0;
}

int VideoEncoder::DrainLocked()
{
    if (!m_ctx) {
        return 0;
    }
    avcodec_send_frame(m_ctx, nullptr);
    while (true) {
        core::AVPacketPtr packet = core::MakeAVPacket();
        int ret = avcodec_receive_packet(m_ctx, packet.get());
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            TG_LOGW("VideoEncoder: drain 收包失败 %d (%s)", ret, core::AvErr(ret));
            break;
        }
        DeliverPacket(std::move(packet));
    }
    return 0;
}

void VideoEncoder::DeliverPacket(core::AVPacketPtr packet)
{
    if (m_muxer && packet) {
        m_muxer->SubmitVideoPacket(std::move(packet));
    }
}

core::AVFramePtr VideoEncoder::BuildAVFrame(const core::FramePtr& frame)
{
    auto avFrame = core::MakeAVFrame();
    avFrame->format = m_ctx->pix_fmt;
    avFrame->width = m_ctx->width;
    avFrame->height = m_ctx->height;
    // 按编码器实际 time_base 换算（openh264 等可能调整 time_base）
    avFrame->pts = frame->ptsUs >= 0
                       ? av_rescale_q(frame->ptsUs, AVRational{1, 1000000}, m_ctx->time_base)
                       : AV_NOPTS_VALUE;
    avFrame->pkt_dts = AV_NOPTS_VALUE;

    // ---- 硬件直连（GPU 模式）----
    if (m_useHwFrame && frame->hwHandle != nullptr) {
#ifdef _WIN32
        // Windows：D3D11 纹理。从帧池取一个硬件帧，把采集纹理在显存里拷进去（无 CPU 回读）
        if (m_ctx->pix_fmt == AV_PIX_FMT_D3D11 && m_hwFramesCtx &&
            frame->hwKind == core::HwKind::D3D11Texture) {
            if (av_hwframe_get_buffer(m_hwFramesCtx, avFrame.get(), 0) < 0) {
                TG_LOGW("VideoEncoder: 硬件帧分配失败，本帧走系统内存");
            } else if (!CopyFromD3D11Texture(frame, avFrame.get())) {
                TG_LOGW("VideoEncoder: 显存拷贝失败，本帧走系统内存");
            } else {
                return avFrame;
            }
        }
#endif
        // iOS VideoToolbox / Android MediaCodec：data[3] 存 native handle（零拷贝直通）
        if (m_ctx->pix_fmt == AV_PIX_FMT_VIDEOTOOLBOX &&
            frame->hwKind == core::HwKind::CVPixelBuffer) {
            avFrame->format = m_ctx->pix_fmt;
            avFrame->data[3] = reinterpret_cast<uint8_t*>(frame->hwHandle);
            avFrame->buf[0] = nullptr;
            return avFrame;
        }
        if (m_ctx->hw_frames_ctx) {
            avFrame->hw_frames_ctx = av_buffer_ref(m_ctx->hw_frames_ctx);
        }
        if (avFrame->hw_frames_ctx) {
            avFrame->format = m_ctx->pix_fmt;
            avFrame->data[3] = reinterpret_cast<uint8_t*>(frame->hwHandle);
            avFrame->buf[0] = nullptr;
            return avFrame;
        }
        TG_LOGW("VideoEncoder: 硬件帧不可用，本帧走系统内存");
    }

    // 系统内存帧：经 sws_scale 归一到编码器的尺寸与像素格式
    // （采集分辨率常与推流分辨率不同，直接 memcpy 会越界，这里统一做缩放/格式转换）
    if (frame->planes[0]) {
        const int srcW = frame->width > 0 ? frame->width : m_ctx->width;
        const int srcH = frame->height > 0 ? frame->height : m_ctx->height;
        const AVPixelFormat srcFmt = frame->pixFmt != AV_PIX_FMT_NONE ? frame->pixFmt : AV_PIX_FMT_NV12;
        if (!EnsureSwsContext(srcW, srcH, srcFmt)) {
            return nullptr;
        }
        if (av_frame_get_buffer(avFrame.get(), 32) < 0) {
            return nullptr;
        }
        const uint8_t* srcData[4] = {frame->planes[0].get(), frame->planes[1].get(),
                                     frame->planes[2].get(), nullptr};
        int srcLinesize[4] = {frame->strides[0], frame->strides[1], frame->strides[2], 0};
        sws_scale(m_sws, srcData, srcLinesize, 0, srcH, avFrame->data, avFrame->linesize);
        avFrame->pts = frame->ptsUs >= 0
                           ? av_rescale_q(frame->ptsUs, AVRational{1, 1000000}, m_ctx->time_base)
                           : AV_NOPTS_VALUE;
        return avFrame;
    }

    // 空帧（仅用于 flush）
    return nullptr;
}

bool VideoEncoder::EnsureSwsContext(int srcWidth, int srcHeight, AVPixelFormat srcFmt)
{
    if (m_sws && m_swsSrcWidth == srcWidth && m_swsSrcHeight == srcHeight && m_swsSrcFmt == srcFmt) {
        return true;
    }
    if (m_sws) {
        sws_freeContext(m_sws);
        m_sws = nullptr;
    }
    m_sws = sws_getContext(srcWidth, srcHeight, srcFmt, m_ctx->width, m_ctx->height, m_ctx->pix_fmt,
                           SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!m_sws) {
        TG_LOGE("VideoEncoder: sws_getContext 失败 %dx%d -> %dx%d", srcWidth, srcHeight, m_ctx->width, m_ctx->height);
        return false;
    }
    m_swsSrcWidth = srcWidth;
    m_swsSrcHeight = srcHeight;
    m_swsSrcFmt = srcFmt;
    return true;
}

}  // namespace TGCapture::codec
