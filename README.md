# TGCapture —— 开播内核（C++20）

用 C++20 重写的开播（采集 → 编码 → 封装 → 推流）内核，**对外接口保持不变**，
仍然基于 FFmpeg API 实现，支持 Windows / Linux / Android / iOS。
（Linux 目前**只有内核、没有原生采集**，见 §11。）

采集、混流、同步、性能模式都是**可独立阅读的模块**，依赖 `TGCapture3rd`
（由 `FfmpegCompileShell` 编译产出：ffmpeg / openssl / srt / fdk-aac / openh264 / libyuv / zlib）。

---

## 1. 快速开始（Windows，5 分钟）

```powershell
# ① 依赖（只需一次）：用 FfmpegCompileShell 编出 TGCapture3rd
cd ..\FfmpegCompileShell
python build.py build --platform windows -o D:\bertonwang\bing\TGCapture3rd

# ② 构建本工程
cd D:\bertonwang\bing\RefactorMediaX
python scripts/build.py --platform windows --abi x64

# ③ 录 3 秒屏幕到本地文件
cd build\windows-x64\Release
.\tg_capture_demo.exe --out out.flv --seconds 3

# ④ 验证产物（ffmpeg 在 TGCapture3rd\ffmpeg\windows\bin）
$env:PATH = "D:\bertonwang\bing\TGCapture3rd\ffmpeg\windows\bin;" + $env:PATH
ffprobe -v error -show_entries stream=codec_name,width,height -of csv=p=0 out.flv
# => h264,1280,720 / aac,44100,2
ffplay out.flv
```

> 构建时 ffmpeg 的 DLL 已自动拷到 exe 同目录，直接双击 exe 也能跑。

---

## 2. 功能总览

| 维度 | 能力 |
| - | - |
| 采集（系统原生，不接 GVoice / 第三方 SDK） | 屏幕（Win `DXGI`/`GDI`、Android `MediaProjection`、iOS `ReplayKit`）、窗口（仅 Win `WGC`/`WGC_No_TBar`）、麦克风（Win `WASAPI`、Android `AudioRecord`、iOS `AVAudioEngine`）、扬声器/系统内录（Win loopback、Android `AudioPlaybackCapture` API 29+） |
| 多源混流 | N 路按采样点加和；单路也走混流器（享受时钟校正）；某路掉线按静音补齐 |
| 时钟校正 | 每路独立：系统时钟跳变限幅 → ForceAlign（>5 帧）→ BURST 收敛（>3 帧）→ PI 微调 |
| 音画同步 | 音频为主时钟；视频 PTS 由音频反推；停滞 clamp、渐进追赶、封装交织自愈 |
| 编码 | 软编 `libopenh264` + `libfdk_aac`；硬编 `h264_nvenc` / `h264_amf` / `h264_mf` / `h264_mediacodec` / `h264_videotoolbox` |
| 封装 | `flv`（rtmp/rtmps/文件）、`mpegts`（srt/文件）；协议 file / http(s) / rtmp(s) / tls / srt |
| 性能模式 | `Cpu`（默认，内存帧 + CPU 缩放）、`Gpu`（全链路 GPU：显存直拷硬编） |
| 兼容层 | `compat/ESMediaXCapturer.h`：`ESMediaX::ESCapturer::IESMediaXCapturer` 换一行 include 即可 |
| 生命周期 | 状态机 + 回调；`Prepare`/`Start`/`Stop` 并发安全；支持同一实例反复重开（见 4.7） |
| 日志 | 自带 `core/Logger.h`（零第三方依赖），level 分级、限频、接管 ffmpeg 日志、sink 转发宿主（见 4.6） |

---

## 3. 目录

```
TGCapture/
├── include/TGCapture/          对外接口（+ compat 兼容层）
├── src/
│   ├── core/                   Frame / Bus / Thread / Queue / Clock / Logger / MediaTypes
│   │                           + AVSyncClock（音画同步） + AudioMixer（混流/PI·BURST）
│   ├── codec/                  VideoEncoder / AudioEncoder（软编 + 硬编直连）
│   ├── capture/                VideoSource / AudioSource 抽象
│   ├── mux/                    Muxer（flv/rtmp、mpegts/srt、文件）
│   ├── platform/windows/       屏幕(DXGI/GDI) + 窗口 + WASAPI 麦克风/扬声器
│   ├── platform/android/       MediaProjection 屏幕 + AudioRecord/内录（JNI 桥接）
│   ├── platform/ios/           ReplayKit 屏幕 + AVAudioEngine 麦克风（.mm）
│   ├── TGCapturer.cpp          门面（状态机、回调、统计、背压）
│   └── ITGCapturer.cpp         Create/Destroy/Init/UnInit
├── demo/windows/               控制台测试 demo（第 10 节）
├── cmake/Deps.cmake            TGCapture3rd 依赖定位
├── ide/android/                Android Studio 宿主工程（gradle + externalNativeBuild）
├── CMakeLists.txt
└── scripts/build.py            构建脚本（windows/android/ios，含 --ide）
```

数据流（总线串联，节点间有界队列 + 背压）：

```
VideoSource ─┐
             ├─> VideoEncoder ─┐
AudioSource ─┴─> AudioMixer ─> AudioEncoder ─┴─> Muxer ─> rtmp://  srt://  file
                     ▲
              音频主时钟 ──> VideoPtsGenerator ──> 视频 PTS
```

---

## 4. 使用方式

### 4.1 最小接入示例

```cpp
#include "TGCapture/ITGCapturer.h"
// 老工程可直接 include "TGCapture/compat/ESMediaXCapturer.h"，其余代码不动

TGCapture::ITGCapturerMgr::Init(1);                       // 0=Debug 1=Info 2=Warn 3=Error
auto* capturer = TGCapture::ITGCapturer::Create();

// ① 采集源（Start 之前配置）
capturer->CreateScreenCapture(0, TGCapture::DXGI);        // 屏幕
capturer->EnableMicrophoneCapture();                      // 麦克风（系统采集）
capturer->EnableSpeakerCapture();                         // 扬声器（可同时开，自动混音）

// ② 参数
TGCapture::VideoEncoderParams vp;
vp.width = 1280; vp.height = 720; vp.fps = 30; vp.vBitrate = 3000000;
vp.vCodec = "libopenh264";                                // 或 h264_nvenc / h264_amf ...
vp.perfMode = TGCapture::PerformanceMode::Cpu;            // 或 Gpu（见第 7 节）
capturer->SetupVideoEncoder(vp);

TGCapture::AudioEncoderParams ap;                         // enabled=false 即纯视频推流
capturer->SetupAudioEncoder(ap);

TGCapture::MuxerParams mp;
mp.muxer = "flv";                                         // 或 mpegts
mp.muxerOutput = "rtmp://127.0.0.1/live/stream";          // 也支持 out.flv / srt://
capturer->SetupMuxer(mp);

// ③ 回调
capturer->SetOnPreparedCallback([](TGCapture::ITGCapturer* c) { c->Start(); });
capturer->SetOnErrorCallback([](int32_t what, int32_t extra, const char* msg) {
    printf("error what=%d extra=%d msg=%s\n", what, extra, msg);
});

// ④ 生命周期
capturer->Prepare();       // 成功后触发 OnPrepared
// ... 运行 ...
capturer->Stop();
TGCapture::ITGCapturer::Destroy(capturer);
TGCapture::ITGCapturerMgr::UnInit();
```

### 4.2 生命周期与回调

| 阶段 | 方法 | 说明 |
| - | - | - |
| 初始化 | `ITGCapturerMgr::Init(logLevel, logDir, printLog)` | 全局一次 |
| 配置 | `Create*Capture` / `Enable*Capture` / `Setup*Encoder` / `SetupMuxer` | `Prepare` 之前 |
| 就绪 | `Prepare()` → `OnPrepared` | 建编码器、开采集、建封装上下文 |
| 运行 | `Start()` → `OnStart` | 起各路线程 |
| 停止 | `Stop()` → `OnStop` | 逆序停采集 → drain 编码 → 收尾封装 |
| 运行期 | `MuteAudio` / `MuteVideo` / `GetStatsInfo` / `GetTrafficStats` | — |
| 销毁 | `Destroy()` + `ITGCapturerMgr::UnInit()` | — |

### 4.3 采集能力（三端）

全部走**系统原生采集**，不接 GVoice、不接任何第三方采集 SDK。

| 能力 | Windows | Android | iOS | 说明 |
| - | - | - | - | - |
| 屏幕录制 | ✅ `DXGI` / `GDI` | ✅ `AndroidSurface` | ✅ `ReplayKit` | Win 默认 DXGI Desktop Duplication，不可用自动回落 GDI |
| 指定窗口 | ✅ `WGC` / `WGC_No_TBar` | ❌ | ❌ | Android/iOS 没有"窗口"概念 |
| 麦克风 | ✅ WASAPI | ✅ AudioRecord | ✅ AVAudioEngine | 系统默认设备；Win 可指定端点 |
| 扬声器 / 系统内录 | ✅ WASAPI loopback | ✅ AudioPlaybackCapture（API 29+） | ❌ | iOS 需 Broadcast Upload Extension |
| 多路同时采集 | ✅ | ✅ | — | 麦克风 + 扬声器，内核按采样点加和 |

不支持的能力**不再静默失败**：`IsSupported()` 返回 false、调用返回负值。

```cpp
capturer->IsSupported(TGCapture::CaptureFeature::Screen);      // 屏幕
capturer->IsSupported(TGCapture::CaptureFeature::Window);      // 窗口
capturer->IsSupported(TGCapture::CaptureFeature::Microphone);  // 麦克风
capturer->IsSupported(TGCapture::CaptureFeature::Speaker);     // 扬声器/系统内录
```

**Windows**

| 项 | 实现 |
| - | - |
| 屏幕 | `DXGI`：D3D11 + `IDXGIOutputDuplication`（GPU 给帧、不被遮挡、支持多显示器 `CreateScreenCapture(index)`）；失败回落 `GDI`（BitBlt） |
| 窗口 | `CreateWindowCapture(hwnd, WGC)` 整窗；`WGC_No_TBar` 只取客户区；遮挡/最小化用 `PrintWindow(PW_RENDERFULLCONTENT)` 兜底 |
| 麦克风 / 扬声器 | WASAPI `eCapture` / `eRender + AUDCLNT_STREAMFLAGS_LOOPBACK`；`WinAudioCapture::SetDeviceId()` 指定设备 |
| 设备枚举 | `platform::ListAudioDevices(bool capture)` 返回端点 id |
| 帧率稳定 | 桌面无变化时复用上一帧，直播不断流 |
| 健壮性 | 设备失效/无数据时转静音兜底，音频时间轴不断 |

**Android**：屏幕与系统内录依赖 `MediaProjection`，只能由 App 弹系统授权框获取。

```java
startActivityForResult(mgr.createScreenCaptureIntent(), REQ);
// 授权通过后：
capturer.SetPrivateData("android.JavaVM", vm);
capturer.SetPrivateData("android.MediaProjection", projection);
// 之后照常：EnableMicrophoneCapture / EnableSpeakerCapture / CreateScreenCapture(0, AndroidSurface)
```

Java 桥接在 `ide/android/.../TGCaptureSystemCapture.java`（VirtualDisplay + ImageReader + AudioRecord/AudioPlaybackCapture）；
绑定编码器输入 Surface（`SetEncoderSurface`）时走零拷贝，C++ 不搬运像素。

**iOS**：`RPScreenRecorder.startCaptureWithHandler`（iOS 11+），CVPixelBuffer 零拷贝直送 VideoToolbox；
麦克风走 `AVAudioSession(playAndRecord)` + `AVAudioEngine` tap。
`RPScreenRecorder` 只能录**本 App** 画面（系统限制）；录全系统请在宿主 App 加 Broadcast Upload Extension，
再用 `PushExternalVideoFrame` / `PushExternalAudioFrame` 把帧送进内核。

### 4.4 外部注入帧（游戏引擎 / 平台扩展）

```cpp
// 视频：data 为 NV12/I420 系统内存；pixelBuffer 为 CVPixelBufferRef(iOS)/硬件句柄时走零拷贝
capturer->PushExternalVideoFrame(data, w, h, stride, ptsUs, pixelBuffer);
// 音频：交错 S16
capturer->PushExternalAudioFrame(TGCapture::TypeOfGameMicphone4Slave, pcm, bytes, sampleRate, channels, ptsUs);
```

**两个容易踩的语义点**

- `pixelBuffer` 的硬件载体类型**由平台决定**（iOS → `CVPixelBuffer`，Windows → `D3D11Texture`），
  同一个入口不支持跨平台混传；非这两个平台传入 `pixelBuffer` 会返回 `-4` 并打 Warn。
- `ptsUs` **默认不生效**：只要音频开着，视频 PTS 一律由音频主时钟反推（这是开播场景的正确做法），
  传入值只在"无音频 / 音频未就绪"时兜底。
  调用方若自带完整时间轴（如游戏引擎已做过音画同步），把
  `VideoEncoderParams::videoUseExternalPts` 置 `true`，内核就不再改写 PTS，
  音画同步责任随之转移到调用方。
- 外部注入的音频会**独占一条混流通道**（追加在内部源之后），
  不会和 0 号麦克风共用同一个通道时钟。

### 4.5 参数总表

**`VideoEncoderParams`**

| 字段 | 默认 | 说明 |
| - | - | - |
| `width` / `height` / `fps` | 1280 / 720 / 30 | 推流分辨率与帧率 |
| `vCodec` | `libopenh264` | 也支持 `h264_nvenc` / `h264_amf` / `h264_mf` / `h264_mediacodec` / `h264_videotoolbox` |
| `vBitrate` / `vBitrateMaxFactor` / `gop` | 3000000 / 3.0 / 15 | 码率与 GOP |
| `threads` | 4 | 软编线程数 |
| `perfMode` | `Cpu` | `Gpu` = 全链路 GPU（第 7 节） |
| `videoMaxLeadUs` | 500000 | 音频停滞时视频最多超前多少 |
| `videoMaxLagUs` | 0（自动） | 视频落后音频超过多少丢帧（编码器侧兜底）。`0` = 按 `forceSyncFactor × 帧间隔 × 1.5` 推导，保证不落在 RangeAdapt 追赶区间内 |
| `audioCompensationUs` | 0 | 音频参考时钟额外补偿 |

**`AudioEncoderParams`**

| 字段 | 默认 | 说明 |
| - | - | - |
| `sampleRate` / `channels` | 44100 / 2 | 输出格式 |
| `aCodec` | `libfdk_aac` | 也支持 `aac` |
| `aBitrate` | 128000 | 音频码率 |
| `enabled` | `true` | `false` = 纯视频（不建音频编码器与音频流） |

**`MuxerParams`**

| 字段 | 默认 | 说明 |
| - | - | - |
| `muxer` | `flv` | `flv` / `mpegts` |
| `muxerOutput` | — | `rtmp://` / `rtmps://` / `srt://` / 文件路径 |
| `interleaveMs` | 350 | 音视频交织阈值，0 = 关闭 |

### 4.6 日志

自带 `core/Logger.h`，**不依赖 spdlog**（少一个第三方依赖）。

```cpp
TGCapture::ITGCapturerMgr::Init(1);                 // 全局 level
// core/Logger.h 是内部头文件（源码构建时可见），宿主工程也可只用它做转发声明
#include "core/Logger.h"
TGCapture::core::Logger::Instance().SetSink(
    [](TGCapture::core::LogLevel lv, const char* msg) { /* 转发到宿主日志系统 */ });
```

**输出三路**（可叠加）

```cpp
// logLevel: 0=Debug 1=Info 2=Warn 3=Error
// logDir:   非空则落盘到该目录（会自动创建）；空则不落盘
// printLog: 是否输出到 stdout（设了 sink 后自动不再往 stdout 打）
ITGCapturerMgr::Init(logLevel, logDir, printLog);
```

| 输出 | 条件 | 格式 |
| - | - | - |
| sink | `SetSink()` 设置了 | 宿主收到 `(level, 原始 msg)`，**不带前缀**，由宿主自行格式化 |
| stdout | `printLog=true` 且未设 sink | `[TGCapture][时间][线程][级别] msg` |
| 文件 | `logDir` 非空 | 同 stdout 格式，按大小轮转 |

**前缀示例**（时间戳精确到毫秒，线程 id 用于区分采集/编码/封装线程）：

```
[TGCapture][2026-10-22 14:29:01.696][7184][I] TGCapturer: 已启动
[TGCapture][2026-10-22 14:29:01.588][60036][D] VideoEncoder[#0]: 编码 1280x720 pts=0
[TGCapture][2026-10-22 14:29:01.617][64406][D] Muxer: 写包 stream=0 pts=0 size=4255
```

**日志文件**：`<logDir>/tgcapture_<YYYYMMDD_HHMMSS>.log`，单个超过 32MB 切
`..._1.log`、`..._2.log`…。`UnInit()` 会关闭文件。

**分级**：只有运行时一档，**没有编译期裁剪**——
Debug 日志在 Release 里也保留，好处是线上能临时打开排查。

| `logLevel` | 级别 | 内容 | ffmpeg 侧 |
| - | - | - | - |
| 0 | Debug | 每帧级追踪（线程进出、前几帧编码、写包） | `AV_LOG_INFO` |
| 1 | Info（默认） | 生命周期、就绪、收尾统计 | `AV_LOG_INFO` |
| 2 | Warn | 只保留异常与降级 | `AV_LOG_WARNING` |
| 3 | Error | 只保留失败 | `AV_LOG_ERROR` |

实测四档（1 秒录制）：`0 → D27/I10/W2`，`1 → D0/I10/W2`，`2 → D0/I0/W2`，`3 → D0/I0/W0`。

**异常可读性**

- FFmpeg 错误一律带可读原因：`core/AvError.h` 的 `AvErr(ret)`
  （`av_err2str` 在 MSVC 下是 compound literal 编译不过，故手写等价实现）。
  一条日志里只能用一次 `AvErr()`。
- FFmpeg 自身日志由 `av_log_set_callback` 接管，转发进 `Logger` 并加 `[ffmpeg]` 前缀
  （默认走 stderr，Android 上等于丢弃）。

**限频**

高频路径（丢帧、写包失败、混流取不到数据）用 `TG_LOGW_THROTTLED` / `TG_LOGE_THROTTLED`：
同一调用点在一个时间窗口内最多输出一条，既不静默也不刷屏。

**已补日志的静默路径**：编码器背压丢帧、PTS 回退丢帧、视频落后音频丢帧、
`send_frame` / `receive_packet` 失败、drain 收包失败、混流取不到数据、音频通道时钟漂移。

**线程安全**：`SetSink` / `Log` / 落盘 共用一把锁，且回调前先拷一份再解锁
（宿主日志系统反向调回来不会死锁）。`UnInit()` 会关闭日志文件、复位 level / sink
并摘掉 av_log 回调。

**当前限制**：
- Android / iOS 还没接 `__android_log_print` / `os_log` sink（移动端需先用 `SetSink` 转发）
- 落盘用 `<filesystem>`，很老的 NDK / GCC 可能需要额外链接
- 日志文件只按大小轮转，不按天数、不自动清理旧文件
- 无异步写：日志在调用线程同步落盘，极端高频下会阻塞调用方

### 4.6.1 性能

`--log-bench <n>` 可复现（只打日志不采集，预热 1000 条后计时）：

| 输出模式 | 每条 | 吞吐 |
| - | - | - |
| 仅格式化（关卡全闭） | 0.34 us | ~296 万 /s |
| **落盘（`--log-dir`）** | **1.58 us** | **~63 万 /s** |
| stdout（无缓冲，demo 默认） | 227 us | ~4.4 千 /s |
| stdout（全缓冲 64KB） | 32.6 us | ~3 万 /s |

**结论：日志模块本身很快，慢的是 stdout。**

无缓冲 stdout 下每条日志一次 `write` 系统调用，比落盘慢 **140 倍**。所以：

- 生产/长时间录制请用 **`--log-dir` 落盘** 或 **`SetSink` 转发**，别依赖 stdout
- 确实要打 stdout 时，把 stdout 设成全缓冲（`setvbuf(..., _IOFBF, ...)`）可提速约 5 倍
- Warn/Error 会立即 `fflush`，普通日志交给 CRT 缓冲 —— 关键信息不会因为缓冲丢

另外要注意 **Debug 档会打每帧级日志（编码/写包各一条）**，30fps 下每秒上百条；
落盘无压力，走无缓冲 stdout 就会成为瓶颈。

### 4.6.2 与 spdlog 的对比

| 维度 | 本工程 `core/Logger.h` | spdlog |
| - | - | - |
| 依赖 | **零**（只用 std，header-only） | 需 fmt（可内置，header-only 或编译版） |
| 格式化 | `vsnprintf`（C 风格） | fmt（类型安全、编译期格式串检查） |
| 级别 | 4 档，运行时 | 7 档 + **`SPDLOG_ACTIVE_LEVEL` 编译期裁剪** |
| 异步 | 无（同步写） | **有**（线程池 + MPMC 队列，可无锁） |
| sink | 单 sink（`std::function`） | **多 sink**，内置 file / rotating / daily / android / syslog… |
| 轮转 | 按大小（自实现） | 按大小 + 按天 |
| 落盘吞吐 | ~63 万 /s | 同步文件 sink 同量级；异步模式更高且不阻塞调用方 |
| 回溯栈 | 无 | 有（`SPDLOG_ENABLE_BACKTRACE`） |
| 自定义格式 | 无 | `pattern_formatter` |
| 二进制体积 | 小 | 引入 fmt 会增大 |

**怎么选**

- **保持现状**：日志量不大（Info 档每秒几十条）、不想引入第三方依赖、
  且已经用 `SetSink` 转给宿主日志系统。把 stdout 换成就地落盘即可。
- **换 spdlog**：需要 *异步不阻塞调用方* / *多 sink* / *编译期裁剪* / *类型安全格式串*
  中任意一项。此时本模块退化成一层薄封装，把 `Sink` 接到 spdlog 即可，改动面很小。

> 表中 spdlog 一侧的能力是事实；性能是公开基准的常见量级，
> 真要替换建议用自己的场景跑一次 `--log-bench` 对照。

### 4.7 线程模型、并发与重开

**线程清单**（`Start()` 起、`Stop()` 停，均为 `std::jthread` + `stop_token`）

| 线程 | 载体 | 职责 |
| - | - | - |
| 视频采集 | `VideoSourceBase`（各平台） | 出视频帧，回调 `OnVideoFrame` |
| 音频采集 ×N | `AudioSourceBase`（各平台） | 出 PCM，进 `AudioMixer` |
| 音频混流 | `TGCapturer::MixLoop` | `AudioMixer::Mix` 产一路 PCM，**并推进音频主时钟** |
| 视频编码 | `VideoEncoder` worker | 编码 + 丢帧保护 |
| 音频编码 | `AudioEncoder` worker | 编码 |
| 封装 | `Muxer` worker | 交织写包 / 推流 |

停止严格**逆数据流**：采集 → 混流 → 编码器 → 封装，每一步都 `join`，
保证不再有回调后才收尾下一步（尾部包不丢）。

**并发约束**

- `Prepare()` / `Start()` / `Stop()` / `ReleaseAll()` 由一把 `std::recursive_mutex` 串行化，
  状态转换用 `compare_exchange`，并发调用只有一个能生效。
- **必须用 recursive_mutex**：`Prepare()` 内部会触发 `m_onPrepared` 回调，
  而宿主的标准用法正是在回调里调 `Start()`（demo 就是这么写的），
  同线程重入，普通 `std::mutex` 会自死锁。
- 帧回调（`OnVideoFrame` / `OnAudioFrameFrom`）与 `MixLoop` **不取锁**，
  只查 `m_state` 原子量 —— 否则停止流程 `join` 时会死锁。
- 采集/编码/封装之间的队列本身是线程安全的（`BoundedQueue` 内部加锁）。

**重开（Stop → Prepare → Start）**

支持同一实例反复启停，并且每轮都会完整复位：队列重开、编码器统计、音频主时钟、
混流器、首帧标志、封装交织基准。

> 维护者注意：`BoundedQueue::Close()` 是**永久关闭**语义（用来唤醒阻塞的消费者），
> 重开必须显式 `Reopen()`。各模块的 `Open()` 已经调用，新增队列时别漏。

验证：`--restart <n>`（重开回归）、`--concur <n>`（Prepare/Stop 并发压测），见 §10。

### 4.8 画面变换（旋转 / 翻转 / 裁剪）

模块：**`src/core/VideoTransform.{h,cpp}`**（libyuv 实现）。

画面来源千差万别（横竖屏、镜像、局部区域、采集分辨率 ≠ 推流分辨率）。
之前这些能力散落在各平台实现里，旋转 / 翻转 / 裁剪更是无处安放，
外部注入帧还会绕过全部平台实现直接进总线。现在收敛成**集中归一化层**，
所有视频帧在进入编码器之前都过一遍。

**处理顺序固定为**

```
crop(裁剪) → flip(翻转) → rotate(旋转) → scale(缩放到编码分辨率) → convert(转像素格式)
```

90 / 270 度旋转会交换宽高。裁剪矩形在**源帧坐标系**上描述，会自动夹到画面内并对齐到偶数。

**参数**（`VideoEncoderParams`，全部留默认即不做变换，零额外开销）

| 字段 | 默认 | 说明 |
| - | - | - |
| `videoRotation` | 0 | 顺时针旋转，只取 0 / 90 / 180 / 270 |
| `videoFlipHorizontal` | false | 水平镜像 |
| `videoFlipVertical` | false | 垂直翻转 |
| `videoCropX` / `videoCropY` | 0 | 裁剪原点 |
| `videoCropWidth` / `videoCropHeight` | 0 | 裁剪尺寸，<= 0 表示不裁剪 |
| `gpuAllowReadbackFallback` | true | GPU 模式下需要变换时，是否允许回读显存处理 |

```cpp
TGCapture::VideoEncoderParams vParams;
vParams.videoRotation = 90;
vParams.videoFlipHorizontal = true;
capturer->SetupVideoEncoder(vParams);
```

demo：`--rotate <0|90|180|270>` / `--flip <h|v|both>` / `--crop <w,h>`。

**GPU 模式下的代价（重要）**

纯显存的 `CopySubresourceRegion` 只能做 1:1 拷贝，带不了旋转 / 翻转 / 缩放。
所以 **GPU 模式一旦开变换，就只能在显存里做不了，必须把纹理读回内存用 libyuv 处理**：

- 每帧几十 MB 的回读 + CPU 变换，**帧率会大幅下降**
  （实测 2160x3840 采集 + 旋转，1 秒只能出 1 帧，基本不可用）
- 启动时会打一条 Warn 提示
- 把 `gpuAllowReadbackFallback` 设为 `false` 可彻底杜绝回读，
  代价是这些帧会被直接丢弃（不出画面）

**GPU 直连只对"屏幕"成立**：Desktop Duplication 复制的是整个显示器，
窗口采集拿不到对应的复制通道，强行走 GPU 会抓到整屏而不是目标窗口。
`Prepare()` 里会用 `VideoSourceBase::SupportsGpuDirect()` 判断，不满足则自动回落 CPU 并打 Warn。
反过来，若编码器没能建起硬件链路（`hwdevice` / `hw_frames_ctx` 初始化失败），
采集侧也会一并切回 CPU —— 否则采集继续出 GPU 纹理帧、编码器走软件路径，
结果是"流能开、包在写、画面全黑"。

需要 GPU 全链路性能时，请不要开变换；或者后续改用 `ID3D11VideoProcessor` /
渲染 quad 的方式在显存内完成变换（当前未实现）。

**当前不支持**：DPI 缩放（高 DPI 屏仍是逻辑像素/物理像素混用）、HDR / 广色域、
旋转角写入码流（`Frame::rotation` 未写进 SPS VUI 或 displaymatrix）。

---

## 5. 音画同步

模块：**`src/core/AVSyncClock.{h,cpp}`** —— 只算时间戳，不碰采集/编码/封装，可单独阅读与测试。
策略对齐 `MediaX` 的 `VideoPtsManager` + `AudioTimestampManager`。

**一句话：音频是主时钟，视频 PTS 由音频时钟反推，不使用采集 wall clock。**
（采集时钟与音频时钟不是一个时钟域，直接混用会让 PTS 跳变、污染封装。）

```
音频采集 → AudioMixer（按采样数推进 + PI/BURST 校正）──► 音频 PTS（主时钟）
                                                          │
                             VideoPtsGenerator::Next(audioRef) ──► 视频 PTS
                                                          │
                                          Muxer 交织（领先 350ms 暂停写，超时自愈）
```

**与 MediaX 的对齐情况**

| MediaX 模块 / 档位 | 本工程 | 状态 |
| - | - | - |
| `AudioTimestampManager`（音频即主时钟，按采样数推进） | `AudioClock` + `AudioMixer` 输出时间轴 | 已对齐 |
| `ReferenceClockManager`（系统时钟跳变限幅 + PI/BURST） | `AudioChannelClock` | 已对齐 |
| `VideoPtsManager` · RangeNormal / RangeAdapt / RangeCatchUp | `VideoPtsGenerator` 三档（见 5.1） | 已对齐 |
| `VideoPtsManager` · ForceAlign | 偏离 > 6×帧间隔时跳齐 | 已对齐（阈值更激进：6 帧 vs MediaX 5 帧量级） |
| `VideoPtsManager` · Pause | 音频停滞 → 视频封顶 `maxLead` | 已对齐 |
| BreakV / BreakA 交织自愈 | `Muxer` 交织 + stall 超时推基准 | 已对齐 |
| 暂停 / 恢复独立档位 | 无，靠 ForceAlign 跳齐兜底 | **差异**（可接受：muted 时不推进 PTS，恢复时跳齐） |

### 5.1 视频 PTS 生成规则

`avDiff = 音频 PTS − 视频 PTS`，**>0 表示视频落后**。分档对齐 `VideoPtsManager`：

| 档位 | 触发条件（30fps 示例） | 处理 |
| - | - | - |
| **RangeNormal** | \|avDiff\| ≤ 2×帧间隔（≤66ms） | 按帧间隔正常递增 |
| **RangeAdapt** | 2× ~ 6×帧间隔（66~200ms） | 渐进追赶 / 减速：落后 150/300/500ms → ×1.1/1.2/1.3；超前 → ×0.9/0.8/0.7 |
| **ForceAlign** | > 6×帧间隔（>200ms） | 直接跳齐到音频 PTS（**不允许回退**） |
| **Pause** | 音频连续 2 拍不推进 | 视频按系统时钟继续走，最多超前 `videoMaxLeadUs`（500ms）后钉住；补帧连续被 clamp 3 次则停产出 |
| 首帧 | — | 取音频 PTS；超过 5s 视为上一轮残留时间轴，从 0 重起 |
| 音频不可用 | `audioRef < 0` | **不用系统时间顶替**，返回无效 PTS，由调用方决定丢帧或降级 |

单调递增是硬约束：`clamp` / `forceSync` / 采集时钟回退都走 `ClampMonotonic`
（最小步长 = 帧间隔/10，下限 1ms），编码器侧再兜一次 `m_lastPtsUs`，
防止封装报 `non monotonically increasing dts`。

### 5.2 兼容场景矩阵

| 分类 | 异常场景 | 处理方式 | 结果 |
| - | - | - | - |
| **时钟源** | 无音频（`--no-audio` / `enabled=false`） | 走 `VideoOnlyTimeline` 采集时间轴，首帧归 0，回退时至少 +1ms | 视频时间轴连续 |
| | 音频已开但首拍未到（设备打开慢） | **视频等音频**，不发帧；超过 `audioTimeoutMs`（3s）才降级为纯视频时间轴 | 不会出现"开头几秒没画面" |
| | 音频 PTS 回退 | `AudioClock::AdvanceTo` 单向 CAS，只前进不回退 | 主时钟单调 |
| | 音频 PTS 跳变（>5×帧间隔） | 只计 `jump` 告警，不改写时间轴 | 可观测、不放大故障 |
| | 系统时钟回退 / 跳变（>10×帧间隔） | `passUs` 退回用帧间隔 | 异常不进时间轴 |
| | 系统冻结 / 挂起 >2s | `AudioChannelClock::ReferencePtsUs` 限幅吸收，不一次性追赶 | 恢复后不突跳 |
| **音频侧** | 设备无数据（静音、被占用、拔麦） | `WinAudioCapture::SilenceLoop` 持续出静音 | 时间轴不断 |
| | 某一路中途掉线 | `AudioMixer::Mix` 按静音补齐，输出时间轴按输出采样率继续推进 | 其余路不受影响 |
| | 多路采样率 / 声道不一致 | swresample 归一到输出格式 | 正常混流 |
| | 采样率 / 声道为 0 | 回落到输出格式，不产生除零 | 不崩溃 |
| | 单路 ↔ 多路切换 | 输出时间轴连续，`AddChannel` 动态建重采样上下文 | 无需重启 |
| **视频侧** | 帧率 0 / 帧间隔 ≤ 0 | 兜底 33333us（30fps） | 不除零 |
| | 帧率突变（动态 fps、分辨率切换） | 帧间隔固定 → avDiff 累积 → 先 RangeAdapt 渐进，过大则 ForceAlign | 自动收敛 |
| | 背压丢帧（编码器 Pending ≥ 8） | PTS 不推进，avDiff 变大后由同步档位纠正 | 追实时性，不累积延迟 |
| | forceSync / clamp 需要回退 | `ClampMonotonic` + 编码器 `m_lastPtsUs` 双重保护 | 单调，不写坏文件 |
| | 视频 muted | 直接丢弃，不推进 PTS；恢复时靠 ForceAlign 跳齐 | 不产生空档 |
| **封装侧** | 某一路领先超过 `interleaveMs`（350ms） | 暂停写该路，等另一路追上 | 交织均衡 |
| | 某一路彻底不再出包 | 该路超时（3×阈值）未到包则把基准前推（BreakV / BreakA 自愈） | 不会永久卡住 |
| | 视频或音频完全无包 | 交织基准为 `kNoPts`，跳过保护直接写出 | 单流文件仍可播 |
| **生命周期** | Stop → Start 重开（同一实例） | `VideoEncoder::Close` 复位 `m_lastPtsUs`；`m_audioClock` 与 `m_mixer` 同步复位 | 第二个文件时间轴从 0 起 |
| | 首帧命中残留时间轴 | 超过 5s 则从 0 重起，计 `staleReset` | 文件开头不缺帧 |

### 5.3 统计与判读

停止时打印一行，用来判断链路是否健康：

```
TGCapturer: 音画同步 avDiff=-13ms clamp=0 forceSync=0 adapt=0 stall=0 skip=0 jump=0 staleReset=0 syncDrop=0
```

| 字段 | 含义 | 健康区间 |
| - | - | - |
| `avDiff` | 音频 − 视频（>0 视频落后） | 绝对值 < 100ms |
| `clamp` | 被 clamp 到 audio+maxLead 的次数 | 偶发可接受，持续增长说明音频长期停滞 |
| `forceSync` | ForceAlign 直接跳齐的次数 | 少量正常，频繁说明音频时钟不稳 |
| `adapt` | RangeAdapt 渐进追赶 / 减速的次数 | 少量正常；持续增长说明帧率与音频长期不匹配 |
| `stall` | 判定"音频停滞"的次数 | 纯静音场景会增长，属预期 |
| `skip` | 建议丢弃的补帧 | 同上 |
| `jump` | 音频 PTS 跳变 / 回退告警 | 0 最佳 |
| `staleReset` | 首帧命中残留时间轴的次数 | 0 最佳 |
| `syncDrop` | 因音频停滞丢掉的视频帧 | 0 最佳 |

### 5.4 设计约束：同步只有一个决策点

**视频 PTS 只由 `VideoPtsGenerator` 决定，其它环节不得再参与"追不追、丢不丢"的决策。**

编码器侧的 `AVSync::Judge` 只保留为最后一道兜底，且阈值由
`forceSyncFactor × 帧间隔 × 1.5` 推导（`videoMaxLagUs = 0` 自动），
保证它落在 RangeAdapt 正常工作区间**之外**。

原因：丢帧发生在 `Next()` 之后，`m_currentPts` 早已推进，
所以编码器丢帧**不改变时间轴**，只减少帧数。若阈值落在追赶区间内，
就会出现"同步模块在渐进追赶、编码器同时砍帧率"的互相抵消 —— 白白丢帧而同步毫无改善。

同理，音频超时降级为纯视频时间轴时必须重置 `m_sync`，
否则编码器拿失效的音频时钟判"落后"，会把降级后所有视频帧丢弃。

---

## 6. 多源混流与时钟校正

模块：**`src/core/AudioMixer.{h,cpp}`**，同样独立于采集/编码/封装。
麦克风、扬声器、外部注入的音频都进混流器（**单路也进**，单路同样享受时钟校正）。

**混流（AudioMixer）**

| 能力 | 说明 |
| - | - |
| N 路混合 | 按采样点加和（32 位累加后饱和到 S16） |
| 缺数据 | 该路按静音补齐，不拖停整体时间轴 |
| 格式归一 | 采样率/声道不一致时用 swresample 归一到输出格式 |
| 时间轴 | 输出 PTS 由**输出采样率固定推进** |
| 线程安全 | `Push`（采集线程）与 `Mix`（混流线程）之间加锁 |

**时钟校正（AudioChannelClock，每路独立）**

| 偏差 | 处理 | 计数 |
| - | - | - |
| 系统时钟跳变/冻结（>2s） | 限幅吸收，不一次性追赶 | `jumpCount` |
| \|drift\| > 5×帧时长 | ForceAlign 直接对齐 | `forceCount` |
| \|drift\| > 3×帧时长 | BURST 步进收敛，每次最多 1 帧时长 | `burstCount` |
| 其余 | PI 微调（比例 + 积分），每帧最多修正 1/4 帧时长 | `piCount` |
| 任何情况 | 不允许回退、不允许一帧前进超过 3 帧时长 | — |

---

## 7. 性能模式（CPU / GPU 全链路）

`VideoEncoderParams.perfMode`（`PerformanceMode::Cpu` / `Gpu`）。

| 维度 | `Cpu`（默认） | `Gpu` |
| - | - | - |
| 采集 | DXGI → GPU→CPU 回读 → 内存位图 → NV12 | DXGI 纹理**留在显存**，不回读、不建内存位图 |
| 像素搬运 | CPU（libyuv 转 NV12、sws 缩放） | 显存内 `CopySubresourceRegion`，CPU 不参与 |
| 编码 | 软编 / 硬编均可 | 硬编直连（NVENC / AMF / MF） |
| 内存 | 4K 屏约 33MB 位图 + NV12 帧 | 仅显存纹理池 |
| 缩放 | CPU sws，可任意降分辨率 | **显存内不做缩放**，按采集分辨率编码 |

| 平台 | GPU 模式落地 |
| - | - |
| Windows | 采集与编码器**共用同一个 `ID3D11Device`**（`EnsureD3D11Device()` → `SetExternalDevice()`），纹理只在显存里拷；`hw_frames_ctx` 按 `AV_PIX_FMT_D3D11` + `sw_format=BGRA` 建池 |
| iOS | `CVPixelBuffer` 零拷贝直送 VideoToolbox |
| Android | MediaProjection 直投编码器输入 Surface |

**自动回落**（任一不满足就退回 CPU 并打日志）：无硬编、采集源不支持 GPU、DXGI 复制通道建不起来、
硬件帧池初始化失败、采集实际走了 GDI。

```cpp
vp.perfMode = TGCapture::PerformanceMode::Gpu;
vp.vCodec = "h264_nvenc";
```

---

## 8. 软编 / 硬编

* **软编兜底**：`libopenh264`（H.264）+ `libfdk_aac`（AAC）—— 任何环境可用。
* **硬编直连（零拷贝）**：按 `vCodec` 字符串选择编码器，帧携带硬件句柄时直接构造硬件 `AVFrame`：

| 平台 | 每帧流转 | 编码器名 | 像素格式 |
| - | - | - | - |
| Android | 采集 Surface → 编码器输入 Surface | `h264_mediacodec` | `AV_PIX_FMT_MEDIACODEC` |
| iOS | `CVPixelBuffer` → VideoToolbox | `h264_videotoolbox` | `AV_PIX_FMT_VIDEOTOOLBOX` |
| Windows | D3D11 纹理 → NVENC / MF / AMF | `h264_nvenc` / `h264_mf` / `h264_amf` | `AV_PIX_FMT_D3D11` |

硬编不可用时自动回退系统内存路径，功能不受影响，只是不再零拷贝。

---

## 9. 构建

命令行与 IDE **共用同一套 CMake 工程**，产物完全一致。

### 9.1 命令行

```powershell
python scripts/build.py --platform windows --abi x64
python scripts/build.py --platform linux --3rd ~/TGCapture3rd-linux   # 需先编出 linux 版依赖
python scripts/build.py --platform android --abi arm64-v8a --ndk $ANDROID_NDK_HOME
python scripts/build.py --platform ios
# 开关：--clean / --build-type Debug|Release / --no-demo / --3rd <依赖目录> / --ide
```

### 9.2 IDE（VS2022 / Android Studio / Xcode）

一条命令生成工程并打开（**只生成，不编译**）：

```powershell
python scripts/build.py --platform windows --ide
python scripts/build.py --platform android --ide
python scripts/build.py --platform ios    --ide
```

| 平台 | IDE | 工程 | 说明 |
| - | - | - | - |
| Windows | VS2022 | `build/windows-x64/TGCapture.sln` | 只保留 `Debug`/`Release`；`tg_capture_demo` 为启动工程；调试环境变量已带 ffmpeg DLL 目录，**F5 直接跑** |
| Android | Android Studio | `ide/android` | gradle 宿主工程，`externalNativeBuild` 驱动根目录 `CMakeLists.txt` |
| iOS | Xcode | `build/ios-fat/TGCapture.xcodeproj` | `--ide` 自动切 Xcode 生成器 |

* Android 换 ABI 要**同时**改 `abiFilters` 与 `-DTG_ABI`；NDK 版本改 `ndkVersion`（默认 r27c）。

### 9.3 依赖目录怎么指定

| 优先级 | 方式 | 用法 |
| - | - | - |
| 1 | 命令行 | `--3rd <目录>`（等价 `-DTG_3RD_ROOT=<目录>`） |
| 2 | 环境变量 | `setx TG_3RD_ROOT D:\work\TGCapture3rd`（**CI / IDE 推荐**） |
| 3 | 自动探测 | 什么都不给 → 工程同级 `TGCapture3rd`（旧名 `3rd-rel-Party` 仍兼容） |

默认值只在 `CMakeLists.txt` 定义一处；configure 会打印 `3rd=<路径> (来源: …)`；
缓存里的失效路径会自动回退探测并告警。
Windows 产物是 `flat` 布局（无 abi 层），`cmake/Deps.cmake` 按 abi → lib/abi → flat 依次探测。

### 9.4 产物

| 平台 | 产物 |
| - | - |
| Linux | `build/linux-x86_64/libtgcapture.a`、`tg_capture_demo`（外灌帧 demo，不依赖采集设备） |
| Windows | `build/windows-x64/Release/tgcapture.lib`、`tg_capture_demo.exe`（+ 同目录 ffmpeg DLL） |
| Android | `libtgcapture.a`（gradle 输出到 `ide/android/app/build/…`） |
| iOS | `build/ios-fat/libtgcapture.a` |

---

## 10. 测试与验证

### 10.1 demo 参数

```powershell
cd build\windows-x64\Release
.\tg_capture_demo.exe --help
```

| 参数 | 说明 |
| - | - |
| `--out <url>` | `out.flv` / `out.ts` / `rtmp://…` / `srt://…`（默认 out.flv；`.ts` 自动用 mpegts） |
| `--vcodec` / `--acodec` | 编码器名 |
| `--w` `--h` `--fps` `--bitrate` | 推流参数（CPU 模式生效） |
| `--seconds <n>` | 运行时长，默认 10 |
| `--perf <cpu\|gpu>` / `--gpu` | 性能模式 |
| `--mode <dxgi\|gdi>` | 屏幕采集方式，默认 dxgi |
| `--monitor <n>` | 显示器序号，默认 0 |
| `--window <hwnd>` | 采集指定窗口（**16 进制**句柄） |
| `--no-border` | 窗口只取客户区 |
| `--speaker` | 麦克风 + 扬声器同时采集（自动混音） |
| `--no-audio` | 纯视频 |
| `--fake-video` | 人造帧（不依赖采集，排查编码/封装链路） |
| `--list-audio` | 列出系统麦克风/扬声器端点后退出 |
| `--restart <n>` | 首轮结束后再 `Stop→Prepare→Start` 跑 n 轮（重开回归，每轮时长同 `--seconds`） |
| `--concur <n>` | 并发压测：`Prepare` 与 `Stop` 两条线程交错冲击 n 秒，验证生命周期互斥 |
| `--rotate <deg>` | 画面旋转 0 / 90 / 180 / 270 |
| `--flip <h\|v\|both>` | 画面翻转：h=水平镜像，v=垂直翻转 |
| `--crop <w,h>` | 从源帧左上角裁剪（如 `--crop 960,540`） |
| `--log-level <n>` | 0=Debug 1=Info 2=Warn 3=Error（默认 0，见 §4.6） |
| `--log-dir <dir>` | 日志落盘目录（默认不落盘） |
| `--no-print-log` | 关闭 stdout 日志输出（配合 `--log-dir` 只看文件） |
| `--log-bench <n>` | 只跑日志吞吐基准（打 n 条）后退出，见 §4.6.1 |

取窗口句柄（注意 demo 按 16 进制解析）：

```powershell
(Get-Process | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1).MainWindowHandle
'{0:X}' -f 853962     # -> D07CA
```

### 10.2 回归用例

| # | 命令 | 预期 |
| - | - | - |
| 1 | `--seconds 3` | 编码 ≈90 帧；`ffprobe` 见 `h264` + `aac` |
| 2 | `--seconds 3 --speaker` | 日志 `启用音频混流（2 路…）`；双流正常 |
| 3 | `--seconds 3 --no-audio` | 只有 `h264` 流；同步统计 `avDiff≈0` |
| 4 | `--seconds 3 --gpu` | 日志 `GPU 链路就绪`；编码器 `h264_nvenc`；`ffprobe` 分辨率 = 采集分辨率 |
| 5 | `--mode gdi` | 日志 `屏幕 GDI …`；正常出帧 |
| 6 | `--window <hwnd>` / 再加 `--no-border` | 整窗与客户区尺寸不同（后者去掉边框） |
| 7 | `--list-audio` | 列出麦克风/扬声器端点（默认项有标记） |
| 8 | `--fake-video --no-audio` | 不依赖设备也能跑通编码/封装 |

音画同步专项（对应 §5.2 矩阵）：

| # | 场景 | 操作 | 预期 |
| - | - | - | - |
| 9 | 音频首拍延迟 | 音频源 `Open` 前 sleep 2~3s | 日志出现 `等待音频主时钟首帧`；视频首帧 PTS 不回退，无开头空档 |
| 10 | 音频永久失效 | 确认无音频设备后跑带音频用例 | 超时后日志 `音频超时未就绪，降级为纯视频时间轴`；仍有视频流 |
| 11 | Stop → Start 重开 | `--restart 2` | 每轮 `encoded` 相当、`dropped=0`，首帧回调每轮都触发 |
| 12 | 高帧率 | `--seconds 10` 且 `fps=60` | `avDiff` 稳定；`stall` 不随帧率线性增长 |
| 13 | 系统冻结 | 录制中挂起进程 10s | `jump` +1，恢复后视频 PTS 不突跳 |
| 14 | 并发启停 | `--concur 5` | `prepare_ok` / `stop` 数量接近、进程 `exit=0`、不挂死 |
| 15 | 画面旋转 | `--rotate 180 --fake-video` | 画面左上/右下亮度与 base **完全互换** |
| 16 | 旋转自洽 | `--rotate 90` vs `--rotate 270` | 两者左上/右下互为交换 |
| 17 | 翻转自洽 | `--flip both` vs `--rotate 180` | 两者结果**完全一致**（水平+垂直 = 180°） |
| 18 | 裁剪 | `--crop 640,360 --fake-video` | 画面只剩源左上区域，整体变暗 |
| 19 | 日志分级 | `--log-level 0/1/2/3` | 依次 `D27/I10/W2`、`D0/I10/W2`、`D0/I0/W2`、`D0/I0/W0` |
| 20 | 日志落盘 | `--log-dir logs --no-print-log` | stdout 只剩 demo 自己的输出；`logs/tgcapture_*.log` 有完整内容 |

> 15~18 用 `--fake-video` 的人造图案（左暗右亮 + 上暗下亮）验证，
> 判读方式：`ffprobe -f lavfi -i "movie=v.flv,crop=iw/2:ih/2:0:0,signalstats"` 取左上/右下平均亮度。
> 真实屏幕在当前会话可能是黑屏，不适合做内容校验。

每次跑完看两处：

```
[demo] 最终统计: {"codec":"...","encoded":N,"dropped":0,"pending":0}
[TGCapturer] 音画同步 avDiff=…ms clamp=0 forceSync=0 adapt=0 stall=0 skip=0 jump=0 staleReset=0 syncDrop=0
```

### 10.3 产物校验

```powershell
$env:PATH = "D:\bertonwang\bing\TGCapture3rd\ffmpeg\windows\bin;" + $env:PATH
ffprobe -v error -show_entries stream=codec_name,width,height,channels,sample_rate -of csv=p=0 out.flv
ffprobe -v error -show_entries format=duration,size -of default=nw=1 out.flv
ffplay out.flv                      # 实际播放确认音画同步
```

推流场景可先起本地服务验证：`ffmpeg -f flv -listen 1 -i rtmp://0.0.0.0/live/stream -c copy out.flv`，
然后 `--out rtmp://127.0.0.1/live/stream`。

### 10.4 Windows 实测结果（当前版本）

| 用例 | 结果 |
| - | - |
| 屏幕 DXGI（2160x3840） | 3 秒 97 帧 ≈30fps 满帧（桌面静止时复用上一帧不断流） |
| 屏幕 GDI 兜底 | 正常出帧 |
| 窗口整窗 / 客户区 | `2012x2032` / `1990x2022`（`--no-border` 正确去边框） |
| 麦克风 + 扬声器 | 两路 WASAPI 就绪，内核启用混流，输出 `h264 + aac` |
| 设备枚举 | `--list-audio` 正确列出端点 |
| 音画同步 | `avDiff` 稳定在 ±25ms 内，`adapt` 0~5，无 clamp / 跳变 / 丢帧 |
| Stop → Start 重开 | `--restart 2`：每轮 `encoded` 96~97、`dropped=0`，首帧回调每轮都触发 |
| 并发启停 | `--concur 5`：5 秒 28~36 次完整启停，`exit=0`，无死锁 / 崩溃 / 挂起 |
| GPU 全链路 | `h264_nvenc` + D3D11 帧池（fmt=`AV_PIX_FMT_D3D11`, sw=`BGRA`），3 秒 98 帧，文件可解析 |
| 稳定性 | 同配置连跑 5 次均 `exit=0`、产物有效 |

### 10.5 Linux 外灌帧 demo（真实素材 1 视频 + 2 音频）

Linux 没有原生采集，但是**整条编码 / 封装 / 混流 / 音画同步链路都可以用外灌帧跑通**。
`demo/linux/main.cpp` 用系统 `ffmpeg` CLI 把素材解成裸 NV12 / S16LE PCM 经管道喂入，
两路音频走不同的 `AudioCapSourceType` 各占独立混流通道，由内核做 sample-wise mix。

#### 参数速查

```bash
./tg_capture_demo --help
```

| 参数 | 默认 | 说明 |
| - | - | - |
| `--video <path>` | dance-girl.n.mp4 | 视频源（任意 ffmpeg 可解的容器） |
| `--audio1 <path>` | 同 `--video` | 第 1 路音频源 → `TypeOfGameMicphone4Slave`（混流通道 0） |
| `--audio2 <path>` | iqiyi.ts | 第 2 路音频源 → `TypeOfGameAudio4Slave`（混流通道 1） |
| `--audio2-volume <f>` | 0.1 | 第 2 路相对音量（线性缩放 S16 样本） |
| `--video-loops <n>` / `--audio1-loops` / `--audio2-loops` | 1 / 1 / 0 | 各路独立循环次数（`0` = 无限） |
| `--audio1-speed <f>` / `--audio2-speed <f>` | 1.0 / 1.0 | 推送速度倍率（`>1`=过快，`<1`=过慢，详见下文） |
| `--out <url>` | out.flv | 输出（`.flv` / `.ts` / `rtmp://…` / `srt://…`） |
| `--seconds <n>` | 10 | 运行秒数；`0` = 直到视频 EOF |
| `--w --h --fps --bitrate` | 1280 720 30 3000000 | 编码参数 |
| `--sample-rate --channels` | 44100 2 | 音频采样率/声道数 |
| `--no-audio` / `--audio2-no-loop` | — | 开关 |
| `--ffmpeg <path>` | ffmpeg (PATH) | 使用的 ffmpeg CLI |
| `--log-level <n>` | 1 | 0=Debug 1=Info 2=Warn 3=Error |

`--audio*-speed` 用于**压测音源节奏异常**（内核应持续吸收 drift）：
- PTS 固定按采样数累加（`1024/44100 ≈ 23.22ms`/块），单调推进
- wallclock 触发间隔 = `23.22ms / speed`，`speed>1` 加密触发（源产太快），`speed<1` 疏松触发（源产太慢）
- Mixer `AudioChannelClock` 看到 `driftUs = refPts(wallclock) - currentPts(samples)`，按偏差大小分档 PI / BURST / ForceAlign 校正
- VideoPtsGenerator 侧对视频 PTS 做 `adapt`（RangeAdapt 渐进追赶）/ `forceSync`（强制跳齐）

#### 典型用例

```bash
cd build/linux-x86_64

# ① 最小烟测：6s 常态双路音频 + 视频
./tg_capture_demo --seconds 6 --out /tmp/smoke.flv

# ② 推流（本地 RTMP）
# 另开终端起接收端：ffmpeg -f flv -listen 1 -i rtmp://0.0.0.0/live/stream -c copy /tmp/pulled.flv
./tg_capture_demo --seconds 30 --out rtmp://127.0.0.1/live/stream

# ③ 换素材：命令行全可配
./tg_capture_demo \
  --video    /path/to/my_video.mp4 \
  --audio1   /path/to/vocal.wav \
  --audio2   /path/to/bgm.mp3  --audio2-volume 0.2 \
  --seconds  60 --out /tmp/mix.flv

# ④ 长时压测（素材循环 10 遍 + 轻度音源 drift，模拟真实硬件时钟不均）
#    预计 wallclock ≈ 10 × 素材时长；视频 EOF 后自动退出
./tg_capture_demo \
  --video /data/home/bertonwang/mov/dance-girl.n.mp4 \
  --audio1 /data/home/bertonwang/mov/dance-girl.n.mp4 \
  --audio2 /data/home/bertonwang/mov/iqiyi.ts         --audio2-volume 0.1 \
  --video-loops 10 --audio1-loops 10 --audio2-loops 400 \
  --audio1-speed 0.97  --audio2-speed 1.03 \
  --w 854 --h 480 --fps 30 --bitrate 1500000 \
  --seconds 0 --out /tmp/stress.flv

# ⑤ 极端 drift（±10%）短跑，看日志里 forceSync/adapt 快速累加
./tg_capture_demo \
  --audio1-speed 0.90 --audio2-speed 1.10 \
  --seconds 90 --out /tmp/drift.flv

# ⑥ 直播推流 + Ctrl+C 优雅停：SIGINT/SIGTERM 会被捕获，正常刷 flv/ts trailer
./tg_capture_demo --seconds 0 --out /tmp/live.flv   # Ctrl+C 结束
```

#### 日志判读

**运行期心跳（10s / 条，MixLoop 发出）** — 长压测时在时间轴上就能看到 drift 分布，不用等 Stop 聚合：

```
[I] TGCapturer: 心跳 avDiff=118ms force=193 adapt=898 clamp=0 stall=0 skip=0 jump=0 staleReset=0
  | ch0[drift=0ms  pi=1636 burst=1 force=0 jump=0 under=1671 comp=0ms drop=0ms buf=0]
  | ch1[drift=0ms  pi=1841 burst=2 force=0 jump=0 under=1465 comp=0ms drop=0ms buf=0]
```

- 外层（视频 PTS 生成器）：`avDiff` < 1 帧时长 OK；`forceSync/adapt` 持续累加表明内核在吸收 drift
- `ch<N>`（每路音频独立）：
  - `drift` 当前瞬时偏差（参考时钟 − 本路时钟）；稳态下近 0，`pi` 持续增长说明 PI 在吃掉偏差（校正真在工作）
  - `burst/force` 一次性启动尖峰正常；稳态连增表示 drift 太大 PI 吃不下
  - `under` 持续增长说明本路补货率 < Mix 消费率（部分段靠静音补齐）
  - `comp/drop` 样本级补/丢累计（限速 5ms/s）
  - `buf` 该路 pcm 积压；长期非 0 表示积压可能死锁
- `clamp / stall / skip / jump / staleReset / syncDrop` 应恒为 0 — 任何非零都要查

**结束聚合（Stop 时一次性输出）**：

```
[I] TGCapturer: 音画同步 avDiff=85ms clamp=0 forceSync=8480 adapt=39482 stall=0 skip=0 jump=0 staleReset=0 syncDrop=0
[I] VideoEncoder: 线程退出 codec=libopenh264 encoded=... dropped=0 latency_us=...
[I] AudioEncoder: 线程退出 codec=libfdk_aac encoded=... latency_us=...
[demo] 输出=... 视频帧=... 音频1=... 音频2=... 拒绝=...
```

**demo 推送心跳（30s / 条）** — 验证 demo 自身仍在供帧 + 校核 speed 效果：

```
[demo] t=60.001s video+901(=1801) a1+1227(=2455) a2+1383(=2766) rej=0
```

`a2+ / a1+` 比值应接近 `audio2-speed / audio1-speed`（本例 `1383/1227≈1.126 ≈ 1.07/0.95`）。

**运行期告警**：每 3 秒节流一条 `混流取不到数据（当前通道数 N，帧长 1024）` 是**正常运行节律**（Mix 消费率略高于补货率时，MixLoop 轻度 sleep 等待），不是错误。

#### 已知时间轴行为

两路音频 `speed` 不同（相位错开触发 Mix）时，`AudioMixer::m_outputPtsUs` 以 Mix 调用频率累加，导致**输出 flv 的 Duration 元数据虚增**（本例 wallclock 29.8min → flv Duration 56:55）。`speed=1.0/1.0` 稳态下不触发（6s/10s 常态烟测 Duration ≈ wallclock）。推流链路里下游用的是实际 PTS 流而非 Duration 元数据，不影响推流本身；只有本地回放时间轴会偏。

---

## 11. 已知限制 / 后续项

* **GPU 模式不做缩放**：显存内只做同尺寸拷贝，因此 GPU 模式**编码分辨率 = 采集分辨率**
  （4K 屏即 2160x3840）；需要降分辨率推流用 CPU 模式。后续可在显存内加 D3D11 VideoProcessor / `scale_d3d11`。
* **Linux：有内核、无原生采集** —— 屏幕需 X11 / PipeWire、音频需 PulseAudio / ALSA，
  当前均未实现，因此 `IsSupported()` 一律返回 false、`Create*Capture` / `Enable*Capture`
  返回空 / 负值。编码 → 封装 → 音画同步 → 混流与外灌帧（`PushExternal*Frame`）照常可用，
  `demo/linux` 用系统 `ffmpeg` CLI 把真实素材解成裸 NV12 / S16LE PCM，
  按"1 视频 + 2 独立音频"喂进 `PushExternalVideoFrame` / `PushExternalAudioFrame`，
  由内核混流器把两路音频合并后再和视频一起走编码封装。
  要真正开播，需补 Linux 采集源，或用 `PushExternalVideoFrame` 从上层喂帧。
* **外部注入音频按 `AudioCapSourceType` 分路**：同一进程内多次 `PushExternalAudioFrame`
  传不同的 `type`（如 `TypeOfGameAudio4Slave` 与 `TypeOfGameMicphone4Slave`）会各自占用
  独立的混流通道并被真正做 sample-wise 相加；传相同 `type` 则串成同一时间轴（覆盖/交替）。
* **Android / iOS 尚未在各自平台编译验证**（本机只有 Windows 工具链）：
  `src/platform/android/*`、`src/platform/ios/*` 与 `TGCaptureSystemCapture.java`
  需在有 SDK/NDK/Xcode 的环境跑一次。
* **日志**：`Init` 的 `logDir` / `printLog` 未实现；移动端未接 `__android_log_print` / `os_log` sink（见 4.6）。
* 暂停静帧、水印、本地文件采集为接口占位（返回 0 或告警）。
* `GetStatsInfo` 当前返回基础 JSON（码率 / 队列 / 编码器统计）。
* **画面变换**：
  * **GPU 模式开销大** —— 纯显存拷贝带不了旋转/翻转/缩放，只能回读显存用 libyuv 处理，
    实测 2160x3840 + 旋转约 1 帧/秒。要走 GPU 全链路性能就不要开变换（见 4.8）。
    后续可用 `ID3D11VideoProcessor` 或渲染 quad 在显存内完成，当前未实现。
  * **DPI 缩放未处理** —— 高 DPI 屏（150%/200%）下 `GetClientRect` 是逻辑像素、
    DXGI 是物理像素，混用会只采到一部分或出黑边；Android Java 侧 `dpi` 还是硬编码 1。
  * **旋转未写进码流** —— `Frame::rotation` 有字段但没写进 SPS VUI / displaymatrix，
    播放器不会自动纠正方向，旋转是把像素真的转过去了。
  * **Android / iOS 的 `FrameWidth()` 是配置值而非真实尺寸**，
    与实际帧不一致时 GPU 路径会走回退（CPU 路径靠 sws 自适应）。
  * 摄像头源（`TypeOfSelfCamera`）枚举存在但**无实现**；`CreateLocalFileCapture` 是空壳。
* **并发**：`Prepare`/`Start`/`Stop` 已串行化（见 4.7），但 `Prepare()` 失败路径的回滚
  目前只覆盖 Muxer；新增模块时记得在自己的失败分支里收尾。

---

## 12. 维护者注意事项（易踩的坑）

都是踩过一次才发现的，改这几处时务必留意。

| 坑 | 现象 | 约束 |
| - | - | - |
| `AvErr()` 一条日志只能用一次 | 第二次会覆盖第一次的输出 | 它返回 `thread_local` 缓冲；需要两个错误码就分两条日志 |
| 宏里用 `__LINE__` 拼变量名 | MSVC 传统预处理器在宏的多行展开中取值不一致，报"未声明的标识符" | `TG_LOG*_THROTTLED` 改用 lambda 携带 `static` 局部量，新增限频宏照此写 |
| 音频采集不填 `captureUs` | 通道时钟的参考时钟退化成常数，每帧触发 ForceAlign，drift 统计全废 | 三端音频采集都要填（`AudioMixer::Push` 有兜底，但采集端应主动填） |
| `BoundedQueue::Close()` | 是**永久关闭**语义，不 `Reopen()` 的话后续 `Push` 全部失败 | 新增队列时在对应 `Open()` 里 `Reopen()`（见 4.7） |
| 编码器里再加同步逻辑 | 与 `VideoPtsGenerator` 互相抵消 | 同步只有一个决策点，见 5.4 |
| `Worker::Loop()` 抛异常 | 直接 `terminate`，不留日志 | 子类 `Loop` 目前没有 try/catch 兜底，新增循环体自己保证不抛 |
| 改 `VideoEncoder::Close()` | 漏复位 `m_lastPtsUs` 会让重开的全视频帧被判成 PTS 回退丢弃 | 新增"随流状态"时一并考虑复位 |
