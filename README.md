# TGCapture —— 开播内核（C++20）

用 C++20 重写的开播（采集 → 编码 → 封装 → 推流）内核，**对外接口保持不变**，
仍然基于 FFmpeg API 实现，支持 Windows / Android / iOS 三端。

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
| 日志 | 自带 `core/Logger.h`（零第三方依赖），支持 level 与自定义 sink 转发宿主 |

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
| `videoMaxLagUs` | 120000 | 视频落后音频超过多少丢帧 |
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

默认输出到 `stdout`，格式 `[TGCapture][level] msg`。
**当前限制**：`Init` 的 `logDir` / `printLog` 尚未实现文件落盘；
Android / iOS 还没接 `__android_log_print` / `os_log` sink（移动端需先用 `SetSink` 转发）。

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

### 5.1 视频 PTS 生成规则

| 场景 | 处理 |
| - | - |
| 首帧 | 直接取音频 PTS（超过 1 小时视为残留时间轴，从 0 起） |
| 音频正常 + 真实帧 | 偏离 > 2×帧间隔 → 跳齐音频（**不允许回退**）；否则按帧间隔递增 |
| 音频正常 + 补帧 | 渐进加速/减速：落后 150/300/500ms → ×1.1/1.2/1.3；超前 → ×0.9/0.8/0.7 |
| 音频停滞（静音/卡顿） | 视频最多超前 500ms 后 clamp；补帧连续被 clamp 3 次就停产出 |
| clamp / 跳齐 | 均带单调递增保护（最小步长 = 帧间隔/10，下限 1ms） |
| **音频不可用** | **不用系统时间顶替**；无音频时走 `VideoOnlyTimeline`（采集时间轴，首帧归零） |
| 兜底 | 编码器侧再看一次 `videoMaxLagUs`，并丢弃 PTS 回退的帧（防 `non monotonically increasing dts`） |

### 5.2 统计与判读

停止时打印一行，用来判断链路是否健康：

```
TGCapturer: 音画同步 avDiff=-16ms clamp=0 forceSync=0 skip=0 jump=0 syncDrop=0
```

| 字段 | 含义 | 健康区间 |
| - | - | - |
| `avDiff` | 音频 − 视频（>0 视频落后） | 绝对值 < 100ms |
| `clamp` | 被 clamp 到 audio+500ms 的次数 | 偶发可接受，持续增长说明音频长期停滞 |
| `forceSync` | 直接跳齐音频的次数 | 少量正常 |
| `skip` | 建议丢弃的补帧 | 同上 |
| `jump` | 音频 PTS 跳变告警 | 0 最佳 |
| `syncDrop` | 因音频停滞丢掉的视频帧 | 0 最佳 |

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

每次跑完看两处：

```
[demo] 最终统计: {"codec":"...","encoded":N,"dropped":0,"pending":0}
[TGCapturer] 音画同步 avDiff=…ms clamp=0 forceSync=0 skip=0 jump=0 syncDrop=0
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
| 音画同步 | `avDiff` 稳定在 ±50ms 内，无 clamp / 跳变 / 丢帧 |
| GPU 全链路 | `h264_nvenc` + D3D11 帧池（fmt=`AV_PIX_FMT_D3D11`, sw=`BGRA`），3 秒 98 帧，文件可解析 |
| 稳定性 | 同配置连跑 5 次均 `exit=0`、产物有效 |

---

## 11. 已知限制 / 后续项

* **GPU 模式不做缩放**：显存内只做同尺寸拷贝，因此 GPU 模式**编码分辨率 = 采集分辨率**
  （4K 屏即 2160x3840）；需要降分辨率推流用 CPU 模式。后续可在显存内加 D3D11 VideoProcessor / `scale_d3d11`。
* **Android / iOS 尚未在各自平台编译验证**（本机只有 Windows 工具链）：
  `src/platform/android/*`、`src/platform/ios/*` 与 `TGCaptureSystemCapture.java`
  需在有 SDK/NDK/Xcode 的环境跑一次。
* **日志**：`Init` 的 `logDir` / `printLog` 未实现；移动端未接 `__android_log_print` / `os_log` sink（见 4.6）。
* 暂停静帧、水印、本地文件采集为接口占位（返回 0 或告警）。
* `GetStatsInfo` 当前返回基础 JSON（码率 / 队列 / 编码器统计）。
