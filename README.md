# TGCapture —— 开播内核（C++20）

用 C++20 重写的开播（采集 → 编码 → 封装 → 推流）内核，**对外接口保持不变**，
仍然基于 FFmpeg API 实现，支持 Windows / Android / iOS 三端，产物依赖 `TGCapture3rd`（由 `FfmpegCompileShell` 编译产出）。

## 1. 设计要点

| 要求 | 落地 |
| - | - |
| C++20 | `std::jthread` + `stop_token` 线程、`shared_ptr` 管理帧、`std::optional`、结构化绑定 |
| 接口不变 | `include/TGCapture/TGCapturerInterface.h` 保持原枚举/结构体/源接口；`compat/ESMediaXCapturer.h` 把 `ESMediaX::ESCapturer::IESMediaXCapturer` 直接映射到新内核，老调用方换一行 include 即可 |
| 仍用 ffmpeg API | 编码器/封装全部走 `avcodec` / `avformat` / `swscale` / `swresample`，不引入别的编解码栈 |
| 通用跨平台 | `platform/windows` `platform/android` `platform/ios` 分别实现采集与硬编直连，内核层无平台分支 |
| 线程基类 | `core/Thread.h`：`Worker` + `jthread`，析构即停止并 join；停止用线程自身的 `stop_token`（避免 Stop 挂死） |
| 软编 + 硬编直连 | 见第 4 节 |
| 总线 | `core/Bus.h`：节点注册 + 有界队列解耦 + 队列深度背压 |
| 帧管理 | `core/Frame.h`：`shared_ptr<Frame>`，AVFrame/AVPacket 用带删除器的 `shared_ptr` 托管（不再手工 `av_frame_free`） |
| 音画同步 | `core/Clock.h`：以音频输出 PTS 为主时钟，视频落后超阈值丢帧、超前则等待 |

## 2. 目录

```
TGCapture/
├── include/TGCapture/          对外接口（+ compat 兼容层）
├── src/
│   ├── core/                   Frame / Bus / Thread / Queue / Clock / Logger / MediaTypes
│   ├── codec/                  VideoEncoder / AudioEncoder（软编 + 硬编直连）
│   ├── capture/                VideoSource / AudioSource 抽象
│   ├── mux/                    Muxer（flv/rtmp、mpegts/srt、文件）
│   ├── platform/windows/           屏幕(DXGI/GDI) + 窗口 + WASAPI 麦克风/扬声器
│   ├── platform/android/           MediaProjection 屏幕 + AudioRecord/内录（JNI 桥接）
│   ├── platform/ios/               ReplayKit 屏幕 + AVAudioEngine 麦克风（.mm）
│   ├── TGCapturer.cpp          门面实现（状态机、回调、统计、背压）
│   └── ITGCapturer.cpp         Create/Destroy/Init/UnInit
├── demo/windows/               控制台测试 demo
├── cmake/Deps.cmake            TGCapture3rd 依赖定位
├── ide/android/                Android Studio 宿主工程（gradle + externalNativeBuild）
├── CMakeLists.txt
└── scripts/build.py            python 构建脚本（windows/android/ios，含 --ide）
```

数据流（总线串联，节点间有界队列 + 背压）：

```
VideoSource ─┐
             ├─> VideoEncoder ─┐
AudioSource ─┴─> AudioEncoder ─┴─> Muxer ─> rtmp://  srt://  file
```

## 3. 采集能力（屏幕 / 窗口 / 麦克风 / 扬声器）

全部走**系统原生采集**，不接 GVoice、不接任何第三方采集 SDK。

### 3.1 能力矩阵

| 能力 | Windows | Android | iOS | 说明 |
| - | - | - | - | - |
| 屏幕录制 | ✅ `DXGI` / `GDI` | ✅ `AndroidSurface`（MediaProjection） | ✅ `ReplayKit` | Win 默认 DXGI Desktop Duplication，不可用时自动回落 GDI |
| 指定窗口 | ✅ `WGC` / `WGC_No_TBar` | ❌ | ❌ | Android/iOS 没有"窗口"概念 |
| 麦克风 | ✅ WASAPI | ✅ AudioRecord | ✅ AVAudioSession + AVAudioEngine | 均为系统默认设备，Win 可指定端点 |
| 扬声器 / 系统内录 | ✅ WASAPI loopback | ✅ AudioPlaybackCapture（Android 10 / API 29+） | ❌ | iOS 需 Broadcast Upload Extension，App 内不可得 |
| 多路音频同时采集 | ✅ 自动混音 | ✅ 自动混音 | — | 麦克风 + 扬声器 可同时开，内核按采样点加和 |

运行期可用 `ITGCapturer::IsSupported(CaptureFeature)` 查询，不支持的能力返回 `false`、
调用返回负值，不再"静默失败"：

```cpp
capturer->IsSupported(TGCapture::CaptureFeature::Screen);      // 屏幕
capturer->IsSupported(TGCapture::CaptureFeature::Window);      // 窗口
capturer->IsSupported(TGCapture::CaptureFeature::Microphone);  // 麦克风
capturer->IsSupported(TGCapture::CaptureFeature::Speaker);     // 扬声器/系统内录
```

### 3.2 Windows

| 项 | 实现 |
| - | - |
| 屏幕 | `VideoCapMode::DXGI`：D3D11 + `IDXGIOutputDuplication`（GPU 给帧，不被遮挡，支持多显示器 `CreateScreenCapture(index)`）；失败自动回落 `GDI`（BitBlt） |
| 窗口 | `CreateWindowCapture(hwnd, WGC)` 采整窗（含标题栏）；`WGC_No_TBar` 只取客户区。被遮挡/最小化时用 `PrintWindow(PW_RENDERFULLCONTENT)` 兜底 |
| 麦克风 | WASAPI `eCapture` 默认端点；可用 `WinAudioCapture::SetDeviceId()` 指定设备 |
| 扬声器 | WASAPI `eRender` + `AUDCLNT_STREAMFLAGS_LOOPBACK` |
| 设备枚举 | `platform::ListAudioDevices(bool capture)` 返回端点 id 列表 |
| 帧率稳定 | 桌面无变化时 DXGI 不给新帧，内核复用上一帧，直播不会断流 |

### 3.3 Android

屏幕/系统内录依赖 `MediaProjection`，**只能由 App 侧弹系统授权框获取**（SDK 不能自行申请）：

```java
// 1) 申请授权
startActivityForResult(mgr.createScreenCaptureIntent(), REQ);
// 2) 授权通过后交给内核
capturer.SetPrivateData("android.JavaVM", vm);
capturer.SetPrivateData("android.MediaProjection", projection);
// 3) 之后照常调用
capturer.EnableMicrophoneCapture();     // AudioRecord(MIC)
capturer.EnableSpeakerCapture();        // AudioPlaybackCapture，需 Android 10+
capturer.CreateScreenCapture(0, TGCapture::AndroidSurface);
```

Java 桥接实现在 `ide/android/app/src/main/java/com/tencent/tgcapture/TGCaptureSystemCapture.java`
（VirtualDisplay + ImageReader(YUV_420_888) + AudioRecord / AudioPlaybackCapture），
C++ 侧 `src/platform/android/AndroidScreenCapture.cpp`、`AndroidAudioCapture.cpp` 负责转 NV12 与出帧。
若上层绑定了编码器输入 Surface（`SetEncoderSurface`），则走零拷贝、C++ 不搬运像素。

### 3.4 iOS

| 项 | 说明 |
| - | - |
| 屏幕 | `ReplayKit` 的 `RPScreenRecorder.startCaptureWithHandler`（iOS 11+），CVPixelBuffer **零拷贝**直送 VideoToolbox |
| 麦克风 | `AVAudioSession(playAndRecord)` + `AVAudioEngine` inputNode tap，转交错 S16 |
| 窗口 / 系统内录 | **不支持**（系统限制）：全系统录屏需 Broadcast Upload Extension，由宿主 App 提供 |

> iOS 的 `RPScreenRecorder` 只能采集**本 App** 画面，这是系统限制而非实现缺失；
> 要录全系统请在宿主 App 里加 Broadcast Upload Extension，再通过
> `PushExternalVideoFrame` / `PushExternalAudioFrame` 把帧送进内核。

## 4. 软编 / 硬编

* **软编兜底**：`libopenh264`（H.264）+ `libfdk_aac`（AAC）—— 任何环境可用。
* **硬编直连（零拷贝）**：编码器按 `vCodec` 字符串选择，帧携带硬件句柄时直接构造硬件 `AVFrame`，
  CPU 不参与像素搬运：

| 平台 | 每帧流转 | 编码器名 |
| - | - | - |
| Android | 采集 Surface → 编码器输入 Surface（MediaCodec 内部直连） | `h264_mediacodec` |
| iOS | `CVPixelBuffer` → VideoToolbox（`AV_PIX_FMT_VIDEOTOOLBOX`） | `h264_videotoolbox` |
| Windows | 采集 D3D11 纹理 → NVENC / MF / AMF（`AV_PIX_FMT_D3D11` + `hw_device_ctx`） | `h264_nvenc` / `h264_mf` / `h264_amf` |

硬编不可用时（无 `hw_frames_ctx` 或上下文创建失败）自动回退系统内存路径，
因此功能不受影响，只是不再零拷贝。

## 5. 构建

依赖 `TGCapture3rd`（由 `FfmpegCompileShell` 产出：ffmpeg / openssl / srt / fdk-aac / openh264 / libyuv / zlib）。
命令行与 IDE **共用同一套 CMake 工程**，产物完全一致。

### 5.1 命令行构建

```powershell
# Windows（VS2022 生成器，与依赖库 ABI 一致）
python scripts/build.py --platform windows --abi x64

# Android
python scripts/build.py --platform android --abi arm64-v8a --ndk $ANDROID_NDK_HOME

# iOS（macOS 上执行）
python scripts/build.py --platform ios

# 常用开关：--clean / --build-type Debug|Release / --no-demo / --3rd <依赖目录>
```

### 5.2 用 IDE 打开（VS2022 / Android Studio / Xcode）

一条命令生成工程并自动打开（**只生成，不编译**，编译交给 IDE）：

```powershell
python scripts/build.py --platform windows --ide   # VS2022
python scripts/build.py --platform android --ide   # Android Studio
python scripts/build.py --platform ios    --ide    # Xcode（macOS）
```

| 平台 | IDE | 打开的工程 | 说明 |
| - | - | - | - |
| Windows | Visual Studio 2022 | `build/windows-x64/TGCapture.sln` | 生成器 `Visual Studio 17 2022`；只保留 `Debug` / `Release`；`tg_capture_demo` 已设为启动工程 |
| Android | Android Studio | `ide/android`（gradle 宿主工程） | `File > Open…` 选该目录；`app/build.gradle` 用 `externalNativeBuild` 驱动根目录 `CMakeLists.txt` |
| iOS | Xcode | `build/ios-fat/TGCapture.xcodeproj` | `--ide` 自动切 `Xcode` 生成器；命令行构建仍用默认生成器 |

各平台要点：

* **Windows / VS2022**
  * 生成后双击 `build/windows-x64/TGCapture.sln` 即可（或再跑一次 `--ide`）。
  * ffmpeg 是 DLL：构建时自动把 `<3rd>/ffmpeg/windows/bin/*.dll` 拷到 `tg_capture_demo.exe` 同目录，
    并写入调试工作目录与 `PATH`（`LocalDebuggerEnvironment`）——**F5 直接跑，不用手工配 PATH**。
  * 只需重新生成工程（不编译）：`cmake -S . -B build/windows-x64 -G "Visual Studio 17 2022" -A x64`。
* **Android / Android Studio**
  * `ide/android` 是本仓库自带的最小 gradle 宿主工程（Native C++ 形态），不加 Java/Kotlin 代码，
    只产出 `libtgcapture.a`；首次打开按向导配置 SDK / Gradle 即可。
  * 换 ABI 时**同时**改 `app/build.gradle` 的 `abiFilters` 与 `-DTG_ABI`。
  * NDK 版本改 `ndkVersion`（默认 `27.0.12077973`，与 FfmpegCompileShell 默认拉取的 r27c 对齐）。
* **iOS / Xcode**
  * 选 `tgcapture` target 编译即可；iOS 只产出静态库（demo 仅 Windows）。

### 5.3 依赖目录怎么指定

优先级从高到低，三选一：

| 方式 | 用法 | 适用 |
| - | - | - |
| 命令行 | `--3rd <目录>`（等价 `cmake -DTG_3RD_ROOT=<目录>`） | 临时换一份产物对比 |
| 环境变量 | `setx TG_3RD_ROOT D:\work\TGCapture3rd` | **CI / IDE 推荐**，不落进缓存 |
| 自动探测 | 什么都不给 | 本地默认：取工程同级的 `TGCapture3rd`（旧名 `3rd-rel-Party` 仍兼容） |

默认值只在 `CMakeLists.txt` 里定义一处，`scripts/build.py` 不重复猜，避免两处漂移。
configure 时会打印 `TGCapture: 3rd=<路径> (来源: …)`，一眼看出用的是哪一份产物；
缓存里残留的旧路径若已不存在，会自动回退探测并告警（不会直接 FATAL）。

> Windows 产物是 `flat` 布局（`<库>/windows/{include,lib}`，**无 abi 层**），
> `cmake/Deps.cmake` 会按 abi → lib/abi → flat 依次探测，不必再改脚本。

### 5.4 产物

| 平台 | 产物 |
| - | - |
| Windows | `build/windows-x64/Release/tgcapture.lib`、`tg_capture_demo.exe`（+ 同目录 ffmpeg DLL） |
| Android | `libtgcapture.a`（由 gradle 输出到 `ide/android/app/build/…`） |
| iOS | `build/ios-fat/libtgcapture.a` |

## 6. demo

```powershell
# 构建时 ffmpeg DLL 已自动拷到 exe 同目录，直接跑即可；
# 从其它目录运行时才需要补 PATH：$env:PATH = "<3rd>\ffmpeg\windows\bin;" + $env:PATH
cd build\windows-x64\Release
.\tg_capture_demo.exe --out out.flv --seconds 10 --w 1280 --h 720 --fps 30
.\tg_capture_demo.exe --out rtmp://127.0.0.1/live/stream
.\tg_capture_demo.exe --out out.ts            # 后缀 .ts 自动用 mpegts
.\tg_capture_demo.exe --vcodec h264_nvenc     # 硬编（需支持 D3D11 的采集输入）
.\tg_capture_demo.exe --fake-video --no-audio # 人造帧，用于链路自检

# ---- 采集相关 ----
.\tg_capture_demo.exe --list-audio                      # 列出系统麦克风/扬声器设备
.\tg_capture_demo.exe --mode gdi                        # 强制 GDI 屏幕采集（默认 dxgi）
.\tg_capture_demo.exe --monitor 1                       # 采集第 2 台显示器
.\tg_capture_demo.exe --window D07CA                    # 采集指定窗口（16 进制 HWND）
.\tg_capture_demo.exe --window D07CA --no-border        # 只取客户区（去标题栏边框）
.\tg_capture_demo.exe --speaker                         # 麦克风 + 扬声器 同时采集（自动混音）
```

参数：`--out --vcodec --acodec --w --h --fps --bitrate --seconds --no-audio --fake-video`
`--speaker --monitor --mode --window --no-border --list-audio`。

`--no-audio` 走**纯视频**链路（`AudioEncoderParams::enabled=false`）：不创建音频编码器、
封装器也不建音频流；不给则默认音视频双流。
`--window` 的句柄可用 PowerShell 取（注意 demo 按 **16 进制**解析）：

```powershell
(Get-Process | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1).MainWindowHandle
'{0:X}' -f 853962     # -> D07CA
```

## 7. 已验证（Windows）

* 构建：MSVC + CMake，静态库 `tgcapture.lib` 与 demo 均编译通过
* 运行：屏幕采集 → openh264 + fdk-aac → flv 文件，5 秒采集编码 106 帧，线程正常退出，
  输出 `FLV` 合法文件（约 270 KB）
* 兼容层：`ESMediaX::ESCapturer::IESMediaXCapturer` 直接调用新内核，编译期 `static_assert` 校验通过
* 依赖定位：对 `TGCapture3rd`（flat 布局，无 abi 层）自动探测成功，构建零警告
* 纯视频：`--no-audio` 输出只有 1 条 `h264` 流；默认输出 `h264 + aac` 两条流
* IDE：VS2022 打开 `build/windows-x64/TGCapture.sln`（启动工程 = `tg_capture_demo`，
  `Debug`/`Release` 两配置，调试环境变量已带 ffmpeg DLL 目录）；
  ffmpeg DLL 自动拷贝到 exe 同目录，脱离 PATH 也能运行
* **屏幕（DXGI）**：`2160x3840` 显示器，3 秒 97 帧（≈30fps 满帧，桌面静止时复用上一帧不断流）
* **屏幕（GDI 兜底）**：同一显示器 BitBlt 路径正常出帧
* **窗口**：整窗 `2012x2032`、客户区 `1990x2022`（`--no-border` 正确去掉边框），稳定出帧
* **麦克风 + 扬声器**：WASAPI 两路同时就绪，内核启用 2 路混音（`启用 2 路音频混音`），输出正常
* **设备枚举**：`--list-audio` 正确列出麦克风与扬声器端点（含默认标记）

## 8. 已知限制 / 后续项

* Windows 采集目前输出的是 NV12 **系统内存**帧；DXGI / WGC 直出 D3D11 纹理的零拷贝链路
  已按 `HwKind::D3D11Texture` 预留接口，接入后 NVENC/MF/AMF 即为全链路零拷贝
* Android / iOS 的采集实现**尚未在各自平台编译验证**（本机只有 Windows 工具链）：
  `src/platform/android/*`、`src/platform/ios/*` 与 Java 桥接 `TGCaptureSystemCapture.java`
  需在有 SDK/NDK/Xcode 的环境上跑一次；iOS 的 ReplayKit 只录本 App（见 3.4）
* `ide/android` 是最小 gradle 宿主工程，**尚未在装有 Android SDK/Gradle 的环境实测**
  （本机只有 Windows 工具链）；Android 侧以命令行 `build.py --platform android` 为准，
  用 AS 打开后若报 Gradle/AGP 版本问题，按向导调整 `ide/android/build.gradle` 的插件版本即可
* 暂停静帧、水印、本地文件采集、多路混流为接口占位（返回 0 或告警），后续按原工程补齐
* `GetStatsInfo` 当前返回基础 JSON（码率/队列/编码器统计）
