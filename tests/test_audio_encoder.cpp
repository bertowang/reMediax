// AudioEncoder：采样格式适配（R1 回归）
//
// 混流器输出恒为交错 S16。指定的编码器不可用而回退到 ffmpeg 内置 aac（只收 FLTP）时，
// 曾把 int16 位模式直接当 float 写进 FLTP 平面 → "Input contains NaN/+-Inf"，
// 编码持续失败 1s 后上报 TGErr_AudioEncodeFailed(-103)，文件 / 推流里没有音频。
// 这里走真实的 ffmpeg 编码 + 解码，验证回退路径产出的音频包可解码、且波形没有被破坏。
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "codec/AudioEncoder.h"
#include "core/Bus.h"
#include "core/Frame.h"
#include "core/MediaTypes.h"
#include "tg_test.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
}

using namespace TGCapture;
using namespace TGCapture::core;

namespace
{

constexpr int kRate = 44100;
constexpr int kCh = 2;
constexpr int kChunk = 1024;
constexpr int kChunks = 43;            // ≈ 1s
constexpr int16_t kAmplitude = 8000;   // 正弦峰值（int16）
constexpr double kPi = 3.14159265358979323846;   // M_PI 在 MSVC 下要 _USE_MATH_DEFINES，不依赖它

// 链尾：收集编码器 Forward 出来的音频包
class PacketSink : public BusNode
{
public:
    PacketSink() : BusNode("test_sink") {}
    bool Submit(FramePtr frame) override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_packets.push_back(std::move(frame));
        return true;
    }
    [[nodiscard]] size_t Pending() const override { return 0; }
    std::vector<FramePtr> Take()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return std::move(m_packets);
    }

private:
    std::mutex m_mutex;
    std::vector<FramePtr> m_packets;
};

AudioMediaType S16Type()
{
    AudioMediaType type;
    type.SetSampleRate(kRate);
    type.SetChannels(kCh);
    type.SetSampleFormat(AV_SAMPLE_FMT_S16);
    type.SetFrameSize(kChunk);
    return type;
}

FramePtr SineChunk(int index)
{
    const size_t samples = static_cast<size_t>(kChunk) * kCh;
    std::shared_ptr<uint8_t> data(new uint8_t[samples * sizeof(int16_t)], std::default_delete<uint8_t[]>());
    auto* pcm = reinterpret_cast<int16_t*>(data.get());
    for (int i = 0; i < kChunk; ++i) {
        const double t = static_cast<double>(index * kChunk + i) / kRate;
        const auto v = static_cast<int16_t>(kAmplitude * std::sin(2.0 * kPi * 440.0 * t));
        for (int c = 0; c < kCh; ++c) {
            pcm[static_cast<size_t>(i) * kCh + c] = v;
        }
    }
    auto frame = std::make_shared<Frame>();
    frame->kind = MediaKind::Audio;
    frame->sampleRate = kRate;
    frame->channels = kCh;
    frame->audioData = std::move(data);
    frame->audioBytes = samples * sizeof(int16_t);
    frame->ptsUs = static_cast<int64_t>(index) * kChunk * 1000000LL / kRate;
    return frame;
}

struct EncodeResult
{
    std::vector<FramePtr> packets;
    uint64_t encoded = 0;
    int32_t lastAvError = 0;
};

// 喂 ~1s 正弦，等编码线程消费完再停（Stop 会 Drain 编码器）
EncodeResult EncodeSine(const std::shared_ptr<codec::AudioEncoder>& enc, const std::shared_ptr<PacketSink>& sink)
{
    TG_ASSERT(enc->Start());
    for (int i = 0; i < kChunks; ++i) {
        TG_ASSERT(enc->Submit(SineChunk(i)));
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (enc->EncodedFrames() < static_cast<uint64_t>(kChunks) && enc->LastAvError() == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    enc->Stop();
    EncodeResult result;
    result.encoded = enc->EncodedFrames();
    result.lastAvError = enc->LastAvError();
    result.packets = sink->Take();
    return result;
}

// 用 ffmpeg 内置 aac 解码器把包解回 PCM（float），统计 NaN/Inf 与 RMS
struct DecodeStats
{
    size_t samples = 0;
    size_t nonFinite = 0;
    double rms = 0.0;
};

DecodeStats DecodeAll(const AVCodecContext* encCtx, const std::vector<FramePtr>& packets)
{
    DecodeStats stats;
    const AVCodec* decoder = avcodec_find_decoder(AV_CODEC_ID_AAC);
    TG_ASSERT(decoder != nullptr);
    AVCodecContext* dec = avcodec_alloc_context3(decoder);
    TG_ASSERT(dec != nullptr);
    dec->sample_rate = encCtx->sample_rate;
    av_channel_layout_copy(&dec->ch_layout, &encCtx->ch_layout);
    if (encCtx->extradata_size > 0) {
        dec->extradata = static_cast<uint8_t*>(av_mallocz(static_cast<size_t>(encCtx->extradata_size) +
                                                          AV_INPUT_BUFFER_PADDING_SIZE));
        std::memcpy(dec->extradata, encCtx->extradata, static_cast<size_t>(encCtx->extradata_size));
        dec->extradata_size = encCtx->extradata_size;
    }
    TG_ASSERT(avcodec_open2(dec, decoder, nullptr) >= 0);

    double sumSq = 0.0;
    // 跳过编码器起始的 priming 段（AAC 前 2048 个样本左右是过渡）
    size_t skip = 2048;
    AVFrame* out = av_frame_alloc();
    for (const auto& pkt : packets) {
        if (!pkt || !pkt->packet) {
            continue;
        }
        if (avcodec_send_packet(dec, pkt->packet.get()) < 0) {
            continue;
        }
        while (avcodec_receive_frame(dec, out) >= 0) {
            const int ch = out->ch_layout.nb_channels;
            for (int c = 0; c < ch; ++c) {
                const auto* plane = reinterpret_cast<const float*>(out->extended_data[c]);
                for (int i = 0; i < out->nb_samples; ++i) {
                    const float v = (out->format == AV_SAMPLE_FMT_FLTP) ? plane[i] : 0.0f;
                    if (!std::isfinite(v)) {
                        ++stats.nonFinite;
                        continue;
                    }
                    if (c == 0 && skip > 0) {
                        --skip;
                        continue;
                    }
                    sumSq += static_cast<double>(v) * v;
                    ++stats.samples;
                }
            }
            av_frame_unref(out);
        }
    }
    av_frame_free(&out);
    avcodec_free_context(&dec);
    stats.rms = stats.samples ? std::sqrt(sumSq / static_cast<double>(stats.samples)) : 0.0;
    return stats;
}

}  // namespace

// 指定编码器不存在 → 回退内置 aac（FLTP）：必须经 S16→FLTP 转换，产出可解码且波形正确的音频
TG_TEST(AudioEncoder, FallbackToBuiltinAacConvertsS16ToFltp)
{
    auto enc = std::make_shared<codec::AudioEncoder>(kChunks + 8);
    auto sink = std::make_shared<PacketSink>();
    enc->LinkNext(sink);

    AudioEncoderParams params;
    params.sampleRate = kRate;
    params.channels = kCh;
    params.aBitrate = 128000;
    params.aCodec = "tg_no_such_audio_encoder";
    TG_ASSERT(enc->Open(params, S16Type()) == 0);
    TG_EXPECT_EQ(enc->CodecName(), std::string("aac"));
    TG_ASSERT(enc->CodecContext() != nullptr);
    TG_EXPECT_EQ(static_cast<int>(enc->CodecContext()->sample_fmt), static_cast<int>(AV_SAMPLE_FMT_FLTP));

    const auto result = EncodeSine(enc, sink);
    TG_EXPECT_EQ(result.lastAvError, 0);             // 修复前：AVERROR(EINVAL)（Input contains NaN）
    TG_EXPECT_EQ(result.encoded, static_cast<uint64_t>(kChunks));
    TG_ASSERT(result.packets.size() >= static_cast<size_t>(kChunks - 2));

    const auto stats = DecodeAll(enc->CodecContext(), result.packets);
    TG_EXPECT_EQ(stats.nonFinite, static_cast<size_t>(0));
    TG_ASSERT(stats.samples > 0);
    // 正弦 RMS = 峰值/√2；归一化到 float [-1,1]。有损编码允许 ±20% 误差，
    // 格式错位（int16 位模式当 float）会是 NaN 或量级完全不对，远超这个范围
    const double expected = (static_cast<double>(kAmplitude) / 32768.0) / std::sqrt(2.0);
    TG_EXPECT(std::fabs(stats.rms - expected) < expected * 0.2);
}

// 编码器本身收 S16（libfdk_aac / macOS 的 aac_at）：不应引入转换，照样正常出包。
// 当前 ffmpeg 里一个都没编进来就跳过
TG_TEST(AudioEncoder, NativeS16EncoderNeedsNoConversion)
{
    std::string name;
    for (const char* candidate : {"libfdk_aac", "aac_at"}) {
        const AVCodec* codec = avcodec_find_encoder_by_name(candidate);
        if (!codec || !codec->sample_fmts) {
            continue;
        }
        for (const AVSampleFormat* f = codec->sample_fmts; *f != AV_SAMPLE_FMT_NONE; ++f) {
            if (*f == AV_SAMPLE_FMT_S16) {
                name = candidate;
                break;
            }
        }
        if (!name.empty()) {
            break;
        }
    }
    if (name.empty()) {
        std::printf("    SKIP 当前 ffmpeg 没有收 S16 的 AAC 编码器（libfdk_aac / aac_at）\n");
        return;
    }
    std::printf("    使用 %s\n", name.c_str());
    auto enc = std::make_shared<codec::AudioEncoder>(kChunks + 8);
    auto sink = std::make_shared<PacketSink>();
    enc->LinkNext(sink);

    AudioEncoderParams params;
    params.sampleRate = kRate;
    params.channels = kCh;
    params.aCodec = name;
    TG_ASSERT(enc->Open(params, S16Type()) == 0);
    TG_EXPECT_EQ(enc->CodecName(), name);
    TG_EXPECT_EQ(static_cast<int>(enc->CodecContext()->sample_fmt), static_cast<int>(AV_SAMPLE_FMT_S16));

    const auto result = EncodeSine(enc, sink);
    TG_EXPECT_EQ(result.lastAvError, 0);
    TG_EXPECT_EQ(result.encoded, static_cast<uint64_t>(kChunks));
    TG_EXPECT(!result.packets.empty());
}

// 输入不是交错 S16：编码器内部按 int16 累积，直接拒绝，而不是开起来再静默产出垃圾
TG_TEST(AudioEncoder, RejectsNonS16Input)
{
    codec::AudioEncoder enc;
    AudioEncoderParams params;
    params.sampleRate = kRate;
    params.channels = kCh;
    params.aCodec = "aac";
    AudioMediaType fltp = S16Type();
    fltp.SetSampleFormat(AV_SAMPLE_FMT_FLTP);
    TG_EXPECT(enc.Open(params, fltp) < 0);
    TG_EXPECT(!enc.IsOpened());
    TG_EXPECT(enc.LastAvError() != 0);
}
