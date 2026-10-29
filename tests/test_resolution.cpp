// 分辨率档位与运行期切换：
//   档位表 / 参数校验；
//   推流中切分辨率（flv / ts）：输出可解码、分辨率确实改变、音视频时间轴连续、通知发出；
//   暂停图 / 水印按帧的实际尺寸自适应
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "TGCapturer.h"
#include "core/PauseFrame.h"
#include "core/Watermark.h"
#include "test_media.h"
#include "tg_test.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

using namespace TGCapture;
using namespace std::chrono_literals;

namespace
{

constexpr int kRate = 48000;
constexpr int kCh = 2;

std::string TmpPath(const char* name)
{
    return (std::filesystem::temp_directory_path() / name).string();
}

using tgmedia::WriteSolidPng;

core::FramePtr MakeNv12(int w, int h, uint8_t y, uint8_t uv)
{
    const size_t ySize = static_cast<size_t>(w) * h;
    auto buffer = std::shared_ptr<uint8_t>(new uint8_t[ySize + ySize / 2], std::default_delete<uint8_t[]>());
    std::memset(buffer.get(), y, ySize);
    std::memset(buffer.get() + ySize, uv, ySize / 2);
    auto f = core::MakeVideoFrame();
    f->width = w;
    f->height = h;
    f->pixFmt = AV_PIX_FMT_NV12;
    f->planes[0] = buffer;
    f->strides[0] = w;
    f->planes[1] = std::shared_ptr<uint8_t>(buffer, buffer.get() + ySize);
    f->strides[1] = w;
    return f;
}

// ---- 推流夹具 ----
struct Rig
{
    TGCapturer c{GameID::GAME_ID_DEFAULT};
    std::mutex mutex;
    std::vector<std::pair<std::string, int64_t>> metaChanged;   // (content, data)
    std::vector<int> errors;

    Rig(const std::string& out, const std::string& muxer)
    {
        c.SetOnNotifyCallback([this](NotifyMsgType t, const std::string& content, int64_t data) {
            if (t == EsCap_Notify_VideoMetaInfo_Changed) {
                std::lock_guard<std::mutex> lock(mutex);
                metaChanged.emplace_back(content, data);
            }
        });
        c.SetOnErrorCallback([this](int32_t what, int32_t, const char*) {
            std::lock_guard<std::mutex> lock(mutex);
            errors.push_back(what);
        });
        VideoEncoderParams vp;
        vp.width = 320;
        vp.height = 240;
        vp.fps = 30;
        vp.gop = 30;
        vp.vBitrate = 300000;
        vp.vCodec = "libopenh264";
        TG_EXPECT_EQ(c.SetupVideoEncoder(vp), int{TGErr_OK});
        AudioEncoderParams ap;
        ap.enabled = true;
        ap.sampleRate = kRate;
        ap.channels = kCh;
        TG_EXPECT_EQ(c.SetupAudioEncoder(ap), int{TGErr_OK});
        MuxerParams mp;
        mp.muxer = muxer;
        mp.muxerOutput = out;
        TG_EXPECT_EQ(c.SetupMuxer(mp), int{TGErr_OK});
    }

    // 实时节奏推 durMs：外部视频 320x240 每 33ms 一帧（尺寸固定 —— 编码器负责缩放），外部 PCM 每 10ms 一块
    void Run(int durMs)
    {
        std::vector<uint8_t> nv12(320 * 240 * 3 / 2, 100);
        std::vector<int16_t> pcm(static_cast<size_t>(kRate / 100) * kCh, 1000);
        const auto start = std::chrono::steady_clock::now();
        auto next = start;
        int tick = 0;
        while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(durMs)) {
            c.PushExternalAudioFrame(TypeOfGameAudio4Slave, reinterpret_cast<const uint8_t*>(pcm.data()),
                                     static_cast<int>(pcm.size() * sizeof(int16_t)), kRate, kCh, -1);
            if (tick % 3 == 0) {
                c.PushExternalVideoFrame(nv12.data(), 320, 240, 320, -1);
            }
            ++tick;
            next += 10ms;
            std::this_thread::sleep_until(next);
        }
    }

    std::vector<std::string> Changes()
    {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<std::string> out;
        for (const auto& [content, _] : metaChanged) out.push_back(content);
        return out;
    }
};

// flv：解复用 + 解码，记录每帧解码出的尺寸（按出现顺序去重）与时间轴
struct Decoded
{
    std::vector<std::pair<int, int>> sizes;   // 依次出现过的分辨率
    int frames = 0;
    int decodeErrors = 0;
    double maxVideoGapMs = 0;
    double maxAudioGapMs = 0;
    double durationSec = 0;
};

Decoded DecodeFlv(const std::string& path)
{
    Decoded d;
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) {
        return d;
    }
    avformat_find_stream_info(fmt, nullptr);
    int vIdx = -1;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) vIdx = static_cast<int>(i);
    }
    AVCodecContext* dec = nullptr;
    if (vIdx >= 0) {
        const AVCodec* codec = avcodec_find_decoder(fmt->streams[vIdx]->codecpar->codec_id);
        if (codec) {
            dec = avcodec_alloc_context3(codec);
            avcodec_parameters_to_context(dec, fmt->streams[vIdx]->codecpar);
            if (avcodec_open2(dec, codec, nullptr) < 0) avcodec_free_context(&dec);
        }
    }
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    int64_t lastV = AV_NOPTS_VALUE, lastA = AV_NOPTS_VALUE, lastADur = 0, firstA = AV_NOPTS_VALUE;
    AVRational aTb{1, 1000};
    auto drain = [&] {
        while (dec && avcodec_receive_frame(dec, frame) >= 0) {
            ++d.frames;
            if (d.sizes.empty() || d.sizes.back() != std::make_pair(frame->width, frame->height)) {
                d.sizes.emplace_back(frame->width, frame->height);
            }
            av_frame_unref(frame);
        }
    };
    while (av_read_frame(fmt, pkt) >= 0) {
        AVStream* st = fmt->streams[pkt->stream_index];
        if (pkt->stream_index == vIdx) {
            if (lastV != AV_NOPTS_VALUE) {
                d.maxVideoGapMs = std::max(d.maxVideoGapMs, (pkt->pts - lastV) * av_q2d(st->time_base) * 1000.0);
            }
            lastV = pkt->pts;
            if (dec) {
                if (avcodec_send_packet(dec, pkt) < 0) ++d.decodeErrors;
                drain();
            }
        } else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && pkt->pts != AV_NOPTS_VALUE) {
            aTb = st->time_base;
            if (firstA == AV_NOPTS_VALUE) firstA = pkt->pts;
            if (lastA != AV_NOPTS_VALUE) {
                d.maxAudioGapMs = std::max(d.maxAudioGapMs, (pkt->pts - lastA - lastADur) * av_q2d(aTb) * 1000.0);
            }
            lastA = pkt->pts;
            lastADur = pkt->duration > 0 ? pkt->duration : 0;
        }
        av_packet_unref(pkt);
    }
    if (dec) {
        avcodec_send_packet(dec, nullptr);
        drain();
    }
    if (firstA != AV_NOPTS_VALUE) d.durationSec = (lastA - firstA) * av_q2d(aTb);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
    return d;
}

// ts：本库未编 mpegts 解复用器，直接在码流里找 IDR 前插入的 SPS（00 00 00 01 67 ...），
// 返回出现过的不同 SPS 的个数及每个 SPS 出现的次数（分辨率不同 → SPS 不同）
std::vector<std::pair<std::vector<uint8_t>, int>> ScanTsSps(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::vector<std::pair<std::vector<uint8_t>, int>> out;
    for (size_t i = 0; i + 16 < data.size(); ++i) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1 && (data[i + 4] & 0x1f) == 7) {
            std::vector<uint8_t> sps(data.begin() + static_cast<long>(i + 4), data.begin() + static_cast<long>(i + 14));
            auto it = std::find_if(out.begin(), out.end(), [&](const auto& p) { return p.first == sps; });
            if (it == out.end()) {
                out.emplace_back(sps, 1);
            } else {
                ++it->second;
            }
        }
    }
    return out;
}

int StatsWidth(TGCapturer& c)
{
    const std::string s = c.GetStatsInfo(StatsInfoType::videoInfo);
    const auto p = s.find("\"width\":");
    return p == std::string::npos ? -1 : std::atoi(s.c_str() + p + 8);
}

}  // namespace

TG_TEST(Resolution, PresetTable)
{
    VideoEncoderParams p;
    p.gop = 77;
    TG_ASSERT(ITGCapturerMgr::ApplyResolutionPreset("720p", p));
    TG_EXPECT_EQ(p.width, 1280);
    TG_EXPECT_EQ(p.height, 720);
    TG_EXPECT(p.fps == 30.0f);
    TG_EXPECT_EQ(p.vBitrate, 3000000);
    TG_EXPECT_EQ(p.ResolutionName, std::string("720p"));
    TG_EXPECT_EQ(p.gop, 77);   // 其它字段不动

    TG_ASSERT(ITGCapturerMgr::ApplyResolutionPreset("1080P60", p, true));   // 不区分大小写 + 竖屏
    TG_EXPECT_EQ(p.width, 1080);
    TG_EXPECT_EQ(p.height, 1920);
    TG_EXPECT(p.fps == 60.0f);
    TG_EXPECT_EQ(p.ResolutionName, std::string("1080p60"));

    const VideoEncoderParams before = p;
    TG_EXPECT(!ITGCapturerMgr::ApplyResolutionPreset("4k", p));
    TG_EXPECT(!ITGCapturerMgr::ApplyResolutionPreset("", p));
    TG_EXPECT_EQ(p.width, before.width);

    TG_EXPECT_EQ(ITGCapturerMgr::GetResolutionPresets(), std::string("360p,480p,540p,720p,720p60,1080p,1080p60"));
}

TG_TEST(Resolution, SetVideoResolutionValidation)
{
    TGCapturer c(GameID::GAME_ID_DEFAULT);
    TG_EXPECT_EQ(c.SetVideoResolution(640, 360), int{TGErr_NotConfigured});
    VideoEncoderParams vp;
    vp.width = 320;
    vp.height = 240;
    vp.fps = 30;
    vp.vBitrate = 300000;
    TG_ASSERT(c.SetupVideoEncoder(vp) == TGErr_OK);
    TG_EXPECT_EQ(c.SetVideoResolution(0, 360), int{TGErr_InvalidArgument});
    TG_EXPECT_EQ(c.SetVideoResolution(640, -2), int{TGErr_InvalidArgument});
    TG_EXPECT_EQ(c.SetVideoResolution(8192, 360), int{TGErr_InvalidArgument});
    TG_EXPECT_EQ(c.SetVideoResolution(640, 360, -1), int{TGErr_InvalidArgument});
    TG_EXPECT_EQ(c.SetResolutionPreset("nope"), int{TGErr_InvalidArgument});
    TG_EXPECT_EQ(c.SetVideoResolution(641, 361), int{TGErr_OK});   // 奇数向下对齐

    // 未开播时改的参数在 Prepare 时生效
    AudioEncoderParams ap;
    ap.enabled = false;
    TG_ASSERT(c.SetupAudioEncoder(ap) == TGErr_OK);
    MuxerParams mp;
    mp.muxerOutput = TmpPath("tg_test_resolution_idle.flv");
    TG_ASSERT(c.SetupMuxer(mp) == TGErr_OK);
    TG_ASSERT(c.Prepare() == TGErr_OK);
    TG_EXPECT_EQ(StatsWidth(c), 640);
    c.Stop();
    std::remove(mp.muxerOutput.c_str());
}

TG_TEST(Resolution, RuntimeSwitchFlv)
{
    const std::string out = TmpPath("tg_test_resolution.flv");
    std::vector<std::string> changes;
    {
        Rig rig(out, "flv");
        TG_ASSERT(rig.c.Prepare() == TGErr_OK);
        TG_ASSERT(rig.c.Start() == TGErr_OK);
        rig.Run(700);
        TG_ASSERT(rig.c.SetVideoResolution(640, 360, 600000) == TGErr_OK);
        rig.Run(700);
        TG_EXPECT_EQ(StatsWidth(rig.c), 640);
        TG_ASSERT(rig.c.SetResolutionPreset("360p", true) == TGErr_OK);   // 竖屏 360x640
        rig.Run(700);
        TG_EXPECT_EQ(StatsWidth(rig.c), 360);
        rig.c.Stop();
        changes = rig.Changes();
        std::lock_guard<std::mutex> lock(rig.mutex);
        TG_EXPECT(rig.errors.empty());
    }
    TG_ASSERT(changes.size() == 2u);
    TG_EXPECT_EQ(changes[0], std::string("640x360"));
    TG_EXPECT_EQ(changes[1], std::string("360x640"));

    const Decoded d = DecodeFlv(out);
    std::printf("    frames=%d sizes=%zu decodeErr=%d maxVideoGap=%.1fms maxAudioGap=%.1fms dur=%.2fs\n", d.frames,
                d.sizes.size(), d.decodeErrors, d.maxVideoGapMs, d.maxAudioGapMs, d.durationSec);
    TG_ASSERT(d.sizes.size() == 3u);
    TG_EXPECT(d.sizes[0] == std::make_pair(320, 240));
    TG_EXPECT(d.sizes[1] == std::make_pair(640, 360));
    TG_EXPECT(d.sizes[2] == std::make_pair(360, 640));
    TG_EXPECT_EQ(d.decodeErrors, 0);
    TG_EXPECT(d.frames > 45);
    TG_EXPECT(d.maxVideoGapMs < 150.0);   // 切换点不留明显的洞（重开编码器 < 1 帧间隔量级）
    TG_EXPECT(d.maxAudioGapMs < 30.0);    // 音频不受影响
    TG_EXPECT(d.durationSec > 1.8);
    std::remove(out.c_str());
}

TG_TEST(Resolution, RuntimeSwitchTsCarriesNewSps)
{
    const std::string out = TmpPath("tg_test_resolution.ts");
    {
        Rig rig(out, "mpegts");
        TG_ASSERT(rig.c.Prepare() == TGErr_OK);
        TG_ASSERT(rig.c.Start() == TGErr_OK);
        rig.Run(1200);   // gop=30 @30fps：切换前至少 2 个 IDR
        TG_ASSERT(rig.c.SetVideoResolution(640, 360) == TGErr_OK);
        rig.Run(1200);
        rig.c.Stop();
        TG_EXPECT(rig.Changes().size() == 1u);
    }
    const auto sps = ScanTsSps(out);
    for (const auto& [bytes, count] : sps) {
        std::printf("    sps x%d:", count);
        for (uint8_t b : bytes) std::printf(" %02x", b);
        std::printf("\n");
    }
    // 两种分辨率各自的 SPS，且新 SPS 不止出现在切换点（之后每个 IDR 前都要带新的，而不是旧的）
    TG_ASSERT(sps.size() == 2u);
    TG_EXPECT(sps[0].second >= 2);
    TG_EXPECT(sps[1].second >= 2);
    std::remove(out.c_str());
}

TG_TEST(Resolution, SwitchInPreparedStateAppliesBeforeFirstFrame)
{
    const std::string out = TmpPath("tg_test_resolution_prepared.flv");
    {
        Rig rig(out, "flv");
        TG_ASSERT(rig.c.Prepare() == TGErr_OK);
        TG_ASSERT(rig.c.SetVideoResolution(256, 144) == TGErr_OK);
        TG_ASSERT(rig.c.Start() == TGErr_OK);
        rig.Run(600);
        rig.c.Stop();
    }
    const Decoded d = DecodeFlv(out);
    TG_ASSERT(d.sizes.size() == 1u);
    TG_EXPECT(d.sizes[0] == std::make_pair(256, 144));
    TG_EXPECT_EQ(d.decodeErrors, 0);
    std::remove(out.c_str());
}

TG_TEST(Resolution, PauseFrameRescalesToFrameSize)
{
    const std::string png = TmpPath("tg_test_pause.png");
    TG_ASSERT(WriteSolidPng(png, 40, 30, 255, 0, 0));
    core::PauseFrame pause;
    TG_ASSERT(pause.Load(png, 64, 48, 0));
    TG_EXPECT_EQ(pause.Width(), 64);

    auto big = MakeNv12(128, 96, 16, 128);
    TG_ASSERT(pause.ApplyTo(*big));   // 尺寸不同：自动按 128x96 重新缩放
    TG_EXPECT_EQ(pause.Width(), 128);
    TG_EXPECT_EQ(pause.Height(), 96);
    // 纯红 BT.601 limited：Y≈82，V≈240
    const int y = big->planes[0].get()[50 * 128 + 70];
    const int v = big->planes[1].get()[20 * 128 + 31];
    TG_EXPECT(y > 70 && y < 95);
    TG_EXPECT(v > 220);

    auto small = MakeNv12(64, 48, 16, 128);
    TG_ASSERT(pause.ApplyTo(*small));
    TG_EXPECT(small->planes[0].get()[10 * 64 + 10] > 70);

    // 竖屏：旋转 90 度后尺寸按帧
    TG_ASSERT(pause.Load(png, 64, 48, 90));
    auto portrait = MakeNv12(48, 64, 16, 128);
    TG_ASSERT(pause.ApplyTo(*portrait));
    TG_EXPECT_EQ(pause.Width(), 48);
    std::remove(png.c_str());
}

TG_TEST(Resolution, WatermarkPositionFollowsFrameSize)
{
    const std::string png = TmpPath("tg_test_logo.png");
    TG_ASSERT(WriteSolidPng(png, 8, 8, 255, 255, 255));
    core::Watermark wm;
    WatermarkParams p;
    p.type = LogoOpType::LOGO_ADD;
    p.index = 1;
    p.url = png;
    p.position = 3;   // 右下
    TG_ASSERT(wm.Apply(p, 64, 48) == 0);

    auto big = MakeNv12(128, 96, 16, 128);
    wm.BlendOnto(*big);
    TG_EXPECT(big->planes[0].get()[95 * 128 + 127] > 200);   // 新画布的右下角
    TG_EXPECT_EQ(int{big->planes[0].get()[47 * 128 + 63]}, 16);   // 旧画布右下角位置不再有水印
    std::remove(png.c_str());
}
