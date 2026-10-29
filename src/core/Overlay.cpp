#include "core/Overlay.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/AvError.h"
#include "core/Logger.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace TGCapture::core
{

namespace
{
inline uint8_t Clamp8(int v) { return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v)); }

inline uint8_t Mix(int s, int d, int a) { return static_cast<uint8_t>((s * a + d * (255 - a) + 127) / 255); }

inline int EvenFloor(int v) { return v >= 0 ? (v & ~1) : -((-v + 1) & ~1); }

// BT.601 limited range；色度取 2x2 平均
void BgraToNv12A(const uint8_t* bgra, int w, int h, bool alpha, Nv12AImage& out)
{
    out.width = w;
    out.height = h;
    out.y.assign(static_cast<size_t>(w) * h, 0);
    out.uv.assign(static_cast<size_t>(w) * h / 2, 128);
    out.a.clear();
    if (alpha) {
        out.a.assign(static_cast<size_t>(w) * h, 255);
    }
    for (int row = 0; row < h; ++row) {
        const uint8_t* p = bgra + static_cast<size_t>(row) * w * 4;
        for (int col = 0; col < w; ++col, p += 4) {
            out.y[static_cast<size_t>(row) * w + col] = Clamp8((66 * p[2] + 129 * p[1] + 25 * p[0] + 128) / 256 + 16);
            if (alpha) {
                out.a[static_cast<size_t>(row) * w + col] = p[3];
            }
        }
    }
    for (int row = 0; row < h; row += 2) {
        for (int col = 0; col < w; col += 2) {
            int r = 0, g = 0, b = 0;
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    const uint8_t* p = bgra + (static_cast<size_t>(row + dy) * w + col + dx) * 4;
                    b += p[0];
                    g += p[1];
                    r += p[2];
                }
            }
            r /= 4;
            g /= 4;
            b /= 4;
            uint8_t* uv = &out.uv[static_cast<size_t>(row / 2) * w + col];
            uv[0] = Clamp8((-38 * r - 74 * g + 112 * b + 128) / 256 + 128);
            uv[1] = Clamp8((112 * r - 94 * g - 18 * b + 128) / 256 + 128);
        }
    }
}

bool ValidId(const std::string& id)
{
    if (id.empty() || id.size() > 64) {
        return false;
    }
    return std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
               c == '-';
    });
}
}  // namespace

bool LoadImageNv12A(const std::string& path, Nv12AImage& out, std::string* why)
{
    auto fail = [&](const std::string& reason) {
        if (why) {
            *why = reason;
        }
        return false;
    };
    AVFormatContext* fmt = nullptr;
    int ret = avformat_open_input(&fmt, path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        return fail(std::string("打不开图片：") + AvErr(ret));
    }
    AVCodecContext* ctx = nullptr;
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    SwsContext* sws = nullptr;
    std::string reason;
    bool ok = false;
    do {
        if (avformat_find_stream_info(fmt, nullptr) < 0) {
            reason = "读取图片信息失败";
            break;
        }
        const AVCodec* dec = nullptr;
        const int idx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
        if (idx < 0 || !dec) {
            reason = "不是可解码的图片（当前只支持 png）";
            break;
        }
        ctx = avcodec_alloc_context3(dec);
        if (!ctx || avcodec_parameters_to_context(ctx, fmt->streams[idx]->codecpar) < 0 ||
            avcodec_open2(ctx, dec, nullptr) < 0) {
            reason = "图片解码器打开失败";
            break;
        }
        bool got = false;
        while (!got && av_read_frame(fmt, pkt) >= 0) {
            if (pkt->stream_index == idx && avcodec_send_packet(ctx, pkt) >= 0) {
                got = avcodec_receive_frame(ctx, frame) >= 0;
            }
            av_packet_unref(pkt);
        }
        if (!got) {
            avcodec_send_packet(ctx, nullptr);
            got = avcodec_receive_frame(ctx, frame) >= 0;
        }
        if (!got) {
            reason = "图片解码失败";
            break;
        }
        const int w = frame->width & ~1;
        const int h = frame->height & ~1;
        if (w < 2 || h < 2) {
            reason = "图片尺寸太小";
            break;
        }
        sws = sws_getContext(frame->width, frame->height, static_cast<AVPixelFormat>(frame->format), w, h,
                             AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws) {
            reason = "图片像素格式无法转换";
            break;
        }
        std::vector<uint8_t> bgra(static_cast<size_t>(w) * h * 4);
        uint8_t* dst[4] = {bgra.data(), nullptr, nullptr, nullptr};
        int lines[4] = {w * 4, 0, 0, 0};
        sws_scale(sws, frame->data, frame->linesize, 0, frame->height, dst, lines);
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame->format));
        const bool alpha = desc && (desc->flags & AV_PIX_FMT_FLAG_ALPHA);
        BgraToNv12A(bgra.data(), w, h, alpha, out);
        ok = true;
    } while (false);
    sws_freeContext(sws);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
    return ok ? true : fail(reason);
}

void BlendNv12A(Frame& dst, int x, int y, const Nv12AView& src, int opacity)
{
    if (dst.pixFmt != AV_PIX_FMT_NV12 || !dst.planes[0] || !dst.planes[1] || !src.y || !src.uv || opacity <= 0) {
        return;
    }
    opacity = std::min(opacity, 255);
    x = EvenFloor(x);
    y = EvenFloor(y);
    const int dstW = dst.width & ~1;
    const int dstH = dst.height & ~1;
    const int x0 = std::max(0, x);
    const int y0 = std::max(0, y);
    const int x1 = std::min(dstW, x + (src.width & ~1));
    const int y1 = std::min(dstH, y + (src.height & ~1));
    if (x0 >= x1 || y0 >= y1) {
        return;
    }
    uint8_t* yDst = dst.planes[0].get();
    uint8_t* uvDst = dst.planes[1].get();
    const int sY = dst.strides[0] > 0 ? dst.strides[0] : dst.width;
    const int sUV = dst.strides[1] > 0 ? dst.strides[1] : dst.width;
    const bool opaque = !src.a && opacity == 255;

    for (int row = y0; row < y1; ++row) {
        const uint8_t* s = src.y + static_cast<size_t>(row - y) * src.yStride + (x0 - x);
        uint8_t* d = yDst + static_cast<size_t>(row) * sY + x0;
        if (opaque) {
            std::memcpy(d, s, static_cast<size_t>(x1 - x0));
            continue;
        }
        const uint8_t* a = src.a ? src.a + static_cast<size_t>(row - y) * src.aStride + (x0 - x) : nullptr;
        for (int i = 0; i < x1 - x0; ++i) {
            const int alpha = a ? a[i] * opacity / 255 : opacity;
            if (alpha > 0) {
                d[i] = alpha >= 255 ? s[i] : Mix(s[i], d[i], alpha);
            }
        }
    }
    // UV：半分辨率，alpha 取 2x2 块左上角
    for (int row = y0; row < y1; row += 2) {
        const int srow = row - y;
        const uint8_t* s = src.uv + static_cast<size_t>(srow / 2) * src.uvStride + (x0 - x);
        uint8_t* d = uvDst + static_cast<size_t>(row / 2) * sUV + x0;
        if (opaque) {
            std::memcpy(d, s, static_cast<size_t>(x1 - x0));
            continue;
        }
        const uint8_t* a = src.a ? src.a + static_cast<size_t>(srow) * src.aStride + (x0 - x) : nullptr;
        for (int i = 0; i < x1 - x0; i += 2) {
            const int alpha = a ? a[i] * opacity / 255 : opacity;
            if (alpha > 0) {
                if (alpha >= 255) {
                    d[i] = s[i];
                    d[i + 1] = s[i + 1];
                } else {
                    d[i] = Mix(s[i], d[i], alpha);
                    d[i + 1] = Mix(s[i + 1], d[i + 1], alpha);
                }
            }
        }
    }
}

PixelRect ResolveOverlayRect(const OverlayDesc& desc, int srcWidth, int srcHeight, int canvasWidth, int canvasHeight)
{
    PixelRect r;
    r.x = EvenFloor(static_cast<int>(std::lround(desc.x * canvasWidth)));
    r.y = EvenFloor(static_cast<int>(std::lround(desc.y * canvasHeight)));
    r.w = static_cast<int>(std::lround(desc.width * canvasWidth)) & ~1;
    if (desc.height > 0) {
        r.h = static_cast<int>(std::lround(desc.height * canvasHeight)) & ~1;
    } else if (srcWidth > 0 && srcHeight > 0) {
        r.h = static_cast<int>(std::lround(static_cast<double>(r.w) * srcHeight / srcWidth)) & ~1;
    } else {
        r.h = 0;
    }
    return r;
}

bool ValidateOverlayDesc(const OverlayDesc& d, std::string* why)
{
    auto fail = [&](const char* reason) {
        if (why) {
            *why = reason;
        }
        return false;
    };
    if (!ValidId(d.id)) {
        return fail("id 非法（1 ~ 64 个字母数字 . _ -）");
    }
    if (d.kind != OverlayKind::Image && d.kind != OverlayKind::VideoFile) {
        return fail("kind 非法");
    }
    if (d.path.empty()) {
        return fail("path 为空");
    }
    if (!(d.x >= -1.0f && d.x <= 1.0f) || !(d.y >= -1.0f && d.y <= 1.0f)) {
        return fail("x / y 越界（-1 ~ 1）");
    }
    if (!(d.width > 0.0f && d.width <= 2.0f) || !(d.height >= 0.0f && d.height <= 2.0f)) {
        return fail("width / height 越界（width (0, 2]，height [0, 2]）");
    }
    if (!(d.opacity >= 0.0f && d.opacity <= 1.0f)) {
        return fail("opacity 越界（0 ~ 1）");
    }
    return true;
}

bool SameOverlayDesc(const OverlayDesc& a, const OverlayDesc& b)
{
    return a.id == b.id && a.kind == b.kind && a.path == b.path && a.loop == b.loop && a.x == b.x && a.y == b.y &&
           a.width == b.width && a.height == b.height && a.opacity == b.opacity && a.zOrder == b.zOrder &&
           a.visible == b.visible;
}

}  // namespace TGCapture::core
