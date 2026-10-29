// 测试用的媒体素材：最小 PNG 编码（不依赖 zlib）、H.264 测试视频（libopenh264 → flv）、视频轨解码
#ifndef TGCAPTURE_TESTS_TEST_MEDIA_H
#define TGCAPTURE_TESTS_TEST_MEDIA_H

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace tgmedia
{

// ---- 最小 PNG 编码（8bit RGB / RGBA、deflate 存储块、不压缩）----
inline uint32_t Crc32(const uint8_t* data, size_t n, uint32_t crc = 0)
{
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) {
        crc ^= data[i];
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

inline void PutBe32(std::vector<uint8_t>& out, uint32_t v)
{
    for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<uint8_t>(v >> s));
}

inline void PutChunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& body)
{
    PutBe32(out, static_cast<uint32_t>(body.size()));
    std::vector<uint8_t> tb(type, type + 4);
    tb.insert(tb.end(), body.begin(), body.end());
    out.insert(out.end(), tb.begin(), tb.end());
    PutBe32(out, Crc32(tb.data(), tb.size()));
}

// pixel(x, y, rgba[4])；alpha = false 时写 RGB（第 4 个分量忽略）
inline bool WritePng(const std::string& path, int w, int h, bool alpha,
                     const std::function<void(int, int, uint8_t*)>& pixel)
{
    std::vector<uint8_t> raw;
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);   // filter none
        for (int x = 0; x < w; ++x) {
            uint8_t p[4] = {0, 0, 0, 255};
            pixel(x, y, p);
            raw.insert(raw.end(), p, p + (alpha ? 4 : 3));
        }
    }
    std::vector<uint8_t> z{0x78, 0x01};
    size_t pos = 0;
    while (pos < raw.size()) {
        const size_t len = std::min<size_t>(65535, raw.size() - pos);
        z.push_back(pos + len == raw.size() ? 1 : 0);
        z.push_back(static_cast<uint8_t>(len));
        z.push_back(static_cast<uint8_t>(len >> 8));
        z.push_back(static_cast<uint8_t>(~len));
        z.push_back(static_cast<uint8_t>(~len >> 8));
        z.insert(z.end(), raw.begin() + static_cast<long>(pos), raw.begin() + static_cast<long>(pos + len));
        pos += len;
    }
    uint32_t a = 1, bsum = 0;
    for (uint8_t c : raw) {
        a = (a + c) % 65521;
        bsum = (bsum + a) % 65521;
    }
    PutBe32(z, (bsum << 16) | a);

    std::vector<uint8_t> png{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8_t> ihdr;
    PutBe32(ihdr, static_cast<uint32_t>(w));
    PutBe32(ihdr, static_cast<uint32_t>(h));
    ihdr.insert(ihdr.end(), {8, static_cast<uint8_t>(alpha ? 6 : 2), 0, 0, 0});
    PutChunk(png, "IHDR", ihdr);
    PutChunk(png, "IDAT", z);
    PutChunk(png, "IEND", {});
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    return f.good();
}

inline bool WriteSolidPng(const std::string& path, int w, int h, uint8_t r, uint8_t g, uint8_t b)
{
    return WritePng(path, w, h, false, [&](int, int, uint8_t* p) {
        p[0] = r;
        p[1] = g;
        p[2] = b;
    });
}

// ---- H.264 测试视频：每帧整幅纯色，亮度 lumaOf(i)，flv 封装（libopenh264 编码）----
inline bool WriteTestVideo(const std::string& path, int w, int h, int fps, int frames,
                           const std::function<int(int)>& lumaOf)
{
    const AVCodec* codec = avcodec_find_encoder_by_name("libopenh264");
    if (!codec) return false;
    AVFormatContext* fmt = nullptr;
    if (avformat_alloc_output_context2(&fmt, nullptr, "flv", path.c_str()) < 0) return false;
    AVCodecContext* enc = avcodec_alloc_context3(codec);
    enc->width = w;
    enc->height = h;
    enc->pix_fmt = AV_PIX_FMT_YUV420P;
    enc->time_base = AVRational{1, fps};
    enc->framerate = AVRational{fps, 1};
    enc->bit_rate = 800000;
    enc->gop_size = fps;
    enc->max_b_frames = 0;
    enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    bool ok = avcodec_open2(enc, codec, nullptr) >= 0;
    AVStream* st = ok ? avformat_new_stream(fmt, nullptr) : nullptr;
    ok = ok && st && avcodec_parameters_from_context(st->codecpar, enc) >= 0;
    if (ok) {
        st->time_base = enc->time_base;
        st->avg_frame_rate = enc->framerate;
    }
    ok = ok && avio_open(&fmt->pb, path.c_str(), AVIO_FLAG_WRITE) >= 0;
    ok = ok && avformat_write_header(fmt, nullptr) >= 0;
    AVFrame* frame = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();
    auto drain = [&] {
        while (avcodec_receive_packet(enc, pkt) == 0) {
            av_packet_rescale_ts(pkt, enc->time_base, st->time_base);
            pkt->stream_index = st->index;
            av_interleaved_write_frame(fmt, pkt);
        }
    };
    for (int i = 0; ok && i < frames; ++i) {
        frame->format = enc->pix_fmt;
        frame->width = w;
        frame->height = h;
        ok = av_frame_get_buffer(frame, 0) >= 0;
        if (!ok) break;
        const int luma = lumaOf(i);
        for (int y = 0; y < h; ++y) std::fill_n(frame->data[0] + y * frame->linesize[0], w, static_cast<uint8_t>(luma));
        for (int y = 0; y < h / 2; ++y) {
            std::fill_n(frame->data[1] + y * frame->linesize[1], w / 2, uint8_t{128});
            std::fill_n(frame->data[2] + y * frame->linesize[2], w / 2, uint8_t{128});
        }
        frame->pts = i;
        frame->pict_type = AV_PICTURE_TYPE_NONE;
        ok = avcodec_send_frame(enc, frame) >= 0;
        av_frame_unref(frame);
        drain();
    }
    if (ok) {
        avcodec_send_frame(enc, nullptr);
        drain();
        av_write_trailer(fmt);
    }
    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&enc);
    if (fmt->pb) avio_closep(&fmt->pb);
    avformat_free_context(fmt);
    return ok;
}

// ---- 解码文件的视频轨：每帧转成灰度（Y 平面）便于取样 ----
struct DecodedFrame
{
    int64_t ptsMs = 0;
    int width = 0;
    int height = 0;
    std::vector<uint8_t> y;   // width * height

    // (fx, fy) 处 size×size 方块的平均亮度，fx / fy 为相对坐标 0 ~ 1
    [[nodiscard]] double LumaAt(double fx, double fy, int size = 4) const
    {
        const int cx = std::clamp(static_cast<int>(fx * width), 0, width - size);
        const int cy = std::clamp(static_cast<int>(fy * height), 0, height - size);
        double acc = 0;
        for (int yy = cy; yy < cy + size; ++yy)
            for (int xx = cx; xx < cx + size; ++xx) acc += y[static_cast<size_t>(yy) * width + xx];
        return acc / (size * size);
    }
};

inline std::vector<DecodedFrame> DecodeVideo(const std::string& path)
{
    std::vector<DecodedFrame> out;
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) return out;
    avformat_find_stream_info(fmt, nullptr);
    const AVCodec* codec = nullptr;
    const int idx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (idx < 0 || !codec) {
        avformat_close_input(&fmt);
        return out;
    }
    AVCodecContext* dec = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(dec, fmt->streams[idx]->codecpar);
    avcodec_open2(dec, codec, nullptr);
    const AVRational tb = fmt->streams[idx]->time_base;
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    auto drain = [&] {
        while (avcodec_receive_frame(dec, frame) >= 0) {
            DecodedFrame d;
            d.width = frame->width;
            d.height = frame->height;
            d.ptsMs = frame->best_effort_timestamp == AV_NOPTS_VALUE
                          ? 0
                          : av_rescale_q(frame->best_effort_timestamp, tb, AVRational{1, 1000});
            d.y.resize(static_cast<size_t>(d.width) * d.height);
            // H.264 解出来是 yuv420p：直接取 Y 平面（不经 sws，避免 limited → full range 的换算改变数值）
            for (int row = 0; row < d.height; ++row) {
                std::copy_n(frame->data[0] + static_cast<size_t>(row) * frame->linesize[0], d.width,
                            d.y.data() + static_cast<size_t>(row) * d.width);
            }
            out.push_back(std::move(d));
            av_frame_unref(frame);
        }
    };
    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == idx) {
            avcodec_send_packet(dec, pkt);
            drain();
        }
        av_packet_unref(pkt);
    }
    avcodec_send_packet(dec, nullptr);
    drain();
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
    return out;
}

}  // namespace tgmedia

#endif  // TGCAPTURE_TESTS_TEST_MEDIA_H
