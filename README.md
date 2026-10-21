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
| 软编 + 硬编直连 | 见第 3 节 |
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
│   ├── platform/windows|android|ios/
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

## 3. 软编 / 硬编

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

## 4. 构建

依赖 `TGCapture3rd`（由 `FfmpegCompileShell` 产出：ffmpeg / openssl / srt / fdk-aac / openh264 / libyuv / zlib）。
命令行与 IDE **共用同一套 CMake 工程**，产物完全一致。

### 4.1 命令行构建

```powershell
# Windows（VS2022 生成器，与依赖库 ABI 一致）
python scripts/build.py --platform windows --abi x64

# Android
python scripts/build.py --platform android --abi arm64-v8a --ndk $ANDROID_NDK_HOME

# iOS（macOS 上执行）
python scripts/build.py --platform ios

# 常用开关：--clean / --build-type Debug|Release / --no-demo / --3rd <依赖目录>
```

### 4.2 用 IDE 打开（VS2022 / Android Studio / Xcode）

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

### 4.3 依赖目录怎么指定

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

### 4.4 产物

| 平台 | 产物 |
| - | - |
| Windows | `build/windows-x64/Release/tgcapture.lib`、`tg_capture_demo.exe`（+ 同目录 ffmpeg DLL） |
| Android | `libtgcapture.a`（由 gradle 输出到 `ide/android/app/build/…`） |
| iOS | `build/ios-fat/libtgcapture.a` |

## 5. demo

```powershell
# 构建时 ffmpeg DLL 已自动拷到 exe 同目录，直接跑即可；
# 从其它目录运行时才需要补 PATH：$env:PATH = "<3rd>\ffmpeg\windows\bin;" + $env:PATH
cd build\windows-x64\Release
.\tg_capture_demo.exe --out out.flv --seconds 10 --w 1280 --h 720 --fps 30
.\tg_capture_demo.exe --out rtmp://127.0.0.1/live/stream
.\tg_capture_demo.exe --out out.ts            # 后缀 .ts 自动用 mpegts
.\tg_capture_demo.exe --vcodec h264_nvenc     # 硬编（需支持 D3D11 的采集输入）
.\tg_capture_demo.exe --fake-video --no-audio # 人造帧，用于链路自检
```

参数：`--out --vcodec --acodec --w --h --fps --bitrate --seconds --no-audio --fake-video`。

`--no-audio` 走**纯视频**链路（`AudioEncoderParams::enabled=false`）：不创建音频编码器、
封装器也不建音频流；不给则默认音视频双流。

## 6. 已验证（Windows）

* 构建：MSVC + CMake，静态库 `tgcapture.lib` 与 demo 均编译通过
* 运行：屏幕采集 → openh264 + fdk-aac → flv 文件，5 秒采集编码 106 帧，线程正常退出，
  输出 `FLV` 合法文件（约 270 KB）
* 兼容层：`ESMediaX::ESCapturer::IESMediaXCapturer` 直接调用新内核，编译期 `static_assert` 校验通过
* 依赖定位：对 `TGCapture3rd`（flat 布局，无 abi 层）自动探测成功，构建零警告
* 纯视频：`--no-audio` 输出只有 1 条 `h264` 流；默认输出 `h264 + aac` 两条流
* IDE：VS2022 打开 `build/windows-x64/TGCapture.sln`（启动工程 = `tg_capture_demo`，
  `Debug`/`Release` 两配置，调试环境变量已带 ffmpeg DLL 目录）；
  ffmpeg DLL 自动拷贝到 exe 同目录，脱离 PATH 也能运行

## 7. 已知限制 / 后续项

* Windows 采集当前为 GDI 路径（稳定兜底）；DXGI Desktop Duplication / WGC 直出 D3D11 纹理的
  零拷贝链路已按 `HwKind::D3D11Texture` 预留接口，接入后 NVENC/MF/AMF 即为全链路零拷贝
* Android / iOS 仅提供注入式采集源（Surface / CVPixelBuffer），需在各自平台编译验证
* `ide/android` 是最小 gradle 宿主工程，**尚未在装有 Android SDK/Gradle 的环境实测**
  （本机只有 Windows 工具链）；Android 侧以命令行 `build.py --platform android` 为准，
  用 AS 打开后若报 Gradle/AGP 版本问题，按向导调整 `ide/android/build.gradle` 的插件版本即可
* 暂停静帧、水印、本地文件采集、多路混流为接口占位（返回 0 或告警），后续按原工程补齐
* `GetStatsInfo` 当前返回基础 JSON（码率/队列/编码器统计）
