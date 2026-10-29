#include "core/Watermark.h"

#include <algorithm>
#include <cstring>
#include <vector>

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
// BT.601 系数（整数化，避免浮点开销）
inline uint8_t Clamp8(int v) { return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v)); }

void BGRAToNv12Alpha(const uint8_t* bgra, int w, int h, int stride,
                     uint8_t* y, uint8_t* uv, uint8_t* a)
{
    for (int row = 0; row < h; ++row) {
        for (int col = 0; col < w; ++col) {
            const uint8_t* p = bgra + static_cast<size_t>(row) * stride + col * 4;
            const int B = p[0], G = p[1], R = p[2], A = p[3];
            y[row * w + col] = Clamp8((66 * R + 129 * G + 25 * B + 128) / 256 + 16);
            a[row * w + col] = static_cast<uint8_t>(A);
        }
    }
    for (int row = 0; row < h; row += 2) {
        for (int col = 0; col < w; col += 2) {
            // 2x2 色度采样（取左上角像素；水印一般低频，和平均效果近似）
            const uint8_t* p = bgra + static_cast<size_t>(row) * stride + col * 4;
            const int B = p[0], G = p[1], R = p[2];
            uv[(row / 2) * w + (col / 2) * 2 + 0] =
                Clamp8((-38 * R - 74 * G + 112 * B + 128) / 256 + 128);
            uv[(row / 2) * w + (col / 2) * 2 + 1] =
                Clamp8((112 * R - 94 * G - 18 * B + 128) / 256 + 128);
        }
    }
}
}  // namespace

int Watermark::ResolvePosition(int32_t pos, int32_t x, int32_t y,
                               int canvasW, int canvasH, int logoW, int logoH,
                               int& outX, int& outY)
{
    switch (pos) {
        case 0: outX = 0;                     outY = 0;                      return 0;  // 左上
        case 1: outX = canvasW - logoW;       outY = 0;                      return 0;  // 右上
        case 2: outX = 0;                     outY = canvasH - logoH;        return 0;  // 左下
        case 3: outX = canvasW - logoW;       outY = canvasH - logoH;        return 0;  // 右下
        case 4: outX = (canvasW - logoW) / 2; outY = 0;                      return 0;  // 顶中
        case 5: outX = (canvasW - logoW) / 2; outY = canvasH - logoH;        return 0;  // 底中
        default: outX = x; outY = y; return 0;
    }
}

bool Watermark::DecodeImage(const std::string& url, int canvasW, int canvasH,
                            WatermarkTexture& out, float opacity, int32_t position,
                            int32_t offX, int32_t offY)
{
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, url.c_str(), nullptr, nullptr) < 0 || !fmt) {
        TG_LOGW("Watermark: 打开图片失败 %s", url.c_str());
        return false;
    }
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt); return false;
    }
    int idx = -1;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) { idx = static_cast<int>(i); break; }
    }
    if (idx < 0) { avformat_close_input(&fmt); return false; }
    const AVCodec* dec = avcodec_find_decoder(fmt->streams[idx]->codecpar->codec_id);
    if (!dec) { avformat_close_input(&fmt); return false; }
    AVCodecContext* ctx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(ctx, fmt->streams[idx]->codecpar);
    if (avcodec_open2(ctx, dec, nullptr) < 0) {
        avcodec_free_context(&ctx); avformat_close_input(&fmt); return false;
    }
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    bool got = false;
    while (!got && av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == idx && avcodec_send_packet(ctx, pkt) >= 0) {
            if (avcodec_receive_frame(ctx, frame) >= 0) got = true;
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    if (!got) {
        av_frame_free(&frame); avcodec_free_context(&ctx); avformat_close_input(&fmt);
        return false;
    }

    // 原尺寸直接用（水印一般不随画布缩放；若后续需要可以在这里加 sws_scale）
    const int w = frame->width;
    const int h = frame->height;
    // 偶数化 —— UV 对齐
    const int ew = w & ~1;
    const int eh = h & ~1;
    SwsContext* sws = sws_getContext(frame->width, frame->height,
                                     static_cast<AVPixelFormat>(frame->format),
                                     ew, eh, AV_PIX_FMT_BGRA,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws) {
        av_frame_free(&frame); avcodec_free_context(&ctx); avformat_close_input(&fmt);
        return false;
    }
    std::vector<uint8_t> bgra(static_cast<size_t>(ew) * eh * 4);
    uint8_t* dst[4] = {bgra.data(), nullptr, nullptr, nullptr};
    int dstLines[4] = {ew * 4, 0, 0, 0};
    sws_scale(sws, frame->data, frame->linesize, 0, frame->height, dst, dstLines);
    sws_freeContext(sws);
    av_frame_free(&frame);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);

    out.width = ew;
    out.height = eh;
    out.yPlane = std::shared_ptr<uint8_t>(new uint8_t[static_cast<size_t>(ew) * eh],
                                          std::default_delete<uint8_t[]>());
    out.uvPlane = std::shared_ptr<uint8_t>(new uint8_t[static_cast<size_t>(ew) * eh / 2],
                                           std::default_delete<uint8_t[]>());
    out.alpha = std::shared_ptr<uint8_t>(new uint8_t[static_cast<size_t>(ew) * eh],
                                         std::default_delete<uint8_t[]>());
    BGRAToNv12Alpha(bgra.data(), ew, eh, ew * 4, out.yPlane.get(), out.uvPlane.get(), out.alpha.get());

    out.opacity = std::clamp(opacity, 0.0f, 1.0f);
    out.position = position;
    out.rawX = offX;
    out.rawY = offY;
    ResolvePosition(position, offX, offY, canvasW, canvasH, ew, eh, out.offsetX, out.offsetY);
    return true;
}

int Watermark::Apply(const WatermarkParams& params, int canvasW, int canvasH)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    switch (params.type) {
        case LogoOpType::LOGO_RM: {
            const auto it = m_layers.find(params.index);
            if (it == m_layers.end()) return 0;
            m_layers.erase(it);
            TG_LOGI("Watermark: 移除 index=%u", params.index);
            return 0;
        }
        case LogoOpType::LOGO_ADD:
        case LogoOpType::LOGO_UPDATE:
        case LogoOpType::LOGO_INIT: {
            if (params.url.empty()) {
                TG_LOGW("Watermark: url 为空 index=%u", params.index);
                return -1;
            }
            WatermarkTexture tex;
            tex.index = params.index;
            if (!DecodeImage(params.url, canvasW, canvasH, tex,
                             1.0f /* opacity: WatermarkParams 当前接口未显式携带，默认不透明 */,
                             params.position, params.offset_x, params.offset_y)) {
                return -2;
            }
            m_layers[params.index] = std::move(tex);
            TG_LOGI("Watermark: %s index=%u url=%s @%d,%d %dx%d",
                    params.type == LogoOpType::LOGO_UPDATE ? "更新" : "添加",
                    params.index, params.url.c_str(),
                    m_layers[params.index].offsetX, m_layers[params.index].offsetY,
                    m_layers[params.index].width, m_layers[params.index].height);
            return 0;
        }
    }
    return 0;
}

void Watermark::Clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_layers.clear();
}

void Watermark::BlendOnto(Frame& nv12) const
{
    if (nv12.pixFmt != AV_PIX_FMT_NV12 || !nv12.planes[0] || !nv12.planes[1]) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_layers.empty()) return;
    uint8_t* yDst = nv12.planes[0].get();
    uint8_t* uvDst = nv12.planes[1].get();
    const int dstW = nv12.width, dstH = nv12.height;
    const int sY = nv12.strides[0] > 0 ? nv12.strides[0] : dstW;
    const int sUV = nv12.strides[1] > 0 ? nv12.strides[1] : dstW;

    for (const auto& [idx, tex] : m_layers) {
        if (!tex.yPlane || !tex.uvPlane || !tex.alpha) continue;
        int offX = 0;
        int offY = 0;
        ResolvePosition(tex.position, tex.rawX, tex.rawY, dstW, dstH, tex.width, tex.height, offX, offY);
        const int x0 = std::max(0, offX);
        const int y0 = std::max(0, offY);
        const int x1 = std::min(dstW, offX + tex.width);
        const int y1 = std::min(dstH, offY + tex.height);
        if (x0 >= x1 || y0 >= y1) continue;

        // Y 平面（全分辨率）
        for (int y = y0; y < y1; ++y) {
            const int ly = y - offY;
            for (int x = x0; x < x1; ++x) {
                const int lx = x - offX;
                const int a = static_cast<int>(tex.alpha.get()[ly * tex.width + lx]) *
                              static_cast<int>(tex.opacity * 255) / 255;
                const int yl = tex.yPlane.get()[ly * tex.width + lx];
                const int ys = yDst[y * sY + x];
                yDst[y * sY + x] = static_cast<uint8_t>((yl * a + ys * (255 - a)) / 255);
            }
        }
        // UV 平面（半分辨率；按 2x2 块中第一个样本的 alpha 近似）
        const int x0u = x0 & ~1, y0u = y0 & ~1;
        const int x1u = x1 & ~1, y1u = y1 & ~1;
        for (int y = y0u; y < y1u; y += 2) {
            const int ly = (y - (offY & ~1)) / 2;
            for (int x = x0u; x < x1u; x += 2) {
                const int lx = (x - (offX & ~1)) / 2;
                const int a = static_cast<int>(tex.alpha.get()[(ly * 2) * tex.width + (lx * 2)]) *
                              static_cast<int>(tex.opacity * 255) / 255;
                const int ul = tex.uvPlane.get()[ly * tex.width + lx * 2];
                const int vl = tex.uvPlane.get()[ly * tex.width + lx * 2 + 1];
                const int us = uvDst[(y / 2) * sUV + (x / 2) * 2];
                const int vs = uvDst[(y / 2) * sUV + (x / 2) * 2 + 1];
                uvDst[(y / 2) * sUV + (x / 2) * 2]     = static_cast<uint8_t>((ul * a + us * (255 - a)) / 255);
                uvDst[(y / 2) * sUV + (x / 2) * 2 + 1] = static_cast<uint8_t>((vl * a + vs * (255 - a)) / 255);
            }
        }
    }
}

}  // namespace TGCapture::core
