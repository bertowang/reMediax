// TGCapture Windows 测试 demo：采集屏幕 -> 编码 -> 封装推流（文件或 rtmp/srt）
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>

#include "TGCapture/ITGCapturer.h"
#include "TGCapture/compat/ESMediaXCapturer.h"   // 兼容层（验证老接口可直接切换）
#include "capture/Source.h"
#include "core/Clock.h"
#include "core/MediaTypes.h"

extern "C"
{
#include <libavutil/pixfmt.h>
}

namespace
{
struct Options
{
    std::string output = "out.flv";
    std::string vcodec = "libopenh264";
    std::string acodec = "libfdk_aac";
    int width = 1280;
    int height = 720;
    int fps = 30;
    int bitrate = 3000000;
    int abitrate = 128000;
    int seconds = 10;
    bool audio = true;
    bool fakeVideo = false;   // 用人造帧代替屏幕采集（排查用）
};

// 测试用视频源：生成 NV12 测试图案，用于隔离"采集侧"与"编码/封装侧"问题
class FakeVideoSource : public TGCapture::capture::VideoSourceBase
{
public:
    FakeVideoSource()
        : TGCapture::capture::VideoSourceBase(TGCapture::TypeOfGameEngine4Slave,
                                              TGCapture::VideoScaleStrategy::SCALE_FIT_V0) {}

    int Open(int width, int height, float fps) override
    {
        m_w = width;
        m_h = height;
        m_fps = fps > 0 ? fps : 30.0f;
        return 0;
    }

protected:
    void Loop(std::stop_token st) override
    {
        const int64_t intervalUs = static_cast<int64_t>(1000000.0 / (m_fps > 0 ? m_fps : 30.0));
        int index = 0;
        while (!st.stop_requested()) {
            const size_t ySize = static_cast<size_t>(m_w) * m_h;
            const size_t uvSize = static_cast<size_t>(m_w) * m_h / 2;
            auto buffer = std::shared_ptr<uint8_t>(new uint8_t[ySize + uvSize], std::default_delete<uint8_t[]>());
            const int shift = (index++ * 4) % 200;
            memset(buffer.get(), 128 + shift, ySize);
            memset(buffer.get() + ySize, 128, uvSize);

            auto frame = std::make_shared<TGCapture::core::Frame>();
            frame->kind = TGCapture::core::MediaKind::Video;
            frame->width = m_w;
            frame->height = m_h;
            frame->pixFmt = AV_PIX_FMT_NV12;
            frame->planes[0] = buffer;
            frame->strides[0] = m_w;
            frame->planes[1] = std::shared_ptr<uint8_t>(buffer, buffer.get() + ySize);
            frame->strides[1] = m_w;
            frame->captureUs = TGCapture::core::MonotonicClock::NowUs();
            frame->ptsUs = frame->captureUs;
            Emit(std::move(frame));
            std::this_thread::sleep_for(std::chrono::microseconds(intervalUs));
        }
    }

private:
    int m_w = 1280;
    int m_h = 720;
    float m_fps = 30.0f;
};

void PrintUsage()
{
    printf("用法: tg_capture_demo [选项]\n"
           "  --out <url>      输出：out.flv / out.ts / rtmp://... / srt://...  (默认 out.flv)\n"
           "  --vcodec <name>  libopenh264 / h264_nvenc / h264_mf / h264_amf (默认 libopenh264)\n"
           "  --acodec <name>  libfdk_aac / aac (默认 libfdk_aac)\n"
           "  --w <width>      默认 1280\n"
           "  --h <height>     默认 720\n"
           "  --fps <n>        默认 30\n"
           "  --bitrate <bps>  默认 3000000\n"
           "  --seconds <n>    运行时长（秒），默认 10\n"
           "  --no-audio       关闭音频\n");
}

Options ParseArgs(int argc, char** argv)
{
    Options opt;
    for (int i = 1; i < argc; ++i) {
        std::string key = argv[i];
        if (key == "--no-audio") {
            opt.audio = false;
            continue;
        }
        if (key == "--fake-video") {
            opt.fakeVideo = true;
            continue;
        }
        if (i + 1 >= argc) {
            break;
        }
        std::string value = argv[i + 1];
        ++i;
        if (key == "--out") {
            opt.output = value;
        } else if (key == "--vcodec") {
            opt.vcodec = value;
        } else if (key == "--acodec") {
            opt.acodec = value;
        } else if (key == "--w") {
            opt.width = std::stoi(value);
        } else if (key == "--h") {
            opt.height = std::stoi(value);
        } else if (key == "--fps") {
            opt.fps = std::stoi(value);
        } else if (key == "--bitrate") {
            opt.bitrate = std::stoi(value);
        } else if (key == "--seconds") {
            opt.seconds = std::stoi(value);
        } else if (key == "--no-audio") {
            opt.audio = false;
        }
    }
    return opt;
}
}  // namespace

int main(int argc, char** argv)
{
    // 关闭 stdout 缓冲，确保崩溃前日志可见
    setvbuf(stdout, nullptr, _IONBF, 0);

    const Options opt = ParseArgs(argc, argv);
    if (argc > 1 && (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0)) {
        PrintUsage();
        return 0;
    }

    TGCapture::ITGCapturerMgr::Init(0);  // 0=Debug，便于排查

    // 通过兼容层（ESMediaX::ESCapturer）创建，验证老调用方零改动可切换
    ESMediaX::ESCapturer::IESMediaXCapturer* capturer = ESMediaX::ESCapturer::IESMediaXCapturer::Create();
    if (!capturer) {
        printf("创建 capturer 失败\n");
        return -1;
    }
    static_assert(std::is_same_v<decltype(capturer), TGCapture::ITGCapturer*>);

    // 视频源：屏幕采集（--fake-video 时用人造帧）
    TGCapture::IVideoSource* video = nullptr;
    std::shared_ptr<FakeVideoSource> fake;
    if (opt.fakeVideo) {
        fake = std::make_shared<FakeVideoSource>();
        video = fake.get();
    } else {
        video = capturer->CreateScreenCapture(0, TGCapture::DXGI);
    }
    if (!video || capturer->AddVideoSource(video) != 0) {
        printf("添加视频源失败\n");
        TGCapture::ITGCapturer::Destroy(capturer);
        return -2;
    }

    // 音频源：麦克风
    if (opt.audio) {
        capturer->EnableMicrophoneCapture(false);
    }

    TGCapture::VideoEncoderParams vParams;
    vParams.width = opt.width;
    vParams.height = opt.height;
    vParams.fps = static_cast<float>(opt.fps);
    vParams.vBitrate = opt.bitrate;
    vParams.gop = opt.fps * 2;
    vParams.vCodec = opt.vcodec;
    vParams.vEncoderType = (opt.vcodec == "h264_nvenc") ? TGCapture::h264_nvenc : TGCapture::libopenh264;
    capturer->SetupVideoEncoder(vParams);

    TGCapture::AudioEncoderParams aParams;
    aParams.sampleRate = 44100;
    aParams.channels = 2;
    aParams.aBitrate = opt.abitrate;
    aParams.aCodec = opt.acodec;
    aParams.enabled = opt.audio;   // --no-audio：不建音频编码器与音频流
    capturer->SetupAudioEncoder(aParams);

    TGCapture::MuxerParams mParams;
    mParams.muxer = (opt.output.find(".ts") != std::string::npos || opt.output.rfind("srt://", 0) == 0)
                        ? "mpegts"
                        : "flv";
    mParams.muxerOutput = opt.output;
    capturer->SetupMuxer(mParams);

    std::atomic_bool started{false};
    capturer->SetOnPreparedCallback([&started](TGCapture::ITGCapturer* c) {
        printf("[demo] prepared -> start\n");
        if (c->Start() == 0) {
            started.store(true);
        }
    });
    capturer->SetOnErrorCallback([](int32_t what, int32_t extra, const char* msg) {
        printf("[demo] error what=%d extra=%d msg=%s\n", what, extra, msg);
    });
    capturer->SetOnStartCallback([]() { printf("[demo] started\n"); });
    capturer->SetOnStopCallback([](int32_t err) { printf("[demo] stopped err=%d\n", err); });

    printf("[demo] 输出: %s  编码器: %s  %dx%d@%d  时长: %ds\n",
           opt.output.c_str(), opt.vcodec.c_str(), opt.width, opt.height, opt.fps, opt.seconds);

    if (capturer->Prepare() != 0) {
        printf("[demo] Prepare 失败\n");
        TGCapture::ITGCapturer::Destroy(capturer);
        return -3;
    }

    // 运行指定时长（或到 300s 上限）
    for (int i = 0; i < opt.seconds * 10; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (i % 50 == 0 && started.load()) {
            printf("[demo] %s\n", capturer->GetStatsInfo(TGCapture::StatsInfoType::bitrateInfo).c_str());
        }
    }

    printf("[demo] 最终统计: %s\n", capturer->GetStatsInfo(TGCapture::StatsInfoType::videoInfo).c_str());
    capturer->Stop();
    TGCapture::ITGCapturer::Destroy(capturer);
    TGCapture::ITGCapturerMgr::UnInit();
    printf("[demo] 完成，输出文件: %s\n", opt.output.c_str());
    return 0;
}
