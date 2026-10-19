# TGCapture —— 开播内核（C++20）

用 C++20 重写的开播（采集 → 编码 → 封装 → 推流）内核，**对外接口保持不变**，
仍然基于 FFmpeg API 实现，支持 Windows / Android / iOS 三端，产物依赖 `3rd-rel-Party`。

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
├── cmake/Deps.cmake            3rd-rel-Party 依赖定位
├── CMakeLists.txt
└── scripts/build.py            python 构建脚本（windows/android/ios）
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

依赖 `3rd-rel-Party`（由 `FfmpegCompileShell` 产出：ffmpeg / openssl / srt / fdk-aac / openh264 / libyuv）。

```powershell
# Windows（默认 MSVC 生成器，与依赖库 ABI 一致）
python scripts/build.py --platform windows --abi x64

# Android
python scripts/build.py --platform android --abi arm64-v8a --ndk $ANDROID_NDK_HOME

# iOS（macOS 上执行）
python scripts/build.py --platform ios

# 其它：--clean / --build-type Debug|Release / --no-demo / --3rd <依赖根目录>
```

产物：`build/<platform>-<abi>/Release/tgcapture.lib`、`tg_capture_demo.exe`（Windows）。

## 5. demo

```powershell
$env:PATH = "..\3rd-rel-Party\ffmpeg\windows\x64\bin;" + $env:PATH
.\tg_capture_demo.exe --out out.flv --seconds 10 --w 1280 --h 720 --fps 30
.\tg_capture_demo.exe --out rtmp://127.0.0.1/live/stream
.\tg_capture_demo.exe --out out.ts            # 后缀 .ts 自动用 mpegts
.\tg_capture_demo.exe --vcodec h264_nvenc     # 硬编（需支持 D3D11 的采集输入）
.\tg_capture_demo.exe --fake-video --no-audio # 人造帧，用于链路自检
```

参数：`--out --vcodec --acodec --w --h --fps --bitrate --seconds --no-audio --fake-video`。

## 6. 已验证（Windows）

* 构建：MSVC + CMake，静态库 `tgcapture.lib` 与 demo 均编译通过
* 运行：屏幕采集 → openh264 + fdk-aac → flv 文件，5 秒采集编码 106 帧，线程正常退出，
  输出 `FLV` 合法文件（约 270 KB）
* 兼容层：`ESMediaX::ESCapturer::IESMediaXCapturer` 直接调用新内核，编译期 `static_assert` 校验通过

## 7. 已知限制 / 后续项

* Windows 采集当前为 GDI 路径（稳定兜底）；DXGI Desktop Duplication / WGC 直出 D3D11 纹理的
  零拷贝链路已按 `HwKind::D3D11Texture` 预留接口，接入后 NVENC/MF/AMF 即为全链路零拷贝
* Android / iOS 仅提供注入式采集源（Surface / CVPixelBuffer），需在各自平台编译验证
* 暂停静帧、水印、本地文件采集、多路混流为接口占位（返回 0 或告警），后续按原工程补齐
* `GetStatsInfo` 当前返回基础 JSON（码率/队列/编码器统计）
