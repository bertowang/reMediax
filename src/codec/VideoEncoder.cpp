#include "codec/VideoEncoder.h"

#include <algorithm>
#include <limits>

#include "core/Logger.h"
#include "mux/Muxer.h"

extern "C"
{
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
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

    AVPixelFormat hwFmt = HwInputFormat(m_codecName);
    if (hwFmt != AV_PIX_FMT_NONE && InitHardwareContext(hwFmt)) {
        m_ctx->pix_fmt = hwFmt;
        m_useHwFrame = true;
    } else {
        // 软编或硬编降级：优先 NV12（硬编友好），否则 YUV420P
        m_ctx->pix_fmt = AV_PIX_FMT_NV12;
        if (!codec->pix_fmts || !avcodec_find_best_pix_fmt_of_list(codec->pix_fmts, AV_PIX_FMT_NV12, 0, nullptr)) {
            m_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
        }
        m_swPixFmt = m_ctx->pix_fmt;
    }

    // 低延迟/实时参数
    av_opt_set(m_ctx->priv_data, "preset", m_useHwFrame ? "p1" : "veryfast", 0);
    av_opt_set(m_ctx->priv_data, "tune", "zerolatency", 0);
    av_opt_set(m_ctx->priv_data, "rc", "cbr", 0);
    if (!params.vExtParams.empty()) {
        av_opt_set(m_ctx->priv_data, "x264-params", params.vExtParams.c_str(), 0);
    }

    m_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    int ret = avcodec_open2(m_ctx, codec, nullptr);
    if (ret < 0) {
        TG_LOGE("VideoEncoder: avcodec_open2 失败 %d (%s)", ret, m_codecName.c_str());
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
    if (av_hwdevice_ctx_create(&m_hwDeviceCtx, type, nullptr, nullptr, 0) < 0) {
        TG_LOGW("VideoEncoder: hwdevice 创建失败，回退软编路径");
        m_hwDeviceCtx = nullptr;
        return false;
    }
    m_ctx->hw_device_ctx = av_buffer_ref(m_hwDeviceCtx);
    TG_LOGI("VideoEncoder: 硬件上下文就绪 type=%d", static_cast<int>(type));
    return true;
}

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
        return -1;
    }

    // 音画同步：视频落后音频过多则丢弃
    if (m_sync && frame->ptsUs >= 0) {
        auto decision = m_sync->Judge(frame->ptsUs, 40000, m_maxLagUs, nullptr);
        if (decision == core::AVSync::Decision::Drop) {
            m_dropped.fetch_add(1);
            return 0;
        }
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
        TG_LOGW("VideoEncoder: send_frame 失败 ret=%d", ret);
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
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF || ret < 0) {
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

    // 零拷贝：硬件帧直接引用采集侧句柄
    if (m_useHwFrame && frame->hwHandle != nullptr) {
        // 由平台实现把 native handle 填入 AVFrame 的 data[3]/hw_frames_ctx
        avFrame->format = m_ctx->pix_fmt;
        avFrame->hw_frames_ctx = m_hwFramesCtx ? av_buffer_ref(m_hwFramesCtx) : nullptr;
        // 约定：data[3] 存放 native handle 指针（与 ffmpeg 的 d3d11/videotoolbox 用法一致）
        avFrame->data[3] = reinterpret_cast<uint8_t*>(frame->hwHandle);
        avFrame->buf[0] = nullptr;
        if (!avFrame->hw_frames_ctx) {
            TG_LOGW("VideoEncoder: 缺少 hw_frames_ctx，回退系统内存帧");
            m_useHwFrame = false;
        } else {
            return avFrame;
        }
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
