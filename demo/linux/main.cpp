// Linux demo：没有采集设备也能验证「编码 → 封装」链路。
//
// Linux 上目前没有原生屏幕 / 音频采集实现（屏幕需 X11/PipeWire，音频需 PulseAudio/ALSA），
// 所以 IsSupported() 一律返回 false、Create*Capture 返回空。
// 这里改用**外灌帧**（PushExternalVideoFrame / PushExternalAudioFrame）：
// 人造 NV12 画面 + 正弦 PCM，验证 libopenh264 / libfdk_aac / flv(mpegts) 封装与音画同步
// —— 也就是除"采集"之外的整条开播链路。
//
// 它同时是唯一的**链接自检**：只编静态库不会触发链接，
// ffmpeg 及其依赖（openssl / srt / fdk-aac / openh264 / zlib）的静态链是否完整，
// 只有在这里才暴露得出来。
//
// 用法:
//   ./tg_capture_demo [--out out.flv] [--seconds 5] [--w 1280] [--h 720]
//                     [--fps 30] [--bitrate 3000000] [--no-audio] [--log-level 1]

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "TGCapture/ITGCapturer.h"

using namespace TGCapture;

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Options
{
    std::string out = "out.flv";
    int width = 1280;
    int height = 720;
    float fps = 30.0f;
    int bitrate = 3000000;
    int seconds = 5;
    bool audio = true;
    int logLevel = 1;
    bool showHelp = false;
};

void PrintUsage()
{
    printf("用法: tg_capture_demo [选项]\n"
           "  --out <url>       输出（out.flv / out.ts / rtmp://… / srt://…），默认 out.flv\n"
           "  --seconds <n>     运行时长，默认 5\n"
           "  --w --h --fps --bitrate   推流参数，默认 1280 720 30 3000000\n"
           "  --no-audio        纯视频（不建音频编码器）\n"
           "  --log-level <n>   0=Debug 1=Info 2=Warn 3=Error，默认 1\n"
           "  --help            打印本帮助\n");
}

Options ParseArgs(int argc, char** argv)
{
    Options opt;
    auto next = [&](int& i, const char* name) -> const char* {
        if (i + 1 >= argc) {
            printf("缺少 %s 的参数\n", name);
            return nullptr;
        }
        return argv[++i];
    };
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--help") { opt.showHelp = true; }
        else if (a == "--no-audio") { opt.audio = false; }
        else if (a == "--out") { if (auto* v = next(i, "--out")) opt.out = v; }
        else if (a == "--seconds") { if (auto* v = next(i, "--seconds")) opt.seconds = std::atoi(v); }
        else if (a == "--w") { if (auto* v = next(i, "--w")) opt.width = std::atoi(v); }
        else if (a == "--h") { if (auto* v = next(i, "--h")) opt.height = std::atoi(v); }
        else if (a == "--fps") { if (auto* v = next(i, "--fps")) opt.fps = static_cast<float>(std::atof(v)); }
        else if (a == "--bitrate") { if (auto* v = next(i, "--bitrate")) opt.bitrate = std::atoi(v); }
        else if (a == "--log-level") { if (auto* v = next(i, "--log-level")) opt.logLevel = std::atoi(v); }
        else { printf("未知参数: %s\n", a.c_str()); opt.showHelp = true; }
    }
    return opt;
}

// 人造 NV12：Y 是随帧号平移的斜向渐变（画面在动，便于确认帧真的进去了），UV 给一点色彩
void FillNv12(uint8_t* buf, int w, int h, int frame)
{
    uint8_t* y = buf;
    uint8_t* uv = buf + static_cast<size_t>(w) * h;
    const int shift = frame * 3;
    for (int r = 0; r < h; ++r) {
        for (int c = 0; c < w; ++c) {
            y[static_cast<size_t>(r) * w + c] = static_cast<uint8_t>((c + r + shift) & 0xFF);
        }
    }
    const size_t uvSize = static_cast<size_t>(w) * (h / 2);
    for (size_t i = 0; i < uvSize; ++i) {
        uv[i] = (i & 1) ? 0x40 : 0xC0;
    }
}

double g_phase = 0.0;

void FillPcm(int16_t* pcm, int samples, int channels)
{
    const double freq = 440.0;
    const double sr = 44100.0;
    for (int i = 0; i < samples; ++i) {
        const int16_t v = static_cast<int16_t>(12000.0 * std::sin(g_phase));
        g_phase += 2.0 * kPi * freq / sr;
        if (g_phase > 2.0 * kPi) { g_phase -= 2.0 * kPi; }
        for (int ch = 0; ch < channels; ++ch) { pcm[i * channels + ch] = v; }
    }
}

int64_t ElapsedUs(const std::chrono::steady_clock::time_point& t0)
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

int main(int argc, char** argv)
{
    const Options opt = ParseArgs(argc, argv);
    if (opt.showHelp) { PrintUsage(); return 0; }

    ITGCapturerMgr::Init(opt.logLevel, "", true);

    ITGCapturer* capturer = ITGCapturer::Create();
    if (!capturer) {
        printf("[demo] ITGCapturer::Create 失败\n");
        ITGCapturerMgr::UnInit();
        return 1;
    }

    std::atomic<bool> prepared{false};
    std::atomic<bool> errored{false};
    capturer->SetOnPreparedCallback([&](ITGCapturer* c) {
        c->Start();
        prepared.store(true);
    });
    capturer->SetOnErrorCallback([&](int32_t what, int32_t extra, const char* msg) {
        printf("[demo] error what=%d extra=%d msg=%s\n", what, extra, msg ? msg : "");
        errored.store(true);
    });

    // Linux 无采集：明确告知，避免误以为"没画面"是 bug
    printf("[demo] 屏幕采集可用=%d 麦克风可用=%d（Linux 无原生实现，走外灌帧）\n",
           static_cast<int>(capturer->IsSupported(CaptureFeature::Screen)),
           static_cast<int>(capturer->IsSupported(CaptureFeature::Microphone)));

    VideoEncoderParams vp;
    vp.width = opt.width;
    vp.height = opt.height;
    vp.fps = opt.fps;
    vp.vBitrate = opt.bitrate;
    vp.vCodec = "libopenh264";

    AudioEncoderParams ap;
    ap.enabled = opt.audio;
    ap.sampleRate = 44100;
    ap.channels = 2;
    ap.aCodec = "libfdk_aac";

    MuxerParams mp;
    const bool isTs = opt.out.size() >= 3 && opt.out.compare(opt.out.size() - 3, 3, ".ts") == 0;
    mp.muxer = isTs ? "mpegts" : "flv";
    mp.muxerOutput = opt.out;

    if (capturer->SetupVideoEncoder(vp) != 0 || capturer->SetupAudioEncoder(ap) != 0 ||
        capturer->SetupMuxer(mp) != 0) {
        printf("[demo] 参数配置失败\n");
        ITGCapturer::Destroy(capturer);
        ITGCapturerMgr::UnInit();
        return 1;
    }

    if (capturer->Prepare() != 0) {
        printf("[demo] Prepare 失败\n");
        ITGCapturer::Destroy(capturer);
        ITGCapturerMgr::UnInit();
        return 1;
    }
    // Prepare() 内部同步触发 OnPrepared（demo 在回调里 Start），这里等它切到 Running
    for (int i = 0; i < 300 && !prepared.load() && !errored.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!prepared.load()) {
        printf("[demo] 未进入 Running（prepared=%d errored=%d）\n",
               static_cast<int>(prepared.load()), static_cast<int>(errored.load()));
        ITGCapturer::Destroy(capturer);
        ITGCapturerMgr::UnInit();
        return 1;
    }

    // ---------------------------------------------------------------- 灌帧
    const int w = opt.width & ~1;          // NV12 要求偶数
    const int h = opt.height & ~1;
    const int64_t frameUs = static_cast<int64_t>(1000000.0 / opt.fps);
    const int sampleRate = ap.sampleRate;
    const int channels = ap.channels;
    const int samplesPerPush = 1024;       // AAC 一帧 1024 采样
    const int bytesPerPush = samplesPerPush * channels * static_cast<int>(sizeof(int16_t));
    const int64_t audioUs = static_cast<int64_t>(1000000.0 * samplesPerPush / sampleRate);
    const int64_t totalUs = static_cast<int64_t>(opt.seconds) * 1000000LL;

    std::vector<uint8_t> nv12(static_cast<size_t>(w) * h * 3 / 2);
    std::vector<int16_t> pcm(static_cast<size_t>(samplesPerPush) * channels);

    const auto t0 = std::chrono::steady_clock::now();
    int64_t videoDue = 0;
    int64_t audioDue = 0;
    int pushedVideo = 0;
    int pushedAudio = 0;
    int rejected = 0;

    while (!errored.load()) {
        const int64_t now = ElapsedUs(t0);
        if (now >= totalUs) { break; }

        if (now >= videoDue) {
            FillNv12(nv12.data(), w, h, pushedVideo);
            if (capturer->PushExternalVideoFrame(nv12.data(), w, h, w, videoDue) == 0) {
                ++pushedVideo;
            } else {
                ++rejected;
            }
            videoDue += frameUs;
        }
        if (opt.audio && now >= audioDue) {
            FillPcm(pcm.data(), samplesPerPush, channels);
            if (capturer->PushExternalAudioFrame(TypeOfGameMicphone4Slave,
                                                 reinterpret_cast<const uint8_t*>(pcm.data()),
                                                 bytesPerPush, sampleRate, channels, audioDue) == 0) {
                ++pushedAudio;
            } else {
                ++rejected;
            }
            audioDue += audioUs;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    capturer->Stop();
    printf("[demo] 输出=%s 视频帧=%d 音频块=%d 拒绝=%d\n",
           opt.out.c_str(), pushedVideo, pushedAudio, rejected);

    ITGCapturer::Destroy(capturer);
    ITGCapturerMgr::UnInit();
    return (pushedVideo > 0 && !errored.load()) ? 0 : 1;
}
