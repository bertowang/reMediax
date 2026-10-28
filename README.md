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
| 时钟校正 | 每路独立：5s 窗口测"调用方时间戳推进速率 vs 墙钟"的原始漂移 → swr 拉伸/压缩样本补偿；时间戳不可信（回退 / 速率偏离 >5%）则整路改用墙钟 |
| 音画同步 | 音频为主时钟；视频 PTS 由音频反推；EMA 滤波 + 比例追赶 + 停滞 clamp；突发到达丢帧而非压缩 PTS；封装交织自愈 |
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
├── demo/                       测试 demo（第 10 节）
│   ├── windows/ linux/         命令行 demo tg_capture_demo（原生采集 / 外灌帧）
│   ├── android/                UI demo：Kotlin 开播测试 App（Gradle :demo）
│   └── ios/                    UI demo：UIKit 开播测试 App（CMake → Xcode）
├── bindings/android/           Android SDK（Gradle :tgcapture → AAR）：Java API + JNI 胶水
├── tests/                      单元测试 tg_tests（零依赖迷你框架，第 10.4 节）
├── cmake/Deps.cmake            TGCapture3rd 依赖定位
├── ide/android/                Android Studio 宿主工程（只做模块聚合：:tgcapture + :demo）
├── CMakeLists.txt
├── CHANGELOG.md
└── scripts/
    ├── build.py                构建脚本（windows/android/ios/linux，含 --ide）
    ├── accept_av.sh            音画同步验收（Linux demo，6 个场景自动判定）
    └── ci.sh                   CI 入口（构建 → 单测 → 可选验收）
```

数据流（节点经 `Bus::Link` 串联，节点间有界队列 + 背压）：

```
VideoSource ─┐
             ├─> VideoEncoder ─┐
AudioSource ─┴─> AudioMixer ─> AudioEncoder ─┴─> Muxer ─> rtmp://  srt://  file
                     ▲
              音频主时钟 ──> VideoPtsGenerator ──> 视频 PTS
```

### 3.1 Bus：数据 / 命令 / 事件三个平面

`core/Bus.h`。编码器与封装器都是 `BusNode`，由 `TGCapturer` 在构造时 `Register` + `Link`：
`video_encoder → muxer`、`audio_encoder → muxer`。**节点之间不再互相持有指针**，新增节点
（录制旁路、转码、统计）只需 `Register` + `Link`，不用改上游。

| 平面 | 接口 | 语义 |
| - | - | - |
| 数据 | `BusNode::Submit(FramePtr)` / `Forward(FramePtr)` | 入队即返回；`Forward` 沿 `Link` 下游送；下游的积压通过 `DownstreamPending()` 反馈给背压 |
| 命令 | `Bus::Send(name, cmd)` / `Broadcast(cmd)` | `Flush`（清队列）、`RequestKeyFrame`（视频编码器下一帧出 IDR）、`Abort`（Muxer 中断阻塞的 I/O）；节点在 `OnCommand` 里处理，不认识的返回 false |
| 事件 | `BusNode::ReportError` / `ReportNotify` → `EventHub` | 异步分发线程 + 有界队列；`Publish` 永不阻塞，`PublishSync` 在调用线程执行 |

事件平面的保证：

- **Error 不丢**：队列满时先丢 Notify，Error 保留。
- **订阅者抛异常不会杀死分发线程**。
- `Drain` / `Shutdown` 可在订阅者内部调用（不会自己 join 自己）。
- 宿主回调（Prepared / Error / Started / Stopped / Notify）**统一经事件平面触发，且不持有生命周期锁**
  —— 回调里可以直接 `Stop()` / `Prepare()` / 销毁对象，不会死锁（见 4.7）。
- 心跳日志里的 `bus[pend=… evDrop=…]` 是总线积压与事件丢弃数；`evDrop` 持续增长说明宿主回调太慢。

> `m_bus` 在 `TGCapturer` 里声明在各节点之前，保证最后析构：节点析构时仍可能上报事件。

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

> 回调都不持有内部锁。线程：`OnPrepared` / `OnStart` / `OnStop`（以及 `Prepare` 失败时的 `Error`）在调用
> `Prepare` / `Start` / `Stop` 的线程同步触发；运行期的 `Error` / `Notify` 在事件分发线程触发（见 3.1、4.7）。
> `Error` 的 `what` 取自 `TGCaptureError`：
>
> | 码 | 含义 |
> | - | - |
> | `TGErr_InvalidState` (-1) | 状态不允许：重复 `Prepare`、未 `Prepare` 就 `Start`、未 `Start` 就 `PushExternal*Frame` |
> | `TGErr_NotConfigured` (-2) | 编码器 / 封装器参数没设全 |
> | `TGErr_OpenVideoEncoder` (-3) | 视频编码器打开失败 |
> | `TGErr_OpenAudioEncoder` (-4) | 音频编码器打开失败（含输入采样格式不是交错 S16、采样格式转换初始化失败）；`extra` = AVERROR 或子模块码 |
> | `TGErr_OpenMuxer` (-5) | 封装器 / 推流地址打开失败 |
> | `TGErr_OpenVideoSource` (-6) / `TGErr_StartFailed` (-7) | 原生视频采集源打不开 / 内部线程启动失败 |
> | `TGErr_InvalidArgument` (-20) ~ `TGErr_Internal` (-23) | 入参非法 / 不支持 / 未实现 / 内部资源创建失败 |
> | `TGErr_MuxWriteFailed` (-101) | **运行期**写包持续失败超过 `writeFailReportMs`（断网 / 磁盘满 / 对端不收）；`extra` = 最后一次的 AVERROR（持续时长写在 message 里），每轮失败只上报一次 |
> | `TGErr_VideoEncodeFailed` (-102) / `TGErr_AudioEncodeFailed` (-103) | **运行期**视频 / 音频编码持续失败超过 1s；`extra` = AVERROR |
> | `TGErr_VideoCaptureStalled` (-104) / `TGErr_AudioCaptureStalled` (-105) | 采集断流超过 `SetAVTimeout` 阈值；恢复时发 `*_Recovered` 通知 |
> | `TGErr_MuxTrailerFailed` (-106) | `Stop` 时写文件尾 / 关闭连接失败，产出文件可能缺索引 |
>
> 完整定义与 `extra` 含义以 `include/TGCapture/TGCapturerInterface.h` 的 `TGCaptureError` 注释为准。
>
> 运行期出错**不会自动重连**：FFmpeg 的 I/O 错误是粘性的，宿主应 `Stop()` 后重新 `Prepare()`。

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

Java 桥接在 `bindings/android/src/main/java/com/tencent/tgcapture/TGCaptureSystemCapture.java`（VirtualDisplay + ImageReader + AudioRecord/AudioPlaybackCapture）；
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
- 音频的 `ptsUs` 是**调用方与内核的时钟契约**，见 `ITGCapturer::PushExternalAudioFrame` 注释：
  - 单调时钟域、单位微秒、表示**采集时刻**，且必须**严格递增**；
  - 推进速率要跟随墙钟（真实设备时钟有 ppm 级偏差，由内核测量并补偿，不用调用方自己校）；
  - 不知道就传 `-1`，内核改用到达时刻；
  - 输出 PTS 由内核生成，与传入值不是同一个时间轴。
  - 违反"严格递增"或"速率偏离 >5%"时内核会告警并**整路改用墙钟**（心跳里 `tsBad` / `fallback`），
    此时该路的漂移补偿不再基于调用方时间戳。

### 4.5 参数总表

**`VideoEncoderParams`**

| 字段 | 默认 | 说明 |
| - | - | - |
| `width` / `height` / `fps` | 1280 / 720 / 30 | 推流分辨率与帧率 |
| `vCodec` | `libopenh264` | 也支持 `h264_nvenc` / `h264_amf` / `h264_mf` / `h264_mediacodec` / `h264_videotoolbox`；硬编本机打不开时自动回落 `libopenh264` 并发 `EsCap_Notify_Encoder_Fallback`（第 8 节） |
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
| `aCodec` | `libfdk_aac` | 也支持 `aac`（ffmpeg 内置）等任意 ffmpeg AAC 编码器名；指定的编码器不可用时自动回退内置 `aac`（打 Warn，发 `EsCap_Notify_Encoder_Fallback`） |
| `aBitrate` | 128000 | 音频码率 |
| `enabled` | `true` | `false` = 纯视频（不建音频编码器与音频流） |

**`MuxerParams`**

| 字段 | 默认 | 说明 |
| - | - | - |
| `muxer` | `flv` | `flv` / `mpegts` |
| `muxerOutput` | — | `rtmp://` / `rtmps://` / `srt://` / 文件路径 |
| `interleaveMs` | 350 | 音视频交织阈值，0 = 关闭 |
| `connectTimeoutMs` | 10000 | 打开输出（含 RTMP 握手）的超时 |
| `ioTimeoutMs` | 5000 | 单次读写超时（`rw_timeout` + 中断回调）；对端不收数据时 `Stop()` 最多等这么久 |
| `writeFailReportMs` | 3000 | 写包持续失败多久后上报 `TGErr_MuxWriteFailed`；0 = 默认值 |

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
- **宿主回调不在锁内触发**：`Prepare` / `Start` / `Stop` 先在锁内完成状态切换，解锁后再通过
  事件平面通知宿主，所以回调里调 `Start()`（demo 的写法）、`Stop()` 都是安全的。
  `recursive_mutex` 仅为兼容同线程内部重入而保留。
- `Stop()` 顺序：（Muxer 正处于写失败时先发 `Abort` 打断阻塞的 I/O）→ 逆序停各节点 → `EventHub::Drain`
  → 触发 `OnStop`。因此 `OnStop` 一定排在此前所有 Error / Notify 之后。
- 音频队列是 `DropOldest` 而不是 `Block`：封装器阻塞时若音频编码器堵在 `Submit` 里，
  `Stop()` 先 join 它就会互等。
- 帧回调（`OnVideoFrame` / `OnAudioFrameFrom`）与 `MixLoop` **不取锁**，
  只查 `m_state` 原子量 —— 否则停止流程 `join` 时会死锁。
- 采集/编码/封装之间的队列本身是线程安全的（`BoundedQueue` 内部加锁）。

**重开（Stop → Prepare → Start）**

支持同一实例反复启停，并且每轮都会完整复位：队列重开、编码器统计、音频主时钟、
混流器、首帧标志、封装交织基准。

> 维护者注意：`BoundedQueue::Close()` 是**永久关闭**语义（用来唤醒阻塞的消费者），
> 重开必须显式 `Reopen()`。各模块的 `Open()` 已经调用，新增队列时别漏。

验证：`--restart <n>`（重开回归）、`--concur <n>`（Prepare/Stop 并发压测），见 §10.2.1。

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
| `VideoPtsManager` · RangeNormal / RangeAdapt / RangeCatchUp | `VideoPtsGenerator` Normal / Adapt / ForceAlign / Pause 四档（见 5.1） | 已对齐（Adapt 改为比例控制 + 滞回） |
| `VideoPtsManager` · ForceAlign | 偏离 > 6×帧间隔时跳齐 | 已对齐（阈值更激进：6 帧 vs MediaX 5 帧量级） |
| `VideoPtsManager` · Pause | 音频停滞 → 视频封顶 `maxLead` | 已对齐 |
| BreakV / BreakA 交织自愈 | `Muxer` 交织 + stall 超时推基准 | 已对齐 |
| 暂停 / 恢复独立档位 | 无，靠 ForceAlign 跳齐兜底 | **差异**（可接受：muted 时不推进 PTS，恢复时跳齐） |

### 5.1 视频 PTS 生成规则

`avDiff = 音频 PTS −（视频上一帧 PTS + 帧间隔）`，**>0 表示视频落后**。
用"本帧按标称间隔推进后"的 PTS 计算：若用上一帧 PTS，偏差会系统性偏大一个帧间隔，
控制器会把视频推到超前约 1 帧处才平衡（表现为基线稳态偏差 -20~-40ms 且不收敛）。

音频时钟按 1024 样本离散推进（44.1kHz 下 23.2ms 一跳），单帧 `avDiff` 自带 ±12ms 量化抖动，
所以**先做 EMA（1/8）再判档**，否则 1 帧门限会被抖动反复触发。

| 档位 | 触发条件（30fps 示例） | 处理 |
| - | - | - |
| **RangeNormal** | \|EMA\| ≤ 1×帧间隔（≤33ms） | 按帧间隔递增 + 持续微调 `EMA / trimGainFrames`（120 帧 ≈ 4s 时间常数），把 1 帧内的残余偏差拉回 0 |
| **RangeAdapt** | \|EMA\| > 1×帧间隔，回到 1/4 帧间隔（8ms）内才退出（滞回） | 比例控制：每帧修正 `avDiff / adaptGainFrames`（15 帧 ≈ 0.5s），限幅 ±30% 帧间隔。修正量用原始 `avDiff` 不用 EMA（EMA 有滞后，用它驱动会过冲） |
| **ForceAlign** | 视频落后 > 6×帧间隔（>200ms） | 直接跳齐到音频 PTS（**不允许回退**） |
| **突发丢帧** | 视频超前 > 2×帧间隔，且本帧是突发到达（距上一产出帧 < 1/4 帧间隔） | 返回无效 PTS 让调用方丢帧，**不压缩 PTS**（压缩会挤出 <10ms 的间隔） |
| **Pause** | 音频停滞：`AudioClock::IdleUs() > stallIdleUs`（100ms） | 视频按系统时钟继续走，最多超前 `videoMaxLeadUs`（500ms）后钉住；补帧连续被 clamp 3 次则停产出；突发帧同样丢弃 |
| 首帧 | — | 取音频 PTS；超过 5s 视为上一轮残留时间轴，从 0 重起 |
| 音频不可用 | `audioRef < 0` | **不用系统时间顶替**，返回无效 PTS，由调用方决定丢帧或降级 |

停滞判定按**墙钟**（`AudioClock::IdleUs()`）而不是"连续几个视频帧间音频没推进"：
后者会把视频突发（若干帧在几 ms 内到达）误判成音频停滞，进而让 Pause 档按真实间隔推进、
挤出 <10ms 的 PTS 间隔。拿不到墙钟时（`audioIdleUs < 0`）才退回按帧计数（`stallFrames = 2`）。

相关参数（`VideoPtsParams`）：`maxLeadUs` 500000、`adaptStartFactor` 1、`adaptGainFrames` 15、
`adaptMaxPermille` 300、`trimGainFrames` 120、`forceSyncFactor` 6、`jumpWarnFactor` 5、
`stallIdleUs` 100000、`stallFrames` 2、`maxConsecutiveClampFill` 3、`staleTimelineUs` 5000000。

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
| | 采集卡顿恢复后突发灌帧（若干帧在几 ms 内到达） | 突发帧在视频超前时丢弃，不压缩 PTS | 不产生 <10ms 间隔，也不被误判成音频停滞 |
| | 背压丢帧（编码器 Pending ≥ 8） | PTS 不推进，avDiff 变大后由同步档位纠正；计入 `bpDrop` | 追实时性，不累积延迟 |
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
TGCapturer: 音画同步 avDiff=-13ms clamp=0 forceSync=0 adapt=0 stall=0 skip=0 jump=0 staleReset=0 syncDrop=0 bpDrop=0
```

| 字段 | 含义 | 健康区间 |
| - | - | - |
| `avDiff` | 音频 − 视频（>0 视频落后） | 绝对值 < 100ms |
| `clamp` | 被 clamp 到 audio+maxLead 的次数 | 偶发可接受，持续增长说明音频长期停滞 |
| `forceSync` | ForceAlign 直接跳齐的次数 | 少量正常，频繁说明音频时钟不稳 |
| `adapt` | RangeAdapt 步长确实被修正的次数 | 少量正常；持续增长说明帧率与音频长期不匹配 |
| `stall` | 判定"音频停滞"的次数 | 纯静音场景会增长，属预期 |
| `skip` | 建议丢弃的帧（补帧连续被 clamp + 视频突发超前） | 卡顿恢复期增长属预期 |
| `jump` | 音频 PTS 跳变 / 回退告警 | 0 最佳 |
| `staleReset` | 首帧命中残留时间轴的次数 | 0 最佳 |
| `syncDrop` | 视频超前音频（突发到达 / 音频停滞）被丢掉的帧 | 0 最佳 |
| `bpDrop` | 编码器背压（Pending 超上限）丢掉的帧 | 0 最佳；非 0 说明 CPU 或码率跟不上 |

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
| 缺数据 | 该路本帧按静音处理，已有残余数据保留到下一帧（不丢样本），不拖停整体时间轴 |
| 格式归一 + 补偿 | swresample 始终创建（**即使格式一致也走重采样器**），归一 + 漂移补偿同在此处完成 |
| 时间轴 | 输出 PTS = `base + 已输出样本数 / 输出采样率`，与实际样本数严格一致 |
| 线程安全 | `Push`（采集线程）与 `Mix`（混流线程）之间加锁 |

**出帧条件（输出时钟）**

所有"活跃通道"（`kChannelIdleUs` = 200ms 内推过数据）都凑够一帧才出帧；有通道迟到时最多等
`kMixWaitFrames` = 2 帧（墙钟超时 或 就绪通道已积压 2 帧，任一满足即出帧）。
旧实现"任一路凑够即出帧"在多路相位错开时会让每路各自触发出帧，输出帧数 ≈ 各路帧数之和。

**设备时钟漂移测量与补偿（每路独立）**

各路设备时钟相对墙钟的漂移**在样本域纠正**，不改写 PTS、也不尾部复制/截断样本（会有咔声）：

| 环节 | 做法 |
| - | - |
| 测量 | 5s 一个窗口，取窗口内"调用方时间戳 − 墙钟"偏移的平均值，相邻窗口均值之差即本窗口漂移（抖动被平均掉，不累积） |
| 补偿 | 下发给 `swr_set_compensation` 平滑伸缩样本 |
| 限速 | 每秒最多伸缩 5ms（≈0.5% 变速，不可闻）；`\|待补偿量\|` ≥ 2ms 才下发，两次下发至少间隔 1s，待补偿量上限 50ms |
| 缓冲上限 | 每路 300ms，超出一次性丢到一半（减少断点次数），计入 `ovf` |
| 超前上限 | 输出时间轴最多超前墙钟 200ms |
| 全路断流恢复 | 输出落后墙钟 > 500ms 时按墙钟前移输出 PTS（只前移不回退），计 `reanchor` |

**时间戳可信性**：调用方时间戳推进速率相对墙钟偏离 > 5%（`kTsRateTolPermille = 50`），
或时间戳未严格递增时，判定该路时间戳不可信 —— 打一条 Warn 并**整路改用墙钟**驱动
（`tsBad` / `fallback`），避免把错误的时钟域带进时间轴。

**诊断统计（`AudioChannelClock` 的 PI/BURST 保留为诊断，不再写输出 PTS）**

| 偏差 | 处理 | 计数 |
| - | - | - |
| 系统时钟跳变/冻结（>2s） | 限幅吸收（单次参考时钟推进上限 2s），不一次性追赶 | `jumpCount` |
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
| Android | 系统内存 NV12 → MediaCodec（暂无 Surface 直连，GPU 模式等价 CPU） | `h264_mediacodec` | `AV_PIX_FMT_NV12` |
| iOS | `CVPixelBuffer` → VideoToolbox | `h264_videotoolbox` | `AV_PIX_FMT_VIDEOTOOLBOX` |
| Windows | D3D11 纹理 → NVENC / MF / AMF | `h264_nvenc` / `h264_mf` / `h264_amf` | `AV_PIX_FMT_D3D11` |

硬件帧链路建不起来（hwdevice / 帧池失败）时退回系统内存路径，功能不受影响，只是不再零拷贝；
iOS 上收到 `CVPixelBuffer` 但编码器走软件链路时，锁住 buffer 回读到系统内存再编码。

**编码器回落**（`Prepare` 期间）：

| 请求的编码器 | 打不开时 | 通知 |
| - | - | - |
| 视频硬编（`*_mediacodec` / `*_videotoolbox` / `*_nvenc` / `*_amf` / `*_qsv` / `*_mf`） | 回落 `libopenh264` | `EsCap_Notify_Encoder_Fallback`，`content = "video:h264_mediacodec->libopenh264"`，`data = 0` |
| 视频软编（名字写错 / 未编进） | 不回落，`Prepare` 返回 `TGErr_OpenVideoEncoder` | — |
| 音频（未编进 / 打不开） | 回落内置 `aac` | 同上，`content = "audio:aac_at->aac"`，`data = 1` |
| 音频输入格式不对 | 不回落，返回 `TGErr_OpenAudioEncoder` | — |

实际使用的编码器以 `GetStatsInfo` / 日志里的 `codec=` 为准。

**编码器探测**：编进 ffmpeg 不等于本机能用（模拟器、老机型、无对应显卡）。想在开播前给用户列可选项：

```cpp
ITGCapturerMgr::IsEncoderAvailable("h264_mediacodec");   // 用最小参数真正 avcodec_open2 一次
ITGCapturerMgr::GetAvailableEncoders(true);              // "h264_mediacodec,libopenh264"（推荐顺序，硬编在前）
ITGCapturerMgr::GetAvailableEncoders(false);             // "libfdk_aac,aac"
```

Java：`TGCapturer.isEncoderAvailable(name)` / `TGCapturer.getAvailableEncoders(video)`（返回 `String[]`）。
结果进程内缓存；硬编首次探测要建一次真实编码会话（Android 模拟器上 h264_mediacodec 实测 40~340ms），
不要放在 UI 线程。两个 demo 启动时都会在后台探测一次，并把结果打到日志面板。

**音频采样格式**：混流器输出恒为交错 S16，`AudioEncoder` 按编码器能力自动适配：

| 编码器 | 采样格式 | 处理 |
| - | - | - |
| `libfdk_aac` | S16 | 直接送入，零转换 |
| 内置 `aac`（`aCodec` 不可用时的回退目标） | FLTP | swresample 转 S16 → FLTP（同采样率，不引入延迟） |

编码器同时支持 S16 与其它格式时优先 S16；输入不是交错 S16 时 `Open` 直接失败
（`TGErr_OpenAudioEncoder`，`extra = AVERROR(EINVAL)`），不会静默产出噪声。

---

## 9. 构建

命令行与 IDE **共用同一套 CMake 工程**，产物完全一致。Android 例外：App 和 AAR 由 Gradle（`ide/android`）驱动，
Gradle 内部仍然调用同一个根 `CMakeLists.txt`。各个 demo 的构建和使用方式见第 10 节。

### 9.1 命令行

```powershell
python scripts/build.py --platform windows --abi x64
python scripts/build.py --platform linux --3rd ~/TGCapture3rd-linux   # 需先编出 linux 版依赖
python scripts/build.py --platform android --abi arm64-v8a --ndk $ANDROID_NDK_HOME   # 只出内核 .a；App / AAR 见 10.3.1
python scripts/build.py --platform ios                                # 含 demo/ios App（未签名）
# 开关：--clean / --build-type Debug|Release / --no-demo / --3rd <依赖目录> / --ide
# iOS 签名：--team <Team ID> / --bundle-id <id>（写进 Xcode 工程，见 10.3.2）
```

### 9.2 IDE（VS2022 / Android Studio / Xcode）

一条命令生成工程并打开（**只生成，不编译**）：

```powershell
python scripts/build.py --platform windows --ide
python scripts/build.py --platform android --ide
python scripts/build.py --platform ios    --ide [--team <Team ID>]
```

| 平台 | IDE | 工程 | 说明 |
| - | - | - | - |
| Windows | VS2022 | `build/windows-x64/TGCapture.sln` | 只保留 `Debug`/`Release`；`tg_capture_demo` 为启动工程；调试环境变量已带 ffmpeg DLL 目录，**F5 直接跑** |
| Android | Android Studio | `ide/android` | Gradle 宿主工程：`:tgcapture`（`bindings/android`）通过 `externalNativeBuild` 驱动根 `CMakeLists.txt`，`:demo` 是 App |
| iOS | Xcode | `build/ios-fat-xcode/TGCapture.xcodeproj` | `--ide` 自动切换到 Xcode 生成器；`TGCaptureDemo` 为默认 scheme |

* Android 换 ABI 时，`bindings/android` 和 `demo/android` 里的 `abiFilters` 要一起改；
  NDK / CMake 版本在 `ide/android/gradle.properties` 里用 `tg.ndkVersion` / `tg.cmakeVersion` 覆盖（默认 `29.0.14033849` / `4.1.1`）。

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

| 平台 | 内核 | demo |
| - | - | - |
| Linux | `build/linux-x86_64/libtgcapture.a` | 命令行：`build/linux-x86_64/tg_capture_demo`（外灌帧，不依赖采集设备） |
| Windows | `build/windows-x64/Release/tgcapture.lib` | 命令行：`build/windows-x64/Release/tg_capture_demo.exe`（+ 同目录 ffmpeg DLL） |
| Android | `bindings/android/build/outputs/aar/tgcapture-release.aar`（内含 `libtgcapture_jni.so`）；`build.py` 只出 `build/android-arm64-v8a/libtgcapture.a` | UI：`demo/android/build/outputs/apk/debug/demo-debug.apk` |
| iOS | `build/ios-fat/libtgcapture.a` | UI：`build/ios-fat/demo/ios/TGCaptureDemo.app`（命令行构建，未签名）；装到真机请用 Xcode 工程 |

---

## 10. Demo（命令行 / UI 两类）

### 10.1 分类一览

| 类别 | 平台 | 目录 | 形态 | 构建方式 | 视频源 | 音频源 | 主要用途 |
| - | - | - | - | - | - | - | - |
| **命令行** | Windows | `demo/windows` | 控制台 `tg_capture_demo.exe` | 根 `CMakeLists.txt`（`build.py` / VS2022） | 屏幕 DXGI·GDI、窗口、人造帧 | WASAPI 麦克风 + 扬声器 | 原生采集、GPU 链路、重开 / 并发回归 |
| **命令行** | Linux | `demo/linux` | 控制台 `tg_capture_demo` | 根 `CMakeLists.txt`（`build.py`） | 外灌：系统 `ffmpeg` 解素材 → NV12 | 外灌：两路素材 → S16 | 编码 / 混流 / 音画同步压测、故障注入、CI 验收 |
| **UI** | Android | `demo/android` | Kotlin App（APK） | Gradle（`ide/android`，`:demo` 模块） | MediaProjection 录屏、测试图案 | 麦克风、系统内录、测试音 | 移动端开播联调、模拟器自测 |
| **UI** | iOS | `demo/ios` | UIKit App（.app） | 根 `CMakeLists.txt` → Xcode 工程 | ReplayKit（App 内画面）、测试图案 | 麦克风、测试音 | 移动端开播联调（仅真机） |

两类 demo 的区别：

* **命令行 demo** 一次运行 = 一次开播，参数全部在命令行，跑完自动 `Stop` 并打印统计，适合脚本化回归和 CI。
* **UI demo** 把参数做成设置页，可以手动反复开播 / 停播、切换静音和关画面，日志面板会显示内核回调。
  它们只调用公开 API（Android 用 `bindings/android` 的 Java 接口，iOS 直接用 C++ 接口），也可以当作接入示例来看。

两类都依赖 `TGCapture3rd`（依赖目录的指定方式见 9.3）。

### 10.2 命令行 demo

#### 10.2.1 Windows（`demo/windows`）：原生采集

**构建**

```powershell
# 命令行：产物 build\windows-x64\Release\tg_capture_demo.exe（ffmpeg DLL 已自动拷到同目录）
python scripts/build.py --platform windows --abi x64 [--build-type Debug]

# IDE：生成并打开 build\windows-x64\TGCapture.sln，tg_capture_demo 是启动工程，F5 直接运行
python scripts/build.py --platform windows --ide
```

在 VS 里调试时，命令行参数填在 `tg_capture_demo` → 属性 → 调试 → 命令参数。

**运行**

```powershell
cd build\windows-x64\Release
.\tg_capture_demo.exe --help
.\tg_capture_demo.exe --out out.flv --seconds 3            # 录 3 秒屏幕 + 麦克风
.\tg_capture_demo.exe --out rtmp://127.0.0.1/live/stream   # 推流
```

##### 参数

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

##### 回归用例

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
[TGCapturer] 音画同步 avDiff=…ms clamp=0 forceSync=0 adapt=0 stall=0 skip=0 jump=0 staleReset=0 syncDrop=0 bpDrop=0
```

##### 产物校验

```powershell
$env:PATH = "D:\bertonwang\bing\TGCapture3rd\ffmpeg\windows\bin;" + $env:PATH
ffprobe -v error -show_entries stream=codec_name,width,height,channels,sample_rate -of csv=p=0 out.flv
ffprobe -v error -show_entries format=duration,size -of default=nw=1 out.flv
ffplay out.flv                      # 实际播放确认音画同步
```

推流场景可先起本地服务验证：`ffmpeg -f flv -listen 1 -i rtmp://0.0.0.0/live/stream -c copy out.flv`，
然后 `--out rtmp://127.0.0.1/live/stream`。

##### 实测结果（当前版本）

> 以下除"音画同步"一行外均为既有结果；音画同步的判据已随本次修复收紧
> （基线稳态 `avDiff` 由 ±25ms 收敛到 ±12ms），Windows 侧待复测后更新数值。

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

#### 10.2.2 Linux（`demo/linux`）：外灌帧（真实素材 1 视频 + 2 音频）

Linux 没有原生采集，但是**整条编码 / 封装 / 混流 / 音画同步链路都可以用外灌帧跑通**。
`demo/linux/main.cpp` 用系统 `ffmpeg` CLI 把素材解成裸 NV12 / S16LE PCM 经管道喂入，
两路音频走不同的 `AudioCapSourceType` 各占独立混流通道，由内核做 sample-wise mix。

**构建**

```bash
# 需先用 FfmpegCompileShell 编出 linux 版依赖；运行时还要系统 ffmpeg CLI（拿来解码素材）
python3 scripts/build.py --platform linux --3rd ~/TGCapture3rd-linux
# 产物：build/linux-x86_64/tg_capture_demo（同时编出 tg_tests，见 10.4）
```

默认素材路径写死在 `demo/linux/main.cpp`（`/data/home/bertonwang/mov/…`）。换机器时请用 `--video` / `--audio1` / `--audio2` 指定。

##### 参数速查

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
| `--audio-pts-jitter-ms <N>` | 0 | 音频 PTS ±N ms 随机抖动（模拟采集端时间戳不准） |
| `--audio-pts-back-every <K>` | 0 | 每 K 个音频帧将 PTS 回退 1 帧时长（模拟设备时钟回退） |
| `--video-stall-start <s> --video-stall-dur <s>` | 0 0 | 视频从第 s 秒起暂停 dur 秒不推帧 |
| `--video-stall-mode <m>` | burst | `freeze` = 卡顿期间的帧直接丢弃；`burst` = 卡顿期间的帧积压，恢复后一次性灌入（模拟采集卡顿恢复） |
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
- 混流器 5s 窗口测出该路漂移后，用 swr 平滑伸缩样本补偿（每秒最多 5ms），
  补偿不掉的部分由欠载 / 缓冲上限吸收，统计落在 `drift` / `comp` / `drop` / `ovf`
- VideoPtsGenerator 侧对视频 PTS 做 `adapt`（RangeAdapt 比例追赶）/ `forceSync`（强制跳齐）

推送节奏本身用浮点步长（`due = seq × 帧长`）而非整数累加：整数累加的截断误差相当于
凭空注入约 -41ppm 的漂移，会让本该干净的基线也出现 `adapt`。
每路一个独立预读线程 + 有界队列（约 500ms），主循环只从队列取块，
某一路管道抖动不会拖住其它路的节奏（旧实现单线程阻塞 `fread`，三路会一起停）。

##### 典型用例

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

##### 日志判读

**运行期心跳（10s / 条，MixLoop 发出）** — 长压测时在时间轴上就能看到 drift 分布，不用等 Stop 聚合：

```
[I] TGCapturer: 心跳 avDiff=-1ms force=0 adapt=0 clamp=0 stall=0 skip=0 jump=0 staleReset=0 bpDrop=0
    mix[out=2569 wait=0 reanchor=0/0ms]| ch0[drift=0us/5s jump=0 under=0 comp=0ms drop=0ms ovf=0ms tsBad=0 buf=1007]
    ch1[drift=0us/5s jump=0 under=0 comp=0ms drop=0ms ovf=0ms tsBad=0 buf=1007]
```

- 外层（视频 PTS 生成器）：`avDiff` < 1 帧时长 OK；`force/adapt` 持续累加表明内核在吸收 drift
- `mix[]`：`out` 已输出帧数；`wait` 等待迟到通道超时后出帧的次数；`reanchor` 断流恢复后前移时间轴的次数/累计量
- `ch<N>`（每路音频独立）：
  - `drift` 最近一个 5s 窗口测得的漂移；稳态下近 0
  - `under` 活跃但迟到的次数；持续增长说明本路补货率 < Mix 消费率（部分段按静音处理）
  - `comp` / `drop` swr 拉伸 / 压缩的累计量（限速 5ms/s）；`ovf` 缓冲超上限丢弃量
  - `tsBad` 时间戳不可信次数，带 `(wall)` 表示已改用墙钟驱动
  - `buf` 该路 pcm 积压帧数；长期接近上限说明消费不过来
- `clamp / force / adapt / stall / skip / jump / staleReset / bpDrop` 基线场景应恒为 0 — 任何非零都要查

**结束聚合（Stop 时一次性输出）**：

```
[I] TGCapturer: 混流汇总 out=2583 wait=0 reanchor=0/0ms
[I] TGCapturer: 混流通道 0 under=0 comp=0ms drop=0ms ovf=0ms drift=0us/5s tsBad=0 fallback=0 jump=0
[I] TGCapturer: 混流通道 1 under=0 comp=0ms drop=0ms ovf=0ms drift=0us/5s tsBad=0 fallback=0 jump=0
[I] TGCapturer: 音画同步 avDiff=0ms clamp=0 forceSync=0 adapt=0 stall=0 skip=0 jump=0 staleReset=0 syncDrop=0 bpDrop=0
[demo] 输出=A.flv 视频帧=1800 音频1=2584 音频2=2584 拒绝=0 resync=0 freeze丢帧=0 预读欠载=v0/a0/a0
```

**demo 推送心跳（30s / 条）** — 验证 demo 自身仍在供帧 + 校核 speed 效果：

```
[demo] t=30.000s video+901(=1801) a1+1227(=2455) a2+1383(=2766) rej=0 resync=0 underflow=v0/a0/a0
```

`a2+ / a1+` 比值应接近 `audio2-speed / audio1-speed`（本例 `1383/1227≈1.126 ≈ 1.07/0.95`）。
`underflow=v/a/a` 是三路预读队列被取空的次数（非 0 说明该路供给跟不上）；
`resync` 是落后超过 200ms 后重新对齐系统时钟的次数。

**运行期告警**：`混流超过 500ms 无输出（当前通道数 N，全部断流或未推数据）` 才是异常
—— 只在持续 500ms 没有任何输出时才提示（3s 节流一条）。
混流按输出时钟出帧，轮询时取空属于正常节律，不需要告警。

##### 故障注入矩阵（压测用）

每个场景 60s，都带 `--video-loops 0 --audio1-loops 0`：

| 场景 | 命令片段 | 预期信号（实测 60s） |
| - | - | - |
| **A** 基线 | （不加故障参数） | 全部计数为 0；`avDiff` 全程 ≤ ±12ms；`drift=0us/5s` |
| **B** 音频 drift | `--audio1-speed 0.95 --audio2-speed 1.05` | 慢路 `under=117 comp=249ms`（补静音），快路 `ovf=2650ms drop=249ms`；`drift=±250ms/5s` |
| **C** 音频 PTS 抖动 | `--audio-pts-jitter-ms 100` | 两路 `tsBad=1 fallback=1`（时间戳未严格递增 → 改用墙钟）；之后 `drift≈0` |
| **D** 音频 PTS 回退 | `--audio-pts-back-every 100` | 两路 `tsBad=1 fallback=1`；输出无断档（`aGap` 异常 0） |
| **E** 视频卡顿 burst | `--video-stall-start 20 --video-stall-dur 5 --video-stall-mode burst` | `skip=140 force=1`；demo 侧 `预读欠载=v292`；卡顿期间 1 次 >100ms 间隔（卡顿本身） |
| **E2** 视频卡顿 freeze | 同上 + `--video-stall-mode freeze` | demo 侧 `freeze丢帧=150`；内核 `skip=0`；`avDiff` 恢复后回 0 |

每种故障**都有专属响应字段**：如果某场景跑出来跟 baseline 完全一样，说明通路没正确传递 PTS。

##### 验收判据

产物侧（用 `ffprobe` 导 `pts_time` 后统计）：

| 指标 | 判据 |
| - | - |
| flv `Duration` | 接近 wallclock（能读到说明 trailer 正常写入；读不到说明进程没走完 `Stop`） |
| 视频 PTS 间隔 < 10ms | **0 次**（这是本次修复的重点：突发帧压缩 PTS 会挤出这种间隔） |
| 视频 PTS 间隔 > 100ms | 仅卡顿场景允许 1 次（卡顿本身） |
| 音频 PTS 间隔异常（<16ms 或 >30ms） | 0 次 |
| 最终 `avDiff` | ≤ ±12ms（音频时钟 1024 样本量化抖动下限） |

##### 已知时间轴行为

两路音频 `speed` 不同时，输出帧数由**输出时钟**决定而非各路之和
（旧实现会让输出帧数 ≈ 各路帧数之和，60s 墙钟产出 114s 音频，flv Duration 虚增一倍）。
改按 `base + 已输出样本数 / 采样率` 累加后，Duration 与 wallclock 一致（实测 60s → 60.02s）。

### 10.3 UI demo

Android 和 iOS 两个 App 的界面和设置项是对齐的：

* **上半部分**：状态（空闲 / 等待授权 / 连接中 / 直播中）、开始 / 停止按钮、静音、关画面、实时码率和统计、事件日志。
  日志里会显示 `onPrepared` / `onError` / `onNotify` 等回调，包括 `EncoderFallback`。
* **下半部分**：设置页，直播期间置灰不可改。每次点「开始」时读一次设置，所以改完设置要停播后重新开始才生效。
* **启动时**：在后台探测一次本机可用的编码器，结果写进日志（`本机可用编码器 视频: … 音频: …`），见第 8 节。

| 设置分组 | 设置项 | 说明 |
| - | - | - |
| 输出 | 输出方式 / 推流地址 / 封装格式 | 推流（RTMP / SRT）或录制到本地文件；`flv` / `mpegts` |
| 视频 | 视频源 | **屏幕录制**或**测试图案**（NV12 彩条 + 移动白块，经 `PushExternalVideoFrame` 注入，模拟器 / 排查采集问题用） |
| | 分辨率 / 方向 / 帧率 / 码率 / 关键帧间隔 | 540P / 720P / 1080P；方向可选跟随屏幕、横屏、竖屏 |
| | 视频编码器 / 性能模式 | 硬编或 `libopenh264`；CPU / GPU。硬编打不开时内核会自动回落 |
| 音频 | 麦克风 / 系统内录 / 测试音 | 三者可叠加，由内核混流；测试音是 1kHz 正弦，每秒响 0.5s，便于听出断音 |
| | 编码器 / 码率 / 采样率 / 声道 | — |
| 高级 | 断流告警阈值 | 即 `SetAVTimeout` |

#### 10.3.1 Android（`demo/android`）

**工程结构**：Gradle 宿主工程在 `ide/android`，它本身不放源码，只做聚合：

| Gradle 模块 | 目录 | 内容 |
| - | - | - |
| `:tgcapture` | `bindings/android` | SDK（AAR）：Java API `com.tencent.tgcapture.*` + JNI 胶水，生成 `libtgcapture_jni.so`。通过 `externalNativeBuild` 引入仓库根目录的 `CMakeLists.txt` |
| `:demo` | `demo/android` | Kotlin 开播测试 App，依赖 `:tgcapture` |

改了内核 C++ 后直接 Run，Gradle 会增量重编 `.so`。

**环境要求**：JDK 17（AGP 8.10 要求；可以直接用 Android Studio 自带的 JBR）、Android SDK 36、
NDK `29.0.14033849`、CMake `4.1.1`。目前只编 `arm64-v8a`，可以跑在 arm64 真机或 Apple Silicon 上的 arm64 模拟器上。

| 本机版本不一致时 | 改法 |
| - | - |
| NDK / CMake 版本 | `ide/android/gradle.properties` 中设置 `tg.ndkVersion=…` / `tg.cmakeVersion=…` |
| 依赖目录 | `-PTG_3RD_ROOT=<dir>`，或环境变量 `TG_3RD_ROOT`；都不给时自动探测同级的 `TGCapture3rd` |
| ABI | `bindings/android` 和 `demo/android` 两个 `build.gradle.kts` 里的 `abiFilters` 要一起改，并确保 `TGCapture3rd` 有对应 ABI 的库 |

**构建**

```bash
# IDE：Android Studio 打开 ide/android（或执行下面这条，会打印路径并打开）
python3 scripts/build.py --platform android --ide
#   运行配置选 demo，点 Run 即可编译、安装并启动

# 命令行（JAVA_HOME 指向 JDK 17）
cd ide/android
export JAVA_HOME="/Applications/Android Studio.app/Contents/jbr/Contents/Home"   # macOS 示例
./gradlew :demo:assembleDebug      # → demo/android/build/outputs/apk/debug/demo-debug.apk
./gradlew :demo:installDebug       # 编译并安装到已连接的设备 / 模拟器
./gradlew :tgcapture:assembleRelease   # 只出 SDK：bindings/android/build/outputs/aar/
```

> `python3 scripts/build.py --platform android --ndk …` 只编内核静态库 `libtgcapture.a`，不含 JNI，也不生成 App；
> 要构建 demo 请走 Gradle。

**使用**

1. 打开 App，日志里会先出现本机可用编码器。
2. 在设置页选好输出方式和视频 / 音频源，点「开始」。
   * 选了麦克风或内录：会申请录音权限（Android 13+ 还会申请通知权限，拒绝不影响开播）。
   * 选了屏幕录制或系统内录（需 Android 10+）：会弹出系统录屏授权，同意后启动前台服务 `TGProjectionService` 拿到 MediaProjection。
   * 两者都没选（测试图案 + 测试音）：不申请任何权限，直接开播。
3. 直播中可以切换静音或关画面；点「停止」结束。推流写失败（`MuxWriteFailed`）时会自动停播。

| 场景 | 推荐设置 |
| - | - |
| 模拟器自测（无屏幕授权、无麦克风） | 视频源选测试图案，音频只开测试音，输出到本地文件 |
| 真机录屏推流 | 视频源选屏幕录制，开麦克风，推流地址填 `rtmp://…` |
| 系统内录 | 打开「系统内录」，只能录到允许被捕获的 App 的声音（AudioPlaybackCapture 的限制） |

**取产物和日志**

```bash
adb pull /sdcard/Android/data/com.tencent.tgcapture.demo/files/records   # 本地录制 tgcapture_<时间>.flv|ts
adb pull /sdcard/Android/data/com.tencent.tgcapture.demo/files/logs      # 内核滚动日志
adb logcat -s 'TGCapture:*'                                              # 完整内核日志（界面上只显示回调事件）
```

**命令行驱动 UI demo**（无人值守回归时用；设置项保存在 SharedPreferences 里，key 见 `res/xml/preferences.xml`）：

```bash
P=com.tencent.tgcapture.demo; F=shared_prefs/${P}_preferences.xml
adb shell run-as $P cat $F > prefs.xml          # 导出当前设置，按需修改，例如 pref_video_codec / pref_perf_mode
adb push prefs.xml /data/local/tmp/p.xml && adb shell "cat /data/local/tmp/p.xml | run-as $P sh -c 'cat > $F'"
adb shell am force-stop $P && adb shell am start -n $P/.MainActivity
adb shell input tap <x> <y>                     # 点开始 / 停止；坐标可用 uiautomator dump 查 btn_start
```

模拟器（API 36，`sdk_gphone64_arm64`）上的实测结果：测试图案 + 测试音录制到文件，`h264_mediacodec` 在 CPU / GPU 模式下都正常出流。
把编码器设成本机不存在的 `h264_videotoolbox` / `aac_at` 时，日志出现两条 `EncoderFallback`，回落到 `libopenh264` + `aac` 后录出的文件可以正常解析。

#### 10.3.2 iOS（`demo/ios`）

**工程结构**：`demo/ios/CMakeLists.txt` 由仓库根目录的 `CMakeLists.txt` 引入（条件：`TG_PLATFORM=ios` 且 `TG_BUILD_DEMO=ON`，默认开启），
生成 `TGCaptureDemo` 这个 App target。代码是纯 UIKit，没有 storyboard；部署目标为 iOS 14。

> `TGCapture3rd` 的 iOS 库**只有 arm64 真机切片**，所以 demo 只能跑在真机上，不支持模拟器。

**构建**

```bash
# IDE（推荐）：生成 build/ios-fat-xcode/TGCapture.xcodeproj 并用 Xcode 打开，scheme 默认已选 TGCaptureDemo
python3 scripts/build.py --platform ios --ide --team <10 位 Team ID> [--bundle-id com.xxx.tgdemo]
#   连上真机，点 Run。Team ID 也可以用环境变量 TG_IOS_TEAM_ID 传入；
#   个人账号需要把 Bundle ID 改成全局唯一值（默认 com.tencent.tgcapture.demo）

# 命令行（Makefiles）：生成 build/ios-fat/demo/ios/TGCaptureDemo.app（未签名，只用于检查能否编过）
python3 scripts/build.py --platform ios [--build-type Debug]
# 不需要 demo 时加 --no-demo，只出 build/ios-fat/libtgcapture.a
```

> 签名信息要通过 `--team` / `--bundle-id` 传入。在 Xcode 里手动改的 Signing 设置，下次重新生成工程时会被覆盖。
> Xcode 工程（`ios-fat-xcode`）和命令行构建（`ios-fat`）使用不同目录，因为同一目录换生成器时 CMake 会报错。

**使用**

1. 首次安装后，需要在「设置 → 通用 → VPN 与设备管理」里信任开发者证书。
2. 打开 App，日志里会先出现本机可用编码器。默认配置为 `h264_videotoolbox` + `aac_at` + GPU 模式。
   当前 iOS 版 ffmpeg 没有编进 `aac_at`，所以开播时日志会出现一条 `EncoderFallback audio:aac_at->aac`，属于正常现象；
   想用 fdk，在设置里把音频编码器改成 `libfdk_aac`。
3. 点「开始」：开麦克风时会申请麦克风权限；选屏幕录制时 ReplayKit 会弹出录屏确认。
4. 静音、关画面、停止的用法和 Android 一致。

| 限制 | 说明 |
| - | - |
| 屏幕录制范围 | ReplayKit 应用内录制，**只能录本 App 的画面**；系统级录屏需要 Broadcast Upload Extension（未实现） |
| 系统内录 | 不支持（同样需要 Broadcast Upload Extension），设置页里没有这一项 |

**取产物和日志**：Info.plist 已开启文件共享（`UIFileSharingEnabled`），可以在「文件」App → 我的 iPhone → TGCaptureDemo，
或者 Finder 的设备页 → 文件，取出 `Documents/records`（本地录制文件）和 `Documents/logs`（内核日志）。
Xcode 运行时，内核日志同时会输出到控制台（`Init(…, printLog=true)`）。

### 10.4 自动化检查（单元测试 / 验收）

```bash
python3 scripts/build.py --platform linux          # 同时编出 tg_tests 和 demo
build/linux-x86_64/tg_tests                        # 单元测试（也可 ctest；可带名称过滤，如 tg_tests Bus）
build/linux-x86_64/tg_tests AudioEncoder           # 只跑音频编码器：真实 ffmpeg 编码 + 解码校验波形
scripts/accept_av.sh                               # 音画同步验收（6 个场景并行，约 20s，失败返回非 0）
```

CI 平台里直接调用 `scripts/ci.sh`：先构建再跑单测；设置 `TG_CI_ACCEPT=1` 时再加跑验收，验收需要素材 `TG_MEDIA_DIR` 和系统 ffmpeg。
依赖目录由 `TG_3RD_ROOT` 指定（见 9.3）。仓库不绑定具体的 CI 平台，没有提交 workflow 文件。

`tg_tests` 在根 `CMakeLists.txt` 里只对 linux / windows 启用。在 Android 上跑的方法：用 NDK 交叉编译一份
（新建一个 wrapper 工程，`add_subdirectory` 仓库根目录，再加 `tg_tests` target），然后 `adb push` 到 arm64 设备或模拟器上的 `/data/local/tmp` 执行。
当前 88 个用例在 Android arm64 模拟器上全部通过。

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
* **Android / iOS 尚未在真机上运行验证**：Android 端已在 arm64 模拟器（API 36）上用 UI demo 跑通了测试图案 / 测试音
  → `h264_mediacodec` / `libfdk_aac` → 本地文件这条链路，`tg_tests` 也在模拟器上通过（见 10.3.1、10.4）；
  但真机上的录屏、麦克风、内录还没验证过。iOS 只验证了编译（真机 arm64，Xcode），还没有在设备上运行。
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
* **无自动重连**：封装器写失败后只上报 `TGErr_MuxWriteFailed`，由宿主决定 `Stop` + 重新 `Prepare`。
  RTMP 真实断流（对端不收包、半开连接）下 `Stop()` 能否在 `ioTimeoutMs` 内返回，目前只用本地 TCP
  黑洞服务验证过，未在真实 RTMP 服务上验证。
* **Windows 未重新验证**：Bus 做实、回调出锁、Muxer 中断等改动目前只在 Linux 上构建并回归。
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
| 本进程 `signal(SIGPIPE, SIG_IGN)` 后再起子进程 | 子进程继承 `SIG_IGN`，读端关闭时它只拿到 `EPIPE` 而不被终止；若子进程是无限循环源（如 `aloop=loop=-1`），`pclose` 会永久阻塞 | 退出路径要主动 kill 子进程再 `pclose`（见 `demo/linux/main.cpp` 的 `FfmpegPipe::Kill`）；跑完顺手确认没有残留 ffmpeg 占 CPU |
| 算同步偏差时用"上一帧 PTS" | 偏差系统性偏大一个帧间隔，控制器会把视频推到超前约 1 帧处才平衡，基线永远不收敛 | 用"本帧按标称间隔推进后"的 PTS 计算，见 5.1 |
| 在节点里直接持有下游指针 | 绕开 Bus，Link 关系与背压反馈失效 | 一律 `Forward()`；背压看 `DownstreamPending()` |
| 在 `m_lifecycleMutex` 里回调宿主 | 宿主回调里调 `Stop()` / 取别的锁时死锁 | 一律经 `m_bus.Publish*`，且在解锁后触发 |
| 宿主回调慢 | 事件队列满后 Notify 被丢（Error 保留），`evDrop` 增长 | 回调里只做转发，别做重活 |
| 日志里打印推流地址 | 地址常带鉴权 token / 密码 | 一律 `core::RedactUrl()` 后再打 |
| `flush_packets` 设在 `priv_data` 上 | 对 flv / mpegts 没有效果 | 低延迟用 `AVFMT_FLAG_FLUSH_PACKETS`（设在 `AVFormatContext::flags`） |
| `Worker::Loop()` 抛异常 | 直接 `terminate`，不留日志 | 子类 `Loop` 目前没有 try/catch 兜底，新增循环体自己保证不抛 |
| 改 `VideoEncoder::Close()` | 漏复位 `m_lastPtsUs` 会让重开的全视频帧被判成 PTS 回退丢弃 | 新增"随流状态"时一并考虑复位 |
| 音频编码器换型 / 新增回退目标 | 把 S16 位模式直接写进 FLTP 平面 → `Input contains NaN`，1s 后上报 `TGErr_AudioEncodeFailed`(-103)，产物里没有音频 | 不要假设编码器收 S16，按 `sample_fmt` 走 `AudioEncoder` 里的 swr 转换；`tests/test_audio_encoder.cpp` 会解码校验波形 |
