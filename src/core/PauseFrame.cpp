#include "core/PauseFrame.h"

#include <algorithm>
#include <cstring>

#include "core/Logger.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace TGCapture::core
{

namespace
{
// 旋转一帧 NV12（90/180/270），返回新的 FramePtr；rotate=0 直接返回原帧。
// 这里只做静帧预处理，不走每帧路径，直接写循环即可，不引 libyuv。
FramePtr RotateNv12(const FramePtr& src, int rotate)
{
    if (!src || rotate == 0) return src;
    const int w = src->width, h = src->height;
    const int dstW = (rotate == 90 || rotate == 270) ? h : w;
    const int dstH = (rotate == 90 || rotate == 270) ? w : h;
    const size_t ySize = static_cast<size_t>(dstW) * dstH;
    auto buffer = std::shared_ptr<uint8_t>(new uint8_t[ySize + ySize / 2],
                                           std::default_delete<uint8_t[]>());
    uint8_t* ySrc = src->planes[0].get();
    uint8_t* uvSrc = src->planes[1].get();
    uint8_t* yDst = buffer.get();
    uint8_t* uvDst = buffer.get() + ySize;
    const int sY = src->strides[0] > 0 ? src->strides[0] : w;
    const int sUV = src->strides[1] > 0 ? src->strides[1] : w;

    auto putY = [&](int dx, int dy, uint8_t v) { yDst[dy * dstW + dx] = v; };
    auto putUV = [&](int dx, int dy, uint8_t u, uint8_t vv) {
        uvDst[(dy / 2) * dstW + (dx / 2) * 2] = u;
        uvDst[(dy / 2) * dstW + (dx / 2) * 2 + 1] = vv;
    };
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const uint8_t yv = ySrc[y * sY + x];
            const uint8_t uv0 = uvSrc[(y / 2) * sUV + (x / 2) * 2];
            const uint8_t uv1 = uvSrc[(y / 2) * sUV + (x / 2) * 2 + 1];
            int dx = x, dy = y;
            if (rotate == 90)  { dx = h - 1 - y; dy = x; }
            else if (rotate == 180) { dx = w - 1 - x; dy = h - 1 - y; }
            else if (rotate == 270) { dx = y; dy = w - 1 - x; }
            putY(dx, dy, yv);
            if ((x & 1) == 0 && (y & 1) == 0) putUV(dx & ~1, dy & ~1, uv0, uv1);
        }
    }
    auto out = MakeVideoFrame();
    out->width = dstW;
    out->height = dstH;
    out->pixFmt = AV_PIX_FMT_NV12;
    out->planes[0] = buffer;
    out->strides[0] = dstW;
    out->planes[1] = std::shared_ptr<uint8_t>(buffer, uvDst);
    out->strides[1] = dstW;
    return out;
}
}  // namespace

PauseFrame::~PauseFrame()
{
    ClearLocked();
}

void PauseFrame::ClearLocked()
{
    if (m_src) {
        av_frame_free(&m_src);
    }
    m_rotate = 0;
    m_frame.reset();
    m_width = 0;
    m_height = 0;
}

void PauseFrame::Clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    ClearLocked();
}

bool PauseFrame::Valid() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_frame != nullptr;
}

int PauseFrame::Width() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_width;
}

int PauseFrame::Height() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_height;
}

bool PauseFrame::Load(const std::string& path, int dstWidth, int dstHeight, int rotate)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ClearLocked();
    }
    if (path.empty() || dstWidth <= 0 || dstHeight <= 0) return false;

    // 解码不持锁：图片可能较大，避免合成线程（ApplyTo）在这期间卡住
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0 || !fmt) {
        TG_LOGW("PauseFrame: 打开 %s 失败", path.c_str());
        return false;
    }
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt);
        return false;
    }
    int streamIdx = -1;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            streamIdx = static_cast<int>(i);
            break;
        }
    }
    if (streamIdx < 0) {
        avformat_close_input(&fmt);
        return false;
    }
    AVCodecParameters* cp = fmt->streams[streamIdx]->codecpar;
    const AVCodec* dec = avcodec_find_decoder(cp->codec_id);
    if (!dec) {
        avformat_close_input(&fmt);
        return false;
    }
    AVCodecContext* ctx = avcodec_alloc_context3(dec);
    if (!ctx) {
        avformat_close_input(&fmt);
        return false;
    }
    avcodec_parameters_to_context(ctx, cp);
    if (avcodec_open2(ctx, dec, nullptr) < 0) {
        avcodec_free_context(&ctx);
        avformat_close_input(&fmt);
        return false;
    }

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    bool got = false;
    while (!got && av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == streamIdx && avcodec_send_packet(ctx, pkt) >= 0) {
            if (avcodec_receive_frame(ctx, frame) >= 0) got = true;
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);

    if (!got) {
        av_frame_free(&frame);
        TG_LOGW("PauseFrame: %s 解码未产出帧", path.c_str());
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    ClearLocked();
    m_src = frame;
    m_rotate = rotate;
    if (!RebuildLocked(dstWidth, dstHeight)) {
        ClearLocked();
        return false;
    }
    TG_LOGI("PauseFrame: 已加载 %s → NV12 %dx%d（rotate=%d）", path.c_str(), m_width, m_height, rotate);
    return true;
}

bool PauseFrame::RebuildLocked(int dstWidth, int dstHeight)
{
    if (!m_src || dstWidth <= 0 || dstHeight <= 0) return false;
    // 先缩放到目标尺寸的 NV12（没有旋转），再按需做一次整帧旋转。
    const bool swap = m_rotate == 90 || m_rotate == 270;
    const int targetW = swap ? dstHeight : dstWidth;
    const int targetH = swap ? dstWidth : dstHeight;
    SwsContext* sws = sws_getContext(m_src->width, m_src->height,
                                     static_cast<AVPixelFormat>(m_src->format),
                                     targetW, targetH, AV_PIX_FMT_NV12,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws) {
        return false;
    }

    const size_t ySize = static_cast<size_t>(targetW) * targetH;
    auto buffer = std::shared_ptr<uint8_t>(new uint8_t[ySize + ySize / 2],
                                           std::default_delete<uint8_t[]>());
    uint8_t* yDst = buffer.get();
    uint8_t* uvDst = buffer.get() + ySize;
    uint8_t* dstPlanes[4] = {yDst, uvDst, nullptr, nullptr};
    int dstLines[4] = {targetW, targetW, 0, 0};
    sws_scale(sws, m_src->data, m_src->linesize, 0, m_src->height, dstPlanes, dstLines);
    sws_freeContext(sws);

    auto nv12 = MakeVideoFrame();
    nv12->width = targetW;
    nv12->height = targetH;
    nv12->pixFmt = AV_PIX_FMT_NV12;
    nv12->planes[0] = buffer;
    nv12->strides[0] = targetW;
    nv12->planes[1] = std::shared_ptr<uint8_t>(buffer, uvDst);
    nv12->strides[1] = targetW;

    m_frame = RotateNv12(nv12, m_rotate);
    m_width = m_frame->width;
    m_height = m_frame->height;
    return true;
}

bool PauseFrame::ApplyTo(Frame& target)
{
    if (target.pixFmt != AV_PIX_FMT_NV12) return false;
    if (!target.planes[0] || !target.planes[1] || target.width <= 0 || target.height <= 0) return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_frame) return false;
    if (target.width != m_width || target.height != m_height) {
        // 帧尺寸变了（运行期切分辨率 / 采集改尺寸）：按新尺寸重缩放一次，之后同尺寸直接复用
        const int oldW = m_width, oldH = m_height;
        if (!RebuildLocked(target.width, target.height) || m_width != target.width ||
            m_height != target.height) {
            TG_LOGW_THROTTLED(3000000, "PauseFrame: 按 %dx%d 重新缩放失败", target.width, target.height);
            return false;
        }
        TG_LOGI("PauseFrame: 帧尺寸变化 %dx%d -> %dx%d，已重新缩放", oldW, oldH, m_width, m_height);
    }
    const int srcY = m_frame->strides[0], srcUV = m_frame->strides[1];
    for (int y = 0; y < target.height; ++y) {
        memcpy(target.planes[0].get() + static_cast<size_t>(y) * target.strides[0],
               m_frame->planes[0].get() + static_cast<size_t>(y) * srcY, m_width);
    }
    for (int y = 0; y < target.height / 2; ++y) {
        memcpy(target.planes[1].get() + static_cast<size_t>(y) * target.strides[1],
               m_frame->planes[1].get() + static_cast<size_t>(y) * srcUV, m_width);
    }
    return true;
}

}  // namespace TGCapture::core
