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
#include "platform/windows/WinAudioCapture.h"

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
    bool speaker = false;     // 同时采集扬声器（WASAPI loopback）
    bool fakeVideo = false;   // 用人造帧代替屏幕采集（排查用）
    int monitor = 0;          // 显示器序号（0 = 主显示器）
    std::string mode = "dxgi";  // dxgi | gdi
    uint64_t window = 0;      // 窗口句柄（16 进制），非 0 时采集该窗口
    bool noBorder = false;    // 窗口采集只取客户区（WGC_No_TBar）
    bool listAudio = false;   // 列出系统麦克风/扬声器设备
    std::string perf = "cpu"; // cpu | gpu（全链路 GPU：采集帧不出显存）
    int restart = 0;          // 首轮结束后再 Stop→Prepare→Start 多少轮（重开回归）
    int concur = 0;           // >0 时改跑并发压测：Prepare 与 Stop 交错冲击 n 秒
    int rotation = 0;         // 画面旋转 0/90/180/270
    bool flipH = false;       // 水平镜像
    bool flipV = false;       // 垂直翻转
    int cropW = 0;            // 裁剪（源帧左上角为原点）
    int cropH = 0;
    int logLevel = 0;         // 0=Debug 1=Info 2=Warn 3=Error
    std::string logDir;       // 非空则日志落盘到该目录
    bool printLog = true;     // 是否输出到 stdout
    int logBench = 0;         // >0 时只跑日志吞吐基准然后退出
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
            // 带方向的测试图案：Y 从左暗到右亮 + 从上暗到下亮。
            // 之前是整帧均匀灰度，旋转 / 翻转后画面完全一样，没法验证画面变换是否真的生效。
            const int shift = (index++ * 4) % 16;
            uint8_t* yPlane = buffer.get();
            for (int y = 0; y < m_h; ++y) {
                uint8_t* row = yPlane + static_cast<size_t>(y) * m_w;
                for (int x = 0; x < m_w; ++x) {
                    int v = 16 + (x * 180 / m_w) + (y * 60 / m_h) + shift;
                    row[x] = static_cast<uint8_t>(v > 235 ? 235 : v);
                }
            }
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
           "  --no-audio       关闭音频（纯视频）\n"
           "  --fake-video     用人造帧代替采集（不依赖设备，排查编码/封装链路）\n"
           "  --speaker        同时采集扬声器（WASAPI loopback）\n"
           "  --monitor <n>    显示器序号，默认 0\n"
           "  --mode <dxgi|gdi>  屏幕采集方式，默认 dxgi（不可用时自动回落 gdi）\n"
           "  --window <hwnd>  采集指定窗口（16 进制句柄），不给则采集屏幕\n"
           "  --no-border      窗口采集只取客户区\n"
           "  --list-audio     列出系统麦克风/扬声器设备后退出\n"
           "  --perf <cpu|gpu> 性能模式，默认 cpu；gpu = 全链路 GPU（采集帧不出显存，需硬编）\n"
           "  --gpu            等价 --perf gpu\n"
           "  --restart <n>    首轮结束后再 Stop→Prepare→Start 跑 n 轮（重开回归，每轮时长同 --seconds）\n"
           "  --concur <n>     并发压测：Prepare 与 Stop 两条线程交错冲击 n 秒，验证生命周期互斥\n"
           "  --rotate <deg>   画面旋转，取 0/90/180/270\n"
           "  --flip <h|v|both> 画面翻转：h=水平镜像，v=垂直翻转\n"
           "  --crop <w,h>     从源帧左上角裁剪（如 --crop 960,540）\n"
           "  --log-level <n>  0=Debug 1=Info 2=Warn 3=Error（默认 0）\n"
           "  --log-dir <dir>  日志落盘目录（默认不落盘）\n"
           "  --no-print-log   关闭 stdout 日志输出（配合 --log-dir 只看文件）\n"
           "  --log-bench <n>  只跑日志吞吐基准（打 n 条）后退出\n");
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
        if (key == "--speaker") {
            opt.speaker = true;
            continue;
        }
        if (key == "--no-border") {
            opt.noBorder = true;
            continue;
        }
        if (key == "--list-audio") {
            opt.listAudio = true;
            continue;
        }
        // 无 value 的开关一律在这里处理掉：不能让它们走到下面的 value 分支，
        // 否则会误吞下一个参数；放在命令行末尾时还会因为 i+1 越界被整条跳过。
        if (key == "--gpu") {
            opt.perf = "gpu";
            continue;
        }
        if (key == "--no-audio") {
            opt.audio = false;
            continue;
        }
        if (key == "--fake-video") {
            opt.fakeVideo = true;
            continue;
        }
        if (key == "--no-print-log") {
            opt.printLog = false;
            continue;
        }
        if (key == "--log-bench") {
            opt.logBench = std::stoi(argv[++i]);
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
        } else if (key == "--monitor") {
            opt.monitor = std::stoi(value);
        } else if (key == "--mode") {
            opt.mode = value;
        } else if (key == "--window") {
            opt.window = std::stoull(value, nullptr, 16);
        } else if (key == "--perf") {
            opt.perf = value;
        } else if (key == "--restart") {
            opt.restart = std::stoi(value);
        } else if (key == "--concur") {
            opt.concur = std::stoi(value);
        } else if (key == "--rotate") {
            opt.rotation = std::stoi(value);
        } else if (key == "--flip") {
            opt.flipH = (value == "h" || value == "both");
            opt.flipV = (value == "v" || value == "both");
        } else if (key == "--log-level") {
            opt.logLevel = std::stoi(value);
        } else if (key == "--log-dir") {
            opt.logDir = value;
        } else if (key == "--no-print-log") {
            opt.printLog = false;
        } else if (key == "--crop") {
            const auto comma = value.find(',');
            if (comma != std::string::npos) {
                opt.cropW = std::stoi(value.substr(0, comma));
                opt.cropH = std::stoi(value.substr(comma + 1));
            }
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
    // 无缓冲：日志实时可见（Ctrl+C 中断也不丢）。
    // 代价是每条日志一次 write 系统调用，吞吐约 6 千条/秒；
    // 生产环境要吞吐请改用 --log-dir 落盘或 SetSink 转发（见 README §4.6）。
    setvbuf(stdout, nullptr, _IONBF, 0);

    const Options opt = ParseArgs(argc, argv);
    if (argc > 1 && (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0)) {
        PrintUsage();
        return 0;
    }

    TGCapture::ITGCapturerMgr::Init(opt.logLevel, opt.logDir, opt.printLog);

    // 日志吞吐基准：只打日志不采集，用于回归日志模块本身的性能
    if (opt.logBench > 0) {
        const int n = opt.logBench;
        // 预热，排除首次分配/锁初始化的影响
        for (int i = 0; i < 1000; ++i) {
            TG_LOGI("warmup %d", i);
        }
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < n; ++i) {
            TG_LOGI("bench 第 %d 条日志 payload=%s", i, "0123456789abcdef");
        }
        const auto t1 = std::chrono::steady_clock::now();
        const double us =
            std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        printf("[demo] LOGBENCH n=%d total=%.3fms per=%.3fus throughput=%.0f/s\n", n, us / 1000.0,
               us / n, n / (us / 1e6));
        TGCapture::ITGCapturerMgr::UnInit();
        return 0;
    }

    // 通过兼容层（ESMediaX::ESCapturer）创建，验证老调用方零改动可切换
    ESMediaX::ESCapturer::IESMediaXCapturer* capturer = ESMediaX::ESCapturer::IESMediaXCapturer::Create();
    if (!capturer) {
        printf("创建 capturer 失败\n");
        return -1;
    }
    static_assert(std::is_same_v<decltype(capturer), TGCapture::ITGCapturer*>);

    // --list-audio：列出系统音频设备后退出（不采集）
    if (opt.listAudio) {
        printf("麦克风设备:\n");
        for (const auto& d : TGCapture::platform::ListAudioDevices(true)) {
            wprintf(L"  %ls %ls\n", d.isDefault ? L"[默认]" : L"      ", d.id.c_str());
        }
        printf("扬声器设备:\n");
        for (const auto& d : TGCapture::platform::ListAudioDevices(false)) {
            wprintf(L"  %ls %ls\n", d.isDefault ? L"[默认]" : L"      ", d.id.c_str());
        }
        TGCapture::ITGCapturer::Destroy(capturer);
        TGCapture::ITGCapturerMgr::UnInit();
        return 0;
    }

    // 视频源：屏幕 / 窗口（--fake-video 时用人造帧）
    TGCapture::IVideoSource* video = nullptr;
    std::shared_ptr<FakeVideoSource> fake;
    if (opt.fakeVideo) {
        fake = std::make_shared<FakeVideoSource>();
        video = fake.get();
    } else if (opt.window != 0) {
        if (!capturer->IsSupported(TGCapture::CaptureFeature::Window)) {
            printf("当前平台不支持窗口采集\n");
            TGCapture::ITGCapturer::Destroy(capturer);
            return -2;
        }
        video = capturer->CreateWindowCapture(reinterpret_cast<void*>(opt.window),
                                              opt.noBorder ? TGCapture::WGC_No_TBar : TGCapture::WGC);
    } else {
        video = capturer->CreateScreenCapture(opt.monitor,
                                              opt.mode == "gdi" ? TGCapture::GDI : TGCapture::DXGI);
    }
    if (!video || capturer->AddVideoSource(video) != 0) {
        printf("添加视频源失败\n");
        TGCapture::ITGCapturer::Destroy(capturer);
        return -2;
    }

    // 音频源：系统麦克风（+ 可选的扬声器 loopback，两路会自动混音）
    if (opt.audio) {
        capturer->EnableMicrophoneCapture();
    }
    if (opt.speaker) {
        capturer->EnableSpeakerCapture();
    }

    TGCapture::VideoEncoderParams vParams;
    vParams.width = opt.width;
    vParams.height = opt.height;
    vParams.fps = static_cast<float>(opt.fps);
    vParams.vBitrate = opt.bitrate;
    vParams.gop = opt.fps * 2;
    vParams.vCodec = opt.vcodec;
    vParams.vEncoderType = (opt.vcodec == "h264_nvenc") ? TGCapture::h264_nvenc : TGCapture::libopenh264;
    // 性能模式：gpu = 采集帧留在显存直送硬编（自动选择编码器时同样生效）
    vParams.perfMode = (opt.perf == "gpu") ? TGCapture::PerformanceMode::Gpu : TGCapture::PerformanceMode::Cpu;
    if (vParams.perfMode == TGCapture::PerformanceMode::Gpu && opt.vcodec == "libopenh264") {
        printf("[demo] GPU 模式需要硬编，自动改用 h264_nvenc\n");
        vParams.vCodec = "h264_nvenc";
        vParams.vEncoderType = TGCapture::h264_nvenc;
    }
    vParams.videoRotation = opt.rotation;
    vParams.videoFlipHorizontal = opt.flipH;
    vParams.videoFlipVertical = opt.flipV;
    vParams.videoCropWidth = opt.cropW;
    vParams.videoCropHeight = opt.cropH;
    printf("[demo] 性能模式: %s  编码器: %s\n", opt.perf.c_str(), vParams.vCodec.c_str());
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

    // 并发压测：Prepare 与 Stop 交错冲击，验证生命周期互斥（不死锁、不野线程、不崩溃）
    if (opt.concur > 0) {
        printf("[demo] 并发压测 %d 秒（Prepare / Stop 交错）\n", opt.concur);
        std::atomic_bool done{false};
        std::atomic_int okPrepare{0};
        std::atomic_int okStop{0};
        std::jthread tPrepare([&] {
            while (!done.load()) {
                if (capturer->Prepare() == 0) {
                    okPrepare.fetch_add(1);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });
        std::jthread tStop([&] {
            while (!done.load()) {
                capturer->Stop();
                okStop.fetch_add(1);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });
        std::this_thread::sleep_for(std::chrono::seconds(opt.concur));
        done.store(true);
        tPrepare.join();
        tStop.join();
        printf("[demo] 并发结束 prepare_ok=%d stop=%d\n", okPrepare.load(), okStop.load());
        capturer->Stop();
        TGCapture::ITGCapturer::Destroy(capturer);
        TGCapture::ITGCapturerMgr::UnInit();
        printf("[demo] 并发压测完成\n");
        return 0;
    }

    // 重开回归：Stop → Prepare → Start 反复跑，验证队列/时钟/统计都正确复位
    for (int r = 1; r <= opt.restart; ++r) {
        printf("[demo] === 重开 #%d ===\n", r);
        const int pr = capturer->Prepare();
        printf("[demo] 重开 Prepare=%d\n", pr);
        if (pr != 0) {
            printf("[demo] 重开失败，停止后续轮次\n");
            break;
        }
        for (int i = 0; i < opt.seconds * 10; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        printf("[demo] 重开统计: %s\n", capturer->GetStatsInfo(TGCapture::StatsInfoType::videoInfo).c_str());
        capturer->Stop();
    }

    TGCapture::ITGCapturer::Destroy(capturer);
    TGCapture::ITGCapturerMgr::UnInit();
    printf("[demo] 完成，输出文件: %s\n", opt.output.c_str());
    return 0;
}
