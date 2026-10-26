// Linux demo：真实素材驱动的「外灌 → 编码 → 封装」链路验证。
//
// Linux 上目前没有原生屏幕 / 音频采集实现（屏幕需 X11/PipeWire，音频需 PulseAudio/ALSA），
// 所以 IsSupported() 一律返回 false、Create*Capture 返回空。
// 这里改用**外灌帧**：
//   1 条视频 + 2 条独立音频，由系统 `ffmpeg` CLI 把源文件解成裸 NV12 / S16LE PCM，
//   demo 读管道按时序 Push 到内核；两条音频进入不同的外部通道，由内核 AudioMixer 做混流，
//   再和视频一起编码封装 —— 即除"采集"之外的整条开播链路。
//
// 为什么用系统 ffmpeg 而不是直接 libav：
//   采集端 ffmpeg 做了白名单裁剪（只保 mov/flv/image2 demuxer + h264/aac 解码），
//   喂进来的素材可能是 ts/mkv/mp3 等任意容器 —— 用 CLI 走解封装更稳也更解耦，
//   不污染采集端 ffmpeg 的组件白名单。
//
// 典型用法：
//   ./tg_capture_demo \
//       --video    /data/home/bertonwang/mov/dance-girl.n.mp4 \
//       --audio1   /data/home/bertonwang/mov/dance-girl.n.mp4 \
//       --audio2   /data/home/bertonwang/mov/iqiyi.ts \
//       --audio2-volume 0.1 \
//       --out      out.flv \
//       --seconds  10

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "TGCapture/ITGCapturer.h"

using namespace TGCapture;

namespace {

struct Options
{
    // 输入
    std::string video  = "/data/home/bertonwang/mov/dance-girl.n.mp4";
    std::string audio1;                                       // 默认 = video
    std::string audio2 = "/data/home/bertonwang/mov/iqiyi.ts";
    float       audio2Volume = 0.1f;                          // 第 2 路相对第 1 路的音量比例
    bool        audio2Loop   = true;                          // 兼容旧参数：true=无限循环, false=播一遍
    // 循环次数（0 = 无限）；--audio2-loops 和 --audio2-no-loop 二选一（前者优先）
    int         videoLoops  = 1;
    int         audio1Loops = 1;
    int         audio2Loops = 0;                              // 默认跟 audio2Loop=true 等价（无限）
    // 推送速度倍率（压测用）：>1 模拟源产太快，<1 模拟源产太慢
    //   内核 AudioMixer 参考时钟是 wallclock、内部 PTS 按采样数累加，
    //   speed!=1 时 wallclock 推进与采样推进出现偏差 → ChannelClock 看到 drift，
    //   触发 PI → BURST → ForceAlign 层级校正，日志里可观测
    float       audio1Speed = 1.0f;
    float       audio2Speed = 1.0f;

    // 编码/封装
    std::string out      = "out.flv";
    int         width    = 1280;
    int         height   = 720;
    float       fps      = 30.0f;
    int         bitrate  = 3000000;
    int         sampleRate = 44100;
    int         channels   = 2;
    bool        audio    = true;                              // 关掉就是纯视频
    int         seconds  = 10;                                // 0 = 直到视频 EOF
    int         logLevel = 1;

    // 工具
    std::string ffmpegBin = "ffmpeg";

    bool        showHelp = false;
};

void PrintUsage()
{
    printf("用法: tg_capture_demo [选项]\n"
           "  输入:\n"
           "    --video <path>          视频源，默认 dance-girl.n.mp4\n"
           "    --audio1 <path>         第 1 路音频源，默认同 --video（原始音量）\n"
           "    --audio2 <path>         第 2 路音频源，默认 iqiyi.ts\n"
           "    --audio2-volume <f>     第 2 路相对音量，默认 0.1\n"
           "    --audio2-no-loop        第 2 路素材放完即停（默认循环）\n"
           "    --video-loops  <n>      视频源循环次数，0=无限，默认 1\n"
           "    --audio1-loops <n>      第 1 路音频循环次数，0=无限，默认 1\n"
           "    --audio2-loops <n>      第 2 路音频循环次数，0=无限，默认 0(无限)\n"
           "    --audio1-speed <f>      第 1 路推送速度倍率，默认 1.0（>1 模拟过快，<1 过慢）\n"
           "    --audio2-speed <f>      第 2 路推送速度倍率，默认 1.0\n"
           "  输出:\n"
           "    --out <url>             out.flv / out.ts / rtmp:// / srt://，默认 out.flv\n"
           "    --seconds <n>           运行秒数，0 = 直到视频结束，默认 10\n"
           "    --w --h --fps --bitrate 编码参数，默认 1280 720 30 3000000\n"
           "    --sample-rate <n>       音频采样率，默认 44100\n"
           "    --channels <n>          音频通道数，默认 2\n"
           "    --no-audio              纯视频\n"
           "  其它:\n"
           "    --ffmpeg <path>         ffmpeg CLI，默认在 PATH 里找\n"
           "    --log-level <n>         0=Debug 1=Info 2=Warn 3=Error，默认 1\n"
           "    --help                  打印本帮助\n");
}

Options ParseArgs(int argc, char** argv)
{
    Options opt;
    auto next = [&](int& i, const char* name) -> const char* {
        if (i + 1 >= argc) { printf("缺少 %s 的参数\n", name); return nullptr; }
        return argv[++i];
    };
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--help") { opt.showHelp = true; }
        else if (a == "--no-audio") { opt.audio = false; }
        else if (a == "--audio2-no-loop") { opt.audio2Loop = false; opt.audio2Loops = 1; }
        else if (a == "--video-loops")  { if (auto* v = next(i, "--video-loops"))  opt.videoLoops  = std::atoi(v); }
        else if (a == "--audio1-loops") { if (auto* v = next(i, "--audio1-loops")) opt.audio1Loops = std::atoi(v); }
        else if (a == "--audio2-loops") { if (auto* v = next(i, "--audio2-loops")) opt.audio2Loops = std::atoi(v); }
        else if (a == "--audio1-speed") { if (auto* v = next(i, "--audio1-speed")) opt.audio1Speed = static_cast<float>(std::atof(v)); }
        else if (a == "--audio2-speed") { if (auto* v = next(i, "--audio2-speed")) opt.audio2Speed = static_cast<float>(std::atof(v)); }
        else if (a == "--video")   { if (auto* v = next(i, "--video"))   opt.video = v; }
        else if (a == "--audio1")  { if (auto* v = next(i, "--audio1"))  opt.audio1 = v; }
        else if (a == "--audio2")  { if (auto* v = next(i, "--audio2"))  opt.audio2 = v; }
        else if (a == "--audio2-volume") { if (auto* v = next(i, "--audio2-volume")) opt.audio2Volume = static_cast<float>(std::atof(v)); }
        else if (a == "--out")     { if (auto* v = next(i, "--out"))     opt.out = v; }
        else if (a == "--seconds") { if (auto* v = next(i, "--seconds")) opt.seconds = std::atoi(v); }
        else if (a == "--w")       { if (auto* v = next(i, "--w"))       opt.width = std::atoi(v); }
        else if (a == "--h")       { if (auto* v = next(i, "--h"))       opt.height = std::atoi(v); }
        else if (a == "--fps")     { if (auto* v = next(i, "--fps"))     opt.fps = static_cast<float>(std::atof(v)); }
        else if (a == "--bitrate") { if (auto* v = next(i, "--bitrate")) opt.bitrate = std::atoi(v); }
        else if (a == "--sample-rate") { if (auto* v = next(i, "--sample-rate")) opt.sampleRate = std::atoi(v); }
        else if (a == "--channels")    { if (auto* v = next(i, "--channels"))    opt.channels = std::atoi(v); }
        else if (a == "--ffmpeg")      { if (auto* v = next(i, "--ffmpeg"))      opt.ffmpegBin = v; }
        else if (a == "--log-level")   { if (auto* v = next(i, "--log-level"))   opt.logLevel = std::atoi(v); }
        else { printf("未知参数: %s\n", a.c_str()); opt.showHelp = true; }
    }
    if (opt.audio1.empty()) { opt.audio1 = opt.video; }
    return opt;
}

// Shell 转义：把路径塞进 popen 命令里，防止空格/引号把命令行拆散
std::string ShellQuote(const std::string& s)
{
    std::string q = "'";
    for (char c : s) {
        if (c == '\'') { q += "'\\''"; } else { q += c; }
    }
    q += '\'';
    return q;
}

// ffmpeg 子进程：封装读管道 + 优雅关闭
class FfmpegPipe
{
public:
    bool Open(const std::string& cmd)
    {
        m_cmd = cmd;
        m_fp = ::popen(cmd.c_str(), "r");
        if (!m_fp) {
            printf("[demo] popen 失败: %s (errno=%d)\n", cmd.c_str(), errno);
            return false;
        }
        return true;
    }
    // 读满 size 字节；EOF 或短读返回 false，调用方据此停止该路
    bool ReadFull(void* buf, size_t size)
    {
        if (!m_fp) { return false; }
        size_t got = 0;
        auto* p = static_cast<uint8_t*>(buf);
        while (got < size) {
            size_t n = std::fread(p + got, 1, size - got, m_fp);
            if (n == 0) {
                if (std::feof(m_fp) || std::ferror(m_fp)) { m_eof = true; return false; }
            }
            got += n;
        }
        return true;
    }
    bool Eof() const { return m_eof; }
    void Close()
    {
        if (m_fp) { ::pclose(m_fp); m_fp = nullptr; }
    }
    ~FfmpegPipe() { Close(); }

private:
    FILE*       m_fp = nullptr;
    bool        m_eof = false;
    std::string m_cmd;
};

int64_t ElapsedUs(const std::chrono::steady_clock::time_point& t0)
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now() - t0).count();
}

// 按比例缩 S16 样本（第 2 路音量调节），不开根号、不做限幅：0..1 范围内不会溢出
void ScalePcm(int16_t* pcm, int samples, int channels, float gain)
{
    if (gain >= 0.999f && gain <= 1.001f) { return; }
    const int total = samples * channels;
    for (int i = 0; i < total; ++i) {
        float v = pcm[i] * gain;
        if (v > 32767.0f)  { v = 32767.0f; }
        if (v < -32768.0f) { v = -32768.0f; }
        pcm[i] = static_cast<int16_t>(v);
    }
}

}  // namespace

int main(int argc, char** argv)
{
    const Options opt = ParseArgs(argc, argv);
    if (opt.showHelp) { PrintUsage(); return 0; }

    // ffmpeg 子进程写管道时如果我们这边先退出，SIGPIPE 会把我们干掉；忽略它，让 fread 自然返回 EOF
    std::signal(SIGPIPE, SIG_IGN);
    // Ctrl+C / kill -TERM 优雅退出：标志位 → 主循环 break → Stop() 刷 Muxer trailer
    // 不装处理器会被默认 SIGINT 当场终结进程，导致 flv 没有 trailer（ffprobe Duration 读不到）
    static std::atomic<bool> s_stop{false};
    auto onSig = +[](int) { s_stop.store(true); };
    std::signal(SIGINT,  onSig);
    std::signal(SIGTERM, onSig);

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
    ap.sampleRate = opt.sampleRate;
    ap.channels = opt.channels;
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

    // --------------------------------------------------------------- 启动 ffmpeg
    // -nostdin: 防止子进程抢终端；2>/dev/null: 把 ffmpeg 的 banner/stats 吞掉，别污染 demo 输出
    const int w = opt.width & ~1;      // NV12 要求偶数
    const int h = opt.height & ~1;
    const int sr = opt.sampleRate;
    const int ch = opt.channels;
    const int frameBytes      = w * h * 3 / 2;
    const int samplesPerPush  = 1024;  // AAC 一帧 1024 采样
    const int pcmChunkBytes   = samplesPerPush * ch * static_cast<int>(sizeof(int16_t));

    // -stream_loop K：K=N-1 跑 N 遍，K=-1 无限；N==1 不加
    auto streamLoopArg = [](int loops) -> std::string {
        if (loops <= 0) { return "-stream_loop -1"; }
        if (loops == 1) { return ""; }
        char b[32];
        std::snprintf(b, sizeof(b), "-stream_loop %d", loops - 1);
        return b;
    };

    char cmdbuf[1024];
    FfmpegPipe videoPipe, audio1Pipe, audio2Pipe;

    std::snprintf(cmdbuf, sizeof(cmdbuf),
                  "%s -nostdin -loglevel error %s -i %s -map 0:v:0 "
                  "-vf \"scale=%d:%d:flags=bicubic,fps=%g,format=nv12\" "
                  "-f rawvideo - 2>/dev/null",
                  opt.ffmpegBin.c_str(), streamLoopArg(opt.videoLoops).c_str(),
                  ShellQuote(opt.video).c_str(), w, h, opt.fps);
    if (!videoPipe.Open(cmdbuf)) { ITGCapturer::Destroy(capturer); ITGCapturerMgr::UnInit(); return 1; }

    if (opt.audio) {
        std::snprintf(cmdbuf, sizeof(cmdbuf),
                      "%s -nostdin -loglevel error %s -i %s -map 0:a:0 "
                      "-ar %d -ac %d -f s16le - 2>/dev/null",
                      opt.ffmpegBin.c_str(), streamLoopArg(opt.audio1Loops).c_str(),
                      ShellQuote(opt.audio1).c_str(), sr, ch);
        if (!audio1Pipe.Open(cmdbuf)) { ITGCapturer::Destroy(capturer); ITGCapturerMgr::UnInit(); return 1; }

        std::snprintf(cmdbuf, sizeof(cmdbuf),
                      "%s -nostdin -loglevel error %s -i %s -map 0:a:0 "
                      "-ar %d -ac %d -f s16le - 2>/dev/null",
                      opt.ffmpegBin.c_str(), streamLoopArg(opt.audio2Loops).c_str(),
                      ShellQuote(opt.audio2).c_str(), sr, ch);
        if (!audio2Pipe.Open(cmdbuf)) { ITGCapturer::Destroy(capturer); ITGCapturerMgr::UnInit(); return 1; }
    }

    printf("[demo] video=%s loops=%d\n"
           "[demo] audio1=%s loops=%d speed=%.3f\n"
           "[demo] audio2=%s loops=%d speed=%.3f vol=%.2f\n"
           "[demo] out=%s %dx%d@%.2f %dkbps / %dHz %dch\n",
           opt.video.c_str(),  opt.videoLoops,
           opt.audio1.c_str(), opt.audio1Loops, opt.audio1Speed,
           opt.audio2.c_str(), opt.audio2Loops, opt.audio2Speed, opt.audio2Volume,
           opt.out.c_str(), w, h, opt.fps, opt.bitrate / 1000, sr, ch);

    // -------------------------------------------------------------------- 灌帧
    const int64_t frameUs  = static_cast<int64_t>(1000000.0 / opt.fps);
    const int64_t audioUs  = static_cast<int64_t>(1000000.0 * samplesPerPush / sr);  // 采样数时间轴
    const int64_t totalUs  = static_cast<int64_t>(opt.seconds) * 1000000LL;           // 0 = 不限
    // speed>1 → wallclock 触发间隔变小（1s wallclock 推送 speed 秒的采样数据）
    //   内核 AudioMixer 的参考时钟是真实 wallclock，内部 PTS 按采样数累加，
    //   触发间隔 ≠ 采样时长 时，ChannelClock 会观察到 drift 并触发 PI/BURST/ForceAlign。
    const int64_t a1TrigUs = static_cast<int64_t>(audioUs / std::max(0.001f, opt.audio1Speed));
    const int64_t a2TrigUs = static_cast<int64_t>(audioUs / std::max(0.001f, opt.audio2Speed));

    std::vector<uint8_t> nv12(static_cast<size_t>(frameBytes));
    std::vector<int16_t> pcm1(static_cast<size_t>(samplesPerPush) * ch);
    std::vector<int16_t> pcm2(static_cast<size_t>(samplesPerPush) * ch);

    const auto t0 = std::chrono::steady_clock::now();
    int64_t videoDue   = 0;
    int64_t audio1Due  = 0, audio1Pts = 0;   // Due = wallclock 触发时刻，Pts = 采样数时间轴
    int64_t audio2Due  = 0, audio2Pts = 0;
    int pushedVideo = 0, pushedAudio1 = 0, pushedAudio2 = 0, rejected = 0;
    bool videoDone = false, audio1Done = !opt.audio, audio2Done = !opt.audio;

    while (!errored.load()) {
        if (s_stop.load()) { printf("[demo] 收到退出信号，刷 trailer\n"); break; }
        const int64_t now = ElapsedUs(t0);
        if (totalUs > 0 && now >= totalUs) { break; }
        // 视频 EOF 作为整体结束条件（音频无限循环场景下不应等 audio pipe EOF，否则永不退出）
        if (videoDone) { break; }
        if (audio1Done && audio2Done && !opt.audio) { break; }

        // ---- 视频 ----
        if (!videoDone && now >= videoDue) {
            if (videoPipe.ReadFull(nv12.data(), frameBytes)) {
                if (capturer->PushExternalVideoFrame(nv12.data(), w, h, w, videoDue) == 0) {
                    ++pushedVideo;
                } else {
                    ++rejected;
                }
                videoDue += frameUs;
            } else {
                videoDone = true;  // 视频 EOF 即结束整体演示
            }
        }
        // ---- 音频 1：原音量，走 Micphone 通道 ----
        if (!audio1Done && now >= audio1Due) {
            if (audio1Pipe.ReadFull(pcm1.data(), pcmChunkBytes)) {
                if (capturer->PushExternalAudioFrame(TypeOfGameMicphone4Slave,
                        reinterpret_cast<const uint8_t*>(pcm1.data()),
                        pcmChunkBytes, sr, ch, audio1Pts) == 0) {
                    ++pushedAudio1;
                } else { ++rejected; }
                audio1Due += a1TrigUs;   // wallclock 节奏（受 speed 影响）
                audio1Pts += audioUs;    // PTS 按采样数推进（不受 speed 影响）
            } else {
                audio1Done = true;
            }
        }
        // ---- 音频 2：按比例缩音量，走 Audio 通道（与音频 1 独立，内核会 mix）----
        if (!audio2Done && now >= audio2Due) {
            if (audio2Pipe.ReadFull(pcm2.data(), pcmChunkBytes)) {
                ScalePcm(pcm2.data(), samplesPerPush, ch, opt.audio2Volume);
                if (capturer->PushExternalAudioFrame(TypeOfGameAudio4Slave,
                        reinterpret_cast<const uint8_t*>(pcm2.data()),
                        pcmChunkBytes, sr, ch, audio2Pts) == 0) {
                    ++pushedAudio2;
                } else { ++rejected; }
                audio2Due += a2TrigUs;
                audio2Pts += audioUs;
            } else {
                audio2Done = true;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    videoPipe.Close();
    audio1Pipe.Close();
    audio2Pipe.Close();

    capturer->Stop();
    printf("[demo] 输出=%s 视频帧=%d 音频1=%d 音频2=%d 拒绝=%d\n",
           opt.out.c_str(), pushedVideo, pushedAudio1, pushedAudio2, rejected);

    ITGCapturer::Destroy(capturer);
    ITGCapturerMgr::UnInit();
    return (pushedVideo > 0 && !errored.load()) ? 0 : 1;
}
