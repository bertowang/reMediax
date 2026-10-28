// 编码器探测（codec/EncoderProbe）与 VideoEncoder 硬编回落
//
// 硬编是否可用取决于运行环境，这里只断言与环境无关的部分：
//   - 名字分类、未编进的编码器判不可用、ffmpeg 内置 aac 一定可用
//   - "硬编打不开 → 回落 libopenh264"用一个带硬编后缀的假名字触发（任何环境都找不到它）
#include <algorithm>
#include <string>

#include "codec/EncoderProbe.h"
#include "codec/VideoEncoder.h"
#include "core/MediaTypes.h"
#include "tg_test.h"

extern "C"
{
#include <libavcodec/avcodec.h>
}

using namespace TGCapture;

namespace
{

core::VideoMediaType Nv12Type(int w, int h)
{
    core::VideoMediaType type;
    type.SetWidth(w);
    type.SetHeight(h);
    type.SetPixelFormat(AV_PIX_FMT_NV12);
    type.SetFrameRate(30, 1);
    return type;
}

VideoEncoderParams SmallParams(const std::string& codec)
{
    VideoEncoderParams params;
    params.width = 320;
    params.height = 240;
    params.fps = 30.0f;
    params.gop = 30;
    params.vBitrate = 500000;
    params.vCodec = codec;
    return params;
}

bool Contains(const std::vector<std::string>& list, const std::string& name)
{
    return std::find(list.begin(), list.end(), name) != list.end();
}

}  // namespace

TG_TEST(EncoderProbe, ClassifiesHardwareVideoEncoders)
{
    for (const char* hw : {"h264_videotoolbox", "h264_mediacodec", "h264_nvenc", "h264_amf", "h264_qsv", "h264_mf",
                           "hevc_videotoolbox"}) {
        TG_EXPECT(codec::IsHardwareVideoEncoder(hw));
    }
    for (const char* sw : {"libopenh264", "libx264", "aac", "libfdk_aac", "", "_mf", "mediacodec"}) {
        TG_EXPECT(!codec::IsHardwareVideoEncoder(sw));
    }
}

TG_TEST(EncoderProbe, UnknownEncoderIsUnavailable)
{
    TG_EXPECT(!codec::ProbeEncoder("tg_no_such_encoder"));
    TG_EXPECT(!codec::ProbeEncoder("tg_no_such_encoder"));   // 第二次走缓存，结论不变
    TG_EXPECT(!codec::ProbeEncoder(""));
}

// ffmpeg 内置 aac 是音频回落的兜底，必须能真正打开
TG_TEST(EncoderProbe, BuiltinAacIsAvailable)
{
    TG_EXPECT(codec::ProbeEncoder("aac"));
    const auto audio = codec::AvailableEncoders(false);
    TG_EXPECT(Contains(audio, "aac"));
    TG_EXPECT_EQ(audio.back(), std::string("aac"));   // 推荐顺序里兜底排最后
    for (const auto& name : audio) {
        TG_EXPECT(avcodec_find_encoder_by_name(name.c_str()) != nullptr);
    }
}

// 视频列表只含编进来且能打开的；libopenh264 编进来了就必须在列表里（软编不依赖设备）
TG_TEST(EncoderProbe, VideoListMatchesBuild)
{
    const auto video = codec::AvailableEncoders(true);
    for (const auto& name : video) {
        TG_EXPECT(codec::ProbeEncoder(name));
    }
    if (avcodec_find_encoder_by_name(codec::kSoftwareVideoEncoder)) {
        TG_EXPECT(Contains(video, codec::kSoftwareVideoEncoder));
    }
}

// 请求的硬编在本机打不开 → 回落 libopenh264，Open 返回成功、CodecName 反映实际编码器
TG_TEST(VideoEncoder, HardwareEncoderFallsBackToSoftware)
{
    if (!avcodec_find_encoder_by_name(codec::kSoftwareVideoEncoder)) {
        std::printf("    SKIP 当前 ffmpeg 未编进 %s\n", codec::kSoftwareVideoEncoder);
        return;
    }
    codec::VideoEncoder enc;
    TG_ASSERT(enc.Open(SmallParams("tg_fake_mediacodec"), Nv12Type(320, 240)) == 0);
    TG_EXPECT(enc.IsOpened());
    TG_EXPECT_EQ(enc.CodecName(), std::string(codec::kSoftwareVideoEncoder));
    TG_EXPECT_EQ(enc.LastAvError(), 0);   // 回落成功后不残留上一次失败的错误码
    enc.Close();
    TG_EXPECT(!enc.IsOpened());
}

// 软编本身打不开是配置错误：不回落，照常失败
TG_TEST(VideoEncoder, UnknownSoftwareEncoderFails)
{
    codec::VideoEncoder enc;
    TG_EXPECT(enc.Open(SmallParams("tg_no_such_video_encoder"), Nv12Type(320, 240)) < 0);
    TG_EXPECT(!enc.IsOpened());
}
