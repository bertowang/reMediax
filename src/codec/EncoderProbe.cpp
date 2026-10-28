#include "codec/EncoderProbe.h"

#include <chrono>
#include <map>
#include <mutex>

#include "core/AvError.h"
#include "core/Logger.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/pixdesc.h>
}

namespace TGCapture::codec
{

namespace
{

// 候选列表即"TGCapture 认识的编码器"，顺序就是推荐顺序。
// 视频：各平台系统硬编在前，软编兜底；音频：fdk 音质最好，系统 AAC 次之，ffmpeg 内置 aac 兜底。
const char* const kVideoCandidates[] = {
    "h264_videotoolbox",   // iOS / macOS
    "h264_mediacodec",     // Android
    "h264_nvenc",          // Windows：NVIDIA
    "h264_amf",            // Windows：AMD
    "h264_qsv",            // Windows：Intel
    "h264_mf",             // Windows：Media Foundation（系统自带，兜底的硬编）
    "libopenh264",
    "libx264",
};
const char* const kAudioCandidates[] = {"libfdk_aac", "aac_at", "aac"};

// 软件像素格式：优先 NV12（硬编友好），其次 YUV420P，再不行取第一个非硬件格式
AVPixelFormat PickSoftwarePixFmt(const AVCodec* codec)
{
    if (!codec->pix_fmts) {
        return AV_PIX_FMT_YUV420P;
    }
    AVPixelFormat firstSw = AV_PIX_FMT_NONE;
    bool hasYuv420p = false;
    for (const AVPixelFormat* f = codec->pix_fmts; *f != AV_PIX_FMT_NONE; ++f) {
        if (*f == AV_PIX_FMT_NV12) {
            return *f;
        }
        hasYuv420p = hasYuv420p || *f == AV_PIX_FMT_YUV420P;
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(*f);
        if (firstSw == AV_PIX_FMT_NONE && desc && !(desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
            firstSw = *f;
        }
    }
    return hasYuv420p ? AV_PIX_FMT_YUV420P : firstSw;
}

bool OpenOnce(const AVCodec* codec)
{
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (!ctx) {
        return false;
    }
    if (codec->type == AVMEDIA_TYPE_VIDEO) {
        // 640x480：宽高都是 16 的倍数（部分 MediaCodec 实现对非对齐尺寸直接拒绝），且所有硬编都支持
        ctx->width = 640;
        ctx->height = 480;
        ctx->time_base = AVRational{1, 30};
        ctx->framerate = AVRational{30, 1};
        ctx->gop_size = 30;
        ctx->max_b_frames = 0;
        ctx->bit_rate = 1000000;
        ctx->pix_fmt = PickSoftwarePixFmt(codec);
    } else {
        ctx->sample_rate = 44100;
        av_channel_layout_default(&ctx->ch_layout, 2);
        ctx->sample_fmt = codec->sample_fmts ? codec->sample_fmts[0] : AV_SAMPLE_FMT_S16;
        ctx->bit_rate = 128000;
        ctx->time_base = AVRational{1, 44100};
    }
    const int ret = (ctx->pix_fmt == AV_PIX_FMT_NONE && codec->type == AVMEDIA_TYPE_VIDEO)
                        ? AVERROR(EINVAL)
                        : avcodec_open2(ctx, codec, nullptr);
    avcodec_free_context(&ctx);
    if (ret < 0) {
        TG_LOGI("EncoderProbe: %s 打开失败 %d (%s)", codec->name, ret, core::AvErr(ret));
    }
    return ret >= 0;
}

}  // namespace

bool IsHardwareVideoEncoder(const std::string& name)
{
    for (const char* suffix : {"_videotoolbox", "_mediacodec", "_nvenc", "_amf", "_qsv", "_mf", "_vaapi"}) {
        const size_t n = std::char_traits<char>::length(suffix);
        if (name.size() > n && name.compare(name.size() - n, n, suffix) == 0) {
            return true;
        }
    }
    return false;
}

bool ProbeEncoder(const std::string& name)
{
    if (name.empty()) {
        return false;
    }
    // 互斥锁覆盖整个探测：并发探测同一个硬编会各建一个会话，部分设备的硬编实例数很少（2~4 个），
    // 同时打开容易互相挤掉，得出"不可用"的假结论
    static std::mutex mutex;
    static std::map<std::string, bool> cache;
    std::lock_guard<std::mutex> lock(mutex);
    if (const auto it = cache.find(name); it != cache.end()) {
        return it->second;
    }
    const AVCodec* codec = avcodec_find_encoder_by_name(name.c_str());
    bool ok = false;
    if (!codec) {
        TG_LOGI("EncoderProbe: %s 未编进 ffmpeg", name.c_str());
    } else {
        const auto begin = std::chrono::steady_clock::now();
        ok = OpenOnce(codec);
        const auto costMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - begin)
                                .count();
        TG_LOGI("EncoderProbe: %s %s（%lldms）", name.c_str(), ok ? "可用" : "不可用",
                static_cast<long long>(costMs));
    }
    cache.emplace(name, ok);
    return ok;
}

std::vector<std::string> AvailableEncoders(bool video)
{
    std::vector<std::string> out;
    auto collect = [&out](const auto& candidates) {
        for (const char* name : candidates) {
            // 先做廉价的编译期检查，未编进的不必进探测（也不进缓存日志）
            if (avcodec_find_encoder_by_name(name) && ProbeEncoder(name)) {
                out.emplace_back(name);
            }
        }
    };
    if (video) {
        collect(kVideoCandidates);
    } else {
        collect(kAudioCandidates);
    }
    return out;
}

}  // namespace TGCapture::codec
