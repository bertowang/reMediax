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
#include <condition_variable>
#include <deque>
#include <mutex>
#include <random>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <sys/types.h>
#include <signal.h>

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
    //   每块 1024 采样按 wallclock 间隔 1024/sr/speed 推送，ptsUs 传推送时刻（墙钟），
    //   内核据"时间戳流逝 vs 样本时长"测出设备漂移并在样本域补偿（≤0.5% 可完全补偿，更大由欠载/丢弃吸收）
    float       audio1Speed = 1.0f;
    float       audio2Speed = 1.0f;
    // 故障注入（压测用，默认全关）：
    //   --audio-pts-jitter-ms <N>  音频 PTS 加 ±N ms 的随机抖动（模拟采集端时间戳不准）
    //   --audio-pts-back-every <K> 每 K 帧对音频 PTS 回退 1 帧时长（模拟设备时钟回退）
    //   --video-stall-start <s> --video-stall-dur <s>  视频在 start 秒后暂停 dur 秒
    //   --video-stall-mode freeze|burst  freeze=卡顿期间的帧丢失（真实采集卡死）；
    //                                    burst=卡顿期间的帧积压，恢复后一次性灌入（默认）
    int         audioPtsJitterMs    = 0;
    int         audioPtsBackEvery   = 0;
    int         videoStallStartSec  = 0;
    int         videoStallDurSec    = 0;
    bool        videoStallBurst     = true;

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
    // 运行期切分辨率（可多次）：--switch-res <秒>:<档位名 | 宽x高>
    std::vector<std::pair<double, std::string>> switchRes;
    // 运行期加文件音频源：--audio-file <秒>:<路径>（内核解码文件音轨，AudioSourceKind::File）
    double      audioFileAt = -1;
    std::string audioFile;
    float       audioFileVolume = 1.0f;
    bool        audioFileLoop = true;

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
           "  故障注入（压测用）:\n"
           "    --audio-pts-jitter-ms <N>  音频 PTS ±N ms 随机抖动，默认 0\n"
           "    --audio-pts-back-every <K> 每 K 个音频帧将 PTS 回退 1 帧时长，默认 0（关）\n"
           "    --video-stall-start <s> --video-stall-dur <s>  视频从第 s 秒起暂停 dur 秒不推帧\n"
           "    --video-stall-mode <m>     freeze=卡顿期间帧丢失 / burst=恢复后积压帧一次性灌入，默认 burst\n"
           "  输出:\n"
           "    --out <url>             out.flv / out.ts / rtmp:// / srt://，默认 out.flv\n"
           "    --seconds <n>           运行秒数，0 = 直到视频结束，默认 10\n"
           "    --w --h --fps --bitrate 编码参数，默认 1280 720 30 3000000\n"
           "    --audio-file <s>:<path> 第 s 秒运行期加文件音频源（mp4/m4a/flv 的 AAC 音轨）\n"
           "    --audio-file-volume <f> 文件音频源音量，默认 1.0\n"
           "    --audio-file-no-loop    文件音频源放完即停（默认循环）\n"
           "    --switch-res <s>:<r>    第 s 秒运行期切换编码分辨率，r = 档位名(720p…) 或 宽x高，可多次\n"
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
        else if (a == "--audio-pts-jitter-ms")  { if (auto* v = next(i, "--audio-pts-jitter-ms"))  opt.audioPtsJitterMs    = std::atoi(v); }
        else if (a == "--audio-pts-back-every") { if (auto* v = next(i, "--audio-pts-back-every")) opt.audioPtsBackEvery   = std::atoi(v); }
        else if (a == "--video-stall-start")    { if (auto* v = next(i, "--video-stall-start"))    opt.videoStallStartSec  = std::atoi(v); }
        else if (a == "--video-stall-dur")      { if (auto* v = next(i, "--video-stall-dur"))      opt.videoStallDurSec    = std::atoi(v); }
        else if (a == "--video-stall-mode") {
            if (auto* v = next(i, "--video-stall-mode")) {
                const std::string m = v;
                if (m == "freeze") { opt.videoStallBurst = false; }
                else if (m == "burst") { opt.videoStallBurst = true; }
                else { printf("--video-stall-mode 只支持 freeze|burst\n"); opt.showHelp = true; }
            }
        }
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
        else if (a == "--audio-file") {
            if (auto* v = next(i, "--audio-file")) {
                const std::string arg = v;
                const auto colon = arg.find(':');
                if (colon != std::string::npos) {
                    opt.audioFileAt = std::atof(arg.substr(0, colon).c_str());
                    opt.audioFile = arg.substr(colon + 1);
                }
            }
        }
        else if (a == "--audio-file-volume") { if (auto* v = next(i, "--audio-file-volume")) opt.audioFileVolume = static_cast<float>(std::atof(v)); }
        else if (a == "--audio-file-no-loop") { opt.audioFileLoop = false; }
        else if (a == "--switch-res") {
            if (auto* v = next(i, "--switch-res")) {
                const std::string arg = v;
                const auto colon = arg.find(':');
                if (colon != std::string::npos) {
                    opt.switchRes.emplace_back(std::atof(arg.substr(0, colon).c_str()), arg.substr(colon + 1));
                }
            }
        }
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
        // 先输出 shell 自身 pid 再 exec：exec 后该 pid 就是 ffmpeg，Close() 时可以直接 kill。
        // 必要性：本进程忽略了 SIGPIPE，子进程继承后读端关闭只会收到 EPIPE；ffmpeg 随后进入
        // 退出 flush，而 aloop=loop=-1 会无限产出 —— 子进程永不退出，pclose 永远阻塞
        const std::string wrapped = "echo $$; exec " + cmd;
        m_fp = ::popen(wrapped.c_str(), "r");
        if (!m_fp) {
            printf("[demo] popen 失败: %s (errno=%d)\n", cmd.c_str(), errno);
            return false;
        }
        char line[32] = {0};
        if (std::fgets(line, sizeof(line), m_fp) != nullptr) {
            m_pid = static_cast<pid_t>(std::atol(line));
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
    // 只发信号不碰 FILE*：可以在读线程仍阻塞于 fread 时调用，fread 随即返回 EOF。
    // pclose 之前子进程尚未被回收（最多是僵尸），pid 不会被复用
    void Kill()
    {
        if (m_pid > 0) {
            ::kill(m_pid, SIGKILL);   // 只要裸数据，不需要 ffmpeg 优雅收尾
            m_pid = -1;
        }
    }
    void Close()
    {
        if (!m_fp) { return; }
        Kill();
        ::pclose(m_fp);
        m_fp = nullptr;
    }
    ~FfmpegPipe() { Close(); }

private:
    FILE*       m_fp = nullptr;
    pid_t       m_pid = -1;
    bool        m_eof = false;
    std::string m_cmd;
};

// 预读器：每路一个读线程 + 有界队列，主循环只从队列取块，不会被某一路的管道阻塞拖住其它路
// （旧实现单线程阻塞 fread：ffmpeg 解码抖动 / 循环 seek 时三路一起停，节奏被打乱）
class Prefetcher
{
public:
    Prefetcher(FfmpegPipe& pipe, size_t chunkBytes, size_t capacity)
        : m_pipe(pipe), m_chunkBytes(chunkBytes), m_capacity(capacity > 0 ? capacity : 1) {}
    ~Prefetcher() { Stop(); }

    void Start()
    {
        m_thread = std::thread([this] {
            for (;;) {
                std::vector<uint8_t> buf(m_chunkBytes);
                const bool ok = m_pipe.ReadFull(buf.data(), buf.size());
                std::unique_lock<std::mutex> lk(m_mutex);
                if (!ok) { m_eof = true; m_cv.notify_all(); return; }
                m_cv.wait(lk, [this] { return m_stop || m_queue.size() < m_capacity; });
                if (m_stop) { return; }
                m_queue.push_back(std::move(buf));
            }
        });
    }
    // 非阻塞取一块；返回 false 时用 Eof() 区分"暂时没数据"和"已结束"
    bool TryPop(std::vector<uint8_t>& out)
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_queue.empty()) { return false; }
        out.swap(m_queue.front());
        m_queue.pop_front();
        m_cv.notify_all();
        ++m_popped;
        return true;
    }
    bool Eof()
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_eof && m_queue.empty();
    }
    uint64_t Underflows() const { return m_underflows; }
    void CountUnderflow() { ++m_underflows; }
    size_t Size()
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_queue.size();
    }
    void Stop()
    {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_stop = true;
        }
        m_cv.notify_all();
        // 读线程要么阻塞在 cv（被 m_stop 唤醒），要么阻塞在 fread（先杀子进程让它拿到 EOF）；
        // 先 join 再关管道 —— 另一个线程还在 fread 时 pclose 同一个 FILE* 是未定义行为
        m_pipe.Kill();
        if (m_thread.joinable()) { m_thread.join(); }
        m_pipe.Close();
    }

private:
    FfmpegPipe&                        m_pipe;
    size_t                             m_chunkBytes;
    size_t                             m_capacity;
    std::thread                        m_thread;
    std::mutex                         m_mutex;
    std::condition_variable            m_cv;
    std::deque<std::vector<uint8_t>>   m_queue;
    bool                               m_eof = false;
    bool                               m_stop = false;
    uint64_t                           m_popped = 0;
    uint64_t                           m_underflows = 0;
};

int64_t ElapsedUs(const std::chrono::steady_clock::time_point& t0)
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now() - t0).count();
}

// 单调时钟（微秒）：PushExternalAudioFrame 的 ptsUs 约定 = 单调时钟域的采集时刻
int64_t MonotonicNowUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// 用同目录的 ffprobe 取首路音频时长（秒），失败返回 0
double ProbeDurationSec(const std::string& ffmpegBin, const std::string& path)
{
    std::string probe = "ffprobe";
    const auto slash = ffmpegBin.rfind('/');
    if (slash != std::string::npos) {
        probe = ffmpegBin.substr(0, slash + 1) + "ffprobe";
    }
    const std::string cmd = probe + " -v error -select_streams a:0 -show_entries format=duration "
                            "-of csv=p=0 " + ShellQuote(path) + " 2>/dev/null";
    FILE* fp = ::popen(cmd.c_str(), "r");
    if (!fp) { return 0.0; }
    char buf[64] = {0};
    const bool ok = std::fgets(buf, sizeof(buf), fp) != nullptr;
    ::pclose(fp);
    return ok ? std::atof(buf) : 0.0;
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
        // 循环播放的音频：首尾 10ms 淡入淡出后用 aloop 循环，避免接缝处波形突变产生咔声
        // （-stream_loop 直接首尾硬拼，短素材如 iqiyi.ts 4.96s 每 5s 一声"咔"，会被误判成混流卡顿）
        auto audioCmd = [&](const std::string& path, int loops) -> std::string {
            const double dur = loops == 1 ? 0.0 : ProbeDurationSec(opt.ffmpegBin, path);
            char b[1024];
            if (dur > 0.1) {
                std::snprintf(b, sizeof(b),
                              "%s -nostdin -loglevel error -i %s -map 0:a:0 "
                              "-af \"aresample=%d,afade=t=in:d=0.01,afade=t=out:st=%.3f:d=0.01,"
                              "aloop=loop=%d:size=2147483647\" -ac %d -f s16le - 2>/dev/null",
                              opt.ffmpegBin.c_str(), ShellQuote(path).c_str(), sr, dur - 0.01,
                              loops <= 0 ? -1 : loops - 1, ch);
            } else {
                std::snprintf(b, sizeof(b),
                              "%s -nostdin -loglevel error %s -i %s -map 0:a:0 "
                              "-ar %d -ac %d -f s16le - 2>/dev/null",
                              opt.ffmpegBin.c_str(), streamLoopArg(loops).c_str(),
                              ShellQuote(path).c_str(), sr, ch);
            }
            return b;
        };
        if (!audio1Pipe.Open(audioCmd(opt.audio1, opt.audio1Loops))) { ITGCapturer::Destroy(capturer); ITGCapturerMgr::UnInit(); return 1; }
        if (!audio2Pipe.Open(audioCmd(opt.audio2, opt.audio2Loops))) { ITGCapturer::Destroy(capturer); ITGCapturerMgr::UnInit(); return 1; }
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
    const int64_t audioUs  = static_cast<int64_t>(1000000.0 * samplesPerPush / sr);  // 一块的样本时长
    const int64_t totalUs  = static_cast<int64_t>(opt.seconds) * 1000000LL;           // 0 = 不限
    // speed>1 → wallclock 触发间隔变小（1s wallclock 推送 speed 秒的采样数据），模拟设备时钟偏快。
    // 步长用浮点、due 按"序号 × 步长"计算：整数步长逐次累加会截断（44.1kHz 下 23219 vs 23219.95us），
    // 等于给 demo 自己注入了 -41ppm 的漂移
    const double vStepUs  = 1000000.0 / opt.fps;
    const double a1StepUs = 1000000.0 * samplesPerPush / sr / std::max(0.001f, opt.audio1Speed);
    const double a2StepUs = 1000000.0 * samplesPerPush / sr / std::max(0.001f, opt.audio2Speed);
    // 主循环最多落后 kMaxLagUs（被调度 / 管道读慢拖住）：超过就丢弃积压、按当前时刻重新对齐，
    // 不再一口气补推（旧实现追帧无上限，一次卡顿后几十帧在 1ms 内灌入，本身就是故障注入）
    const int64_t kMaxLagUs = 200000;

    // 预读 ~500ms：每路一个读线程，主循环非阻塞取块
    const size_t vCap = std::max<size_t>(2, static_cast<size_t>(500000.0 / vStepUs));
    const size_t aCap = std::max<size_t>(4, static_cast<size_t>(500000 / std::max<int64_t>(1, audioUs)));
    Prefetcher videoQ(videoPipe, static_cast<size_t>(frameBytes), vCap);
    Prefetcher audio1Q(audio1Pipe, static_cast<size_t>(pcmChunkBytes), aCap);
    Prefetcher audio2Q(audio2Pipe, static_cast<size_t>(pcmChunkBytes), aCap);
    videoQ.Start();
    if (opt.audio) {
        audio1Q.Start();
        audio2Q.Start();
    }
    // 预热：等各路队列预读到一半再开始计时，避免开头就欠载（最多等 2s）
    for (int i = 0; i < 400; ++i) {
        const bool vOk = videoQ.Size() >= vCap / 2 || videoQ.Eof();
        const bool aOk = !opt.audio || ((audio1Q.Size() >= aCap / 2 || audio1Q.Eof()) &&
                                        (audio2Q.Size() >= aCap / 2 || audio2Q.Eof()));
        if (vOk && aOk) { break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    std::vector<uint8_t> vbuf, a1buf, a2buf;

    // 故障注入状态
    std::mt19937 jitterRng{20261026};
    auto applyPtsInjection = [&](int64_t pts, int frameSeq) -> int64_t {
        int64_t out = pts;
        if (opt.audioPtsJitterMs > 0) {
            std::uniform_int_distribution<int> dist(-opt.audioPtsJitterMs, opt.audioPtsJitterMs);
            out += static_cast<int64_t>(dist(jitterRng)) * 1000LL;
        }
        if (opt.audioPtsBackEvery > 0 && frameSeq > 0 && (frameSeq % opt.audioPtsBackEvery) == 0) {
            out -= audioUs;              // 回退 1 帧时长
        }
        return out < 0 ? 0 : out;
    };
    const int64_t stallStartUs = static_cast<int64_t>(opt.videoStallStartSec) * 1000000LL;
    const int64_t stallEndUs   = stallStartUs + static_cast<int64_t>(opt.videoStallDurSec) * 1000000LL;
    bool stallEnteredLogged = false;
    bool stallExitedLogged  = false;

    const auto t0 = std::chrono::steady_clock::now();
    // ptsUs 约定：单调时钟域的采集时刻。demo 里"采集时刻" = 该块的计划推送时刻（墙钟 due）。
    // 旧实现传"按采样数累加"的 PTS → 时间戳与样本时长恒等，内核测出的漂移恒为 0，补偿永不生效。
    const int64_t monoBaseUs = MonotonicNowUs() - ElapsedUs(t0);
    // 节拍器：due = 序号 × 浮点步长（不累加整数步长，避免截断误差变成人为漂移）
    struct Pacer
    {
        double  stepUs = 0;
        int64_t seq = 0;
        int64_t Due() const { return static_cast<int64_t>(static_cast<double>(seq) * stepUs); }
        void    Advance() { ++seq; }
    };
    Pacer videoP{vStepUs}, audio1P{a1StepUs}, audio2P{a2StepUs};
    int pushedVideo = 0, pushedAudio1 = 0, pushedAudio2 = 0, rejected = 0;
    uint64_t lagResyncs = 0, stallDropped = 0;
    // 推送进度心跳：长压测时需要确认 demo 自身仍在供帧
    const int64_t kDemoBeatUs = 30 * 1000000LL;
    int64_t lastBeatUs = 0;
    int lastPushedVideo = 0, lastPushedAudio1 = 0, lastPushedAudio2 = 0;
    bool videoDone = false, audio1Done = !opt.audio, audio2Done = !opt.audio;

    // 主循环落后过多：把 due 拉到 now（丢弃追赶），返回是否发生了重新对齐
    auto resyncIfLagging = [&](Pacer& p, int64_t now) -> bool {
        const int64_t lag = now - p.Due();
        if (lag > kMaxLagUs) {
            p.seq += static_cast<int64_t>(static_cast<double>(lag) / p.stepUs);   // 丢掉整数个步长，保持相位
            ++lagResyncs;
            return true;
        }
        return false;
    };

    size_t nextSwitch = 0;
    bool audioFileAdded = false;
    while (!errored.load()) {
        if (s_stop.load()) { printf("[demo] 收到退出信号，刷 trailer\n"); break; }
        const int64_t now = ElapsedUs(t0);
        if (totalUs > 0 && now >= totalUs) { break; }
        while (nextSwitch < opt.switchRes.size() &&
               now >= static_cast<int64_t>(opt.switchRes[nextSwitch].first * 1e6)) {
            const std::string& r = opt.switchRes[nextSwitch].second;
            int w = 0, h = 0;
            const int ret = std::sscanf(r.c_str(), "%dx%d", &w, &h) == 2
                                ? capturer->SetVideoResolution(w, h)
                                : capturer->SetResolutionPreset(r);
            printf("[demo] t=%.3fs 切换分辨率 %s ret=%d\n", now / 1e6, r.c_str(), ret);
            ++nextSwitch;
        }
        if (!audioFileAdded && !opt.audioFile.empty() && now >= static_cast<int64_t>(opt.audioFileAt * 1e6)) {
            AudioSourceDesc d;
            d.kind = AudioSourceKind::File;
            d.id = "file";
            d.filePath = opt.audioFile;
            d.volume = opt.audioFileVolume;
            d.loop = opt.audioFileLoop;
            const int ret = capturer->AddAudioSource(d);
            printf("[demo] t=%.3fs 加文件音频源 %s ret=%d\n", now / 1e6, opt.audioFile.c_str(), ret);
            audioFileAdded = true;
        }

        // 心跳：30s/次，每路给出"本窗口帧数"和"累计帧数"，瞬时速率 = Δ/30
        if (now - lastBeatUs >= kDemoBeatUs) {
            const int dv = pushedVideo  - lastPushedVideo;
            const int d1 = pushedAudio1 - lastPushedAudio1;
            const int d2 = pushedAudio2 - lastPushedAudio2;
            printf("[demo] t=%lld.%03llds video+%d(=%d) a1+%d(=%d) a2+%d(=%d) rej=%d "
                   "resync=%llu underflow=v%llu/a%llu/a%llu\n",
                   static_cast<long long>(now / 1000000),
                   static_cast<long long>((now % 1000000) / 1000),
                   dv, pushedVideo, d1, pushedAudio1, d2, pushedAudio2, rejected,
                   static_cast<unsigned long long>(lagResyncs),
                   static_cast<unsigned long long>(videoQ.Underflows()),
                   static_cast<unsigned long long>(audio1Q.Underflows()),
                   static_cast<unsigned long long>(audio2Q.Underflows()));
            std::fflush(stdout);
            lastBeatUs = now;
            lastPushedVideo  = pushedVideo;
            lastPushedAudio1 = pushedAudio1;
            lastPushedAudio2 = pushedAudio2;
        }
        // 视频 EOF 作为整体结束条件（音频无限循环场景下不应等 audio pipe EOF，否则永不退出）
        if (videoDone) { break; }

        // ---- 视频 ----
        const bool inVideoStall = opt.videoStallDurSec > 0 &&
                                  now >= stallStartUs && now < stallEndUs;
        if (inVideoStall && !stallEnteredLogged) {
            printf("[demo][inject] t=%.3fs 进入视频卡顿窗口（dur=%ds mode=%s）\n",
                   now / 1e6, opt.videoStallDurSec, opt.videoStallBurst ? "burst" : "freeze");
            stallEnteredLogged = true;
        }
        if (!inVideoStall && stallEnteredLogged && !stallExitedLogged) {
            printf("[demo][inject] t=%.3fs 退出视频卡顿窗口（freeze 丢弃 %llu 帧）\n", now / 1e6,
                   static_cast<unsigned long long>(stallDropped));
            stallExitedLogged = true;
        }
        if (inVideoStall && !opt.videoStallBurst) {
            // freeze：卡顿期间的帧直接丢失（真实采集卡死），恢复后从当前时刻继续
            while (now >= videoP.Due() && videoQ.TryPop(vbuf)) {
                videoP.Advance();
                ++stallDropped;
            }
        }
        if (!inVideoStall) {
            // burst 模式卡顿结束后不做 resync：积压帧一次性灌入正是要注入的故障
            const bool burstRecovering = opt.videoStallBurst && stallEnteredLogged &&
                                         now - stallEndUs < 2 * kMaxLagUs + opt.videoStallDurSec * 1000000LL;
            if (!burstRecovering) {
                resyncIfLagging(videoP, now);
            }
            while (now >= videoP.Due()) {
                if (!videoQ.TryPop(vbuf)) {
                    if (videoQ.Eof()) { videoDone = true; }
                    else { videoQ.CountUnderflow(); }
                    break;
                }
                if (capturer->PushExternalVideoFrame(vbuf.data(), w, h, w, monoBaseUs + videoP.Due()) == 0) {
                    ++pushedVideo;
                } else {
                    ++rejected;
                }
                videoP.Advance();
                if (!burstRecovering) { break; }   // 正常节奏一次一帧
            }
        }

        // ---- 音频：每路按自身节奏推送，ptsUs = 该块的计划推送时刻（单调时钟） ----
        auto pumpAudio = [&](Prefetcher& q, std::vector<uint8_t>& buf, Pacer& p,
                             int& pushed, bool& done, AudioCapSourceType type, float gain) {
            if (done) { return; }
            resyncIfLagging(p, now);
            while (now >= p.Due()) {
                if (!q.TryPop(buf)) {
                    if (q.Eof()) { done = true; } else { q.CountUnderflow(); }
                    return;
                }
                auto* pcm = reinterpret_cast<int16_t*>(buf.data());
                ScalePcm(pcm, samplesPerPush, ch, gain);
                const int64_t pts = applyPtsInjection(monoBaseUs + p.Due(), pushed);
                if (capturer->PushExternalAudioFrame(type, buf.data(), pcmChunkBytes, sr, ch, pts) == 0) {
                    ++pushed;
                } else {
                    ++rejected;
                }
                p.Advance();
            }
        };
        // 音频 1：原音量，走 Micphone 通道；音频 2：按比例缩音量，走 Audio 通道（内核 mix）
        pumpAudio(audio1Q, a1buf, audio1P, pushedAudio1, audio1Done, TypeOfGameMicphone4Slave, 1.0f);
        pumpAudio(audio2Q, a2buf, audio2P, pushedAudio2, audio2Done, TypeOfGameAudio4Slave, opt.audio2Volume);

        // 睡到最近一个 due（最多 2ms），不空转
        int64_t nextDue = videoP.Due();
        if (!audio1Done) { nextDue = std::min(nextDue, audio1P.Due()); }
        if (!audio2Done) { nextDue = std::min(nextDue, audio2P.Due()); }
        const int64_t waitUs = std::clamp<int64_t>(nextDue - ElapsedUs(t0), 200, 2000);
        std::this_thread::sleep_for(std::chrono::microseconds(waitUs));
    }

    videoQ.Stop();
    audio1Q.Stop();
    audio2Q.Stop();

    capturer->Stop();
    printf("[demo] 输出=%s 视频帧=%d 音频1=%d 音频2=%d 拒绝=%d resync=%llu freeze丢帧=%llu "
           "预读欠载=v%llu/a%llu/a%llu\n",
           opt.out.c_str(), pushedVideo, pushedAudio1, pushedAudio2, rejected,
           static_cast<unsigned long long>(lagResyncs),
           static_cast<unsigned long long>(stallDropped),
           static_cast<unsigned long long>(videoQ.Underflows()),
           static_cast<unsigned long long>(audio1Q.Underflows()),
           static_cast<unsigned long long>(audio2Q.Underflows()));

    ITGCapturer::Destroy(capturer);
    ITGCapturerMgr::UnInit();
    return (pushedVideo > 0 && !errored.load()) ? 0 : 1;
}
