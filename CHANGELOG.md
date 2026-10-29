# Changelog

版本号只在 `CMakeLists.txt` 的 `project(VERSION)` 写一处，`ITGCapturerMgr::GetVersion()` 由它注入。

## Unreleased

### 文件音频源（B7）
- `AudioSourceKind::File`：`capture/FileAudioSource`，纯 ffmpeg 实现、三端共用。解封装 + 解码（AAC）+ 重采样到编码格式，
  每 10ms 一块按墙钟节拍送进混音器（时间戳按已送样本数推进，混音器测得漂移为 0；被挂起落后 200ms 以上重新对齐，不追赶）。
- `loop = true` 无缝循环（seek 回开头接着解码，不插静音）；`loop = false` 播完发 `EsCap_Notify_LocalFile_EOF`
  （content = id），之后该路输出静音，不触发断流看护。
- `AddAudioSource` 先同步探测文件：不存在 / 无音轨 → `InvalidArgument`，解码器缺失 → `NotSupported`；默认 id 为 `file` / `file.2`。
  任意状态可加删，推流中加入淡入 20ms，音量 / 电平 / 移除与其它源一致。
- 新增 `TypeOfLocalFileAudio = 30`（AudioCapSourceType）。
- 支持的容器取决于 ffmpeg 白名单：mov（mp4 / m4a / mov）、flv；当前开播 profile 未编 mpegts 解封装器，ts 文件不支持。
- Android：`addFileAudioSource` 可用，`NOTIFY_LOCAL_FILE_EOF`；demo 自动化 `tg_cmd audiofile:<路径>`，回归脚本 `--audio-file`。
- 命令行 demo：`--audio-file <秒>:<路径>`、`--audio-file-volume`、`--audio-file-no-loop`。

### 分辨率档位与运行期切换（B6）
- 档位表 `src/core/ResolutionPresets.h`：360p / 480p / 540p / 720p / 720p60 / 1080p / 1080p60（宽高 / 帧率 / 建议码率）。
  `ITGCapturerMgr::ApplyResolutionPreset(name, params, portrait)` 填 `VideoEncoderParams`，`GetResolutionPresets()` 列名字。
- `ITGCapturer::SetVideoResolution(w, h, bitrate)` / `SetResolutionPreset(name, portrait)`：任意状态可调。
  推流中由编码线程排空旧编码器 → 按新尺寸重开 → 首帧 IDR，新 SPS/PPS 以 `AV_PKT_DATA_NEW_EXTRADATA` 交给封装
  （flv 重发 AVC sequence header；ts 由 Muxer 更新 `codecpar->extradata`，之后每个 IDR 前插的都是新参数集）。
  音频不受影响，视频时间轴连续；生效后发 `EsCap_Notify_VideoMetaInfo_Changed`（`"宽x高"`，data = 码率）。
  硬编不接受新尺寸时回落软编（发 `Encoder_Fallback`），之后切到硬编能接受的尺寸会切回硬编；都打不开则回退原分辨率并报 `TGErr_VideoEncodeFailed`。
  GPU 硬件帧直通时返回 `TGErr_NotSupported`。
- 采集侧：`VideoSourceBase::Resize`；Android 屏幕采集用 `VirtualDisplay.resize + setSurface` 换新尺寸的 ImageReader（不重新授权）。
- 暂停图常驻解码后的原图，帧尺寸变化时自动重新缩放；水印位置按每帧的实际尺寸解析。
- `GetStatsInfo(videoInfo)` 新增 `width` / `height`（当前编码尺寸）。
- Android 绑定：`setVideoResolution` / `setResolutionPreset` / `getResolutionPresets` / `applyResolutionPreset`，`NOTIFY_VIDEO_META_CHANGED`。
- 依赖：Android ffmpeg 需要 `h264_metadata` / `hevc_metadata` bsf（宽高不是 16 的倍数时 `h264_mediacodec` 内部会插入），
  FfmpegCompileShell 已将其并入强制 bsf 列表。
- demo：命令行 `--switch-res <秒>:<档位|宽x高>`；Android demo 自动化 `tg_cmd res:<档位|宽x高>`，回归脚本 `--switch-res`。

### 流量统计：实时值 + 累计值
- 修复 `GetStatsInfo(bitrateInfo).bytes`：此前是 1s 码率窗口内的字节数（每秒清零），并不是注释里说的累计值，
  现在改为真正的累计载荷字节。`bitrate_bps` / `packets` 的含义不变。
- 修复实时码率在写出停滞后一直保持断流前的值：现在超过 2 个窗口（2s）没有新数据就归 0
  （`GetTrafficStats` 同样受益）。
- `bitrateInfo` 新增字段：
  - 实时：`video_bps` / `audio_bps` / `io_bps`；
  - 累计：`video_bytes` / `audio_bytes` / `io_bytes`；
  - 其他：`avg_bps` / `duration_ms`。
  其中 `io_*` 是封装后实际写进 AVIO 的量（含文件头尾）。实测录 flv / ts 时，`io_bytes` 与最终文件大小逐字节一致。
- 新增 `src/core/TrafficMeter.h`（单写者、读端无锁），以及 `tests/test_traffic_meter.cpp`（8 个用例）。
- Windows 命令行 demo 在 `Stop` 之后再打印一次 `bitrateInfo`，用来核对累计值。

### 编码器探测与回落
- 新增 `ITGCapturerMgr::IsEncoderAvailable(name)` / `GetAvailableEncoders(video)`（`src/codec/EncoderProbe.*`）：
  用最小参数真正 `avcodec_open2` 一次判定可用性，进程内缓存。Java 侧对应
  `TGCapturer.isEncoderAvailable` / `getAvailableEncoders`。
- `VideoEncoder::Open`：请求的硬编本机打不开时回落 `libopenh264`（此前直接 `TGErr_OpenVideoEncoder`，
  模拟器 / 老机型上选 `h264_mediacodec` 就开不了播）；软编失败照常报错。
- `AudioEncoder::Open`：回落 `aac` 的条件扩大到"编进了但打不开"（此前只覆盖"未编进"）；输入格式错误仍不回落。
- 新增通知 `EsCap_Notify_Encoder_Fallback = 40`（`content = "video:A->B"` / `"audio:A->B"`，`data = 0/1`），
  Prepare 期间发出。
- Android：`h264_mediacodec` 不再走 `AV_PIX_FMT_MEDIACODEC` 硬件帧链路（采集侧没有 Surface 直连，
  此前 GPU 模式会白建一个 hwdevice 并刷"hw_frames_ctx 初始化失败"）；帧池初始化失败时同时撤掉 hwdevice。
- 新增 `tests/test_encoder_probe.cpp`（6 个用例）。全部 88 个用例已在 Android arm64 模拟器上通过。

### AudioEncoder 采样格式适配（R1）
- 回退到 ffmpeg 内置 `aac`（只收 FLTP）时，S16 → FLTP 转换已在 iOS 回归中补上（swresample）；
  本轮补齐回归测试与剩余缺口：
  - 新增 `tests/test_audio_encoder.cpp`：真实编码 + 解码，校验回退路径无 NaN、43/43 帧编出、
    解码后 RMS 与输入一致。在修复前的实现上可稳定复现（`Input contains NaN`、`send_frame -22`、1/43 帧）。
  - 编码器采样格式改为"支持 S16/S16P 就直接用、否则取首选格式再转换"，不再无脑取 `sample_fmts[0]`。
  - `Open` 校验输入必须是交错 S16（编码器内部按 int16 累积），其它格式直接失败（返回 -5，`extra=EINVAL`），
    不再开起来后静默产出噪声。
  - `swr_init` 失败时带出真实 `AVERROR`（此前统一记为 `EINVAL`）；`AVFrame` 声道布局改用
    `av_channel_layout_copy`，不再按值浅拷贝。

### 错误定义与异常处理（本轮）

#### 错误码
- `TGCaptureError` 重写：返回值与 `OnErrorCallback` 的 `what` 取同一套枚举，同一数值全局只有一个含义。
  分三段 —— `-1~-19` 调用级（状态/配置/打开失败）、`-20~-99` 调用级（参数与能力）、
  `-100` 以下运行期（只经回调，无返回值对应）。`extra` 统一为底层原始错误码（`AVERROR` / 平台码），
  仅供排障，不要用于分支判断。
- 补齐此前被吞掉的失败：Muxer 建流失败 / 写尾失败、编码器 `send_frame` / `receive_packet` /
  `av_frame_get_buffer` 失败、原生采集源 `Open` 失败、混流通道创建失败、工作线程启动失败、
  重复 `Prepare` / `Start`。
- 新增断流看护线程，覆盖"没有失败返回值、只表现为不再出数据"的采集类故障
  （`TGErr_VideoCaptureStalled` / `TGErr_AudioCaptureStalled`，恢复后发 `*_Recovered` 通知），
  顺带让 `SetAVTimeout` 的 `audioTimeoutMs` 真正生效（此前只存不用）。
- `OnStopCallback(err)` 带出本轮最后一次**运行期**错误码（此前恒为 0）。调用级误用
  （`InvalidState` / `InvalidArgument` / `NotConfigured` / `NotSupported`，已同步返回给调用方）不计入。

#### 失败后的状态与资源
- `Prepare` 中途失败现在会完整回滚（统一走 `TeardownLocked`）。此前只改状态就返回，后果是双重的：
  `Stop()` 在 `Idle` 态直接返回 → 资源无法释放；编码器 `Open()` 的 `if (m_opened) return 0`
  → 下一轮静默复用残留上下文，**第二次 Prepare 返回成功但新参数不生效**。
- `Start` 失败时逆序停掉本轮已启动的工作线程（此前只退状态，muxer / 编码线程继续空转）。
- `VideoEncoder::Open` 的 `avcodec_open2` 失败改走 `Close()`：此前只放 codec ctx，
  硬件设备 / 帧池 / staging 纹理全残留，每失败一次泄漏一整套。
- `AudioEncoder` 的 PCM 缓冲在失败分支也会消费，并加积压上限 + `Close()` 清空
  （此前编码持续失败时以约 176 KB/s 单向增长且永不释放）。
- `avformat_network_init` 收敛为进程级一次（此前每次 `Open` 含失败重试都 +1 而永不 -1）。
- `BuildAVFrame` 中 `av_hwframe_get_buffer` 失败改为丢帧返回。此前会落穿到泛型硬件分支，
  把 D3D11 纹理写进 `data[3]`（D3D11 约定是 `data[0]`），等于把格式错误的硬件帧交给编码器。

#### 线程安全
- 采集回调栅栏：`StopCapture()` 现在是"关闸 → join → 等在飞回调退出"。
  此前各平台的产帧线程多半不是我们起的（Android Java 线程 / iOS ReplayKit 队列），
  `Worker::Stop()` 的 join 不构成栅栏，而 `StopLocked` 随后就去拆编码器 → UAF 窗口。
  实现在基类 `Emit()` 统一拦截，平台代码零改动。
- `Stop()` 的锁外收尾（Drain + 发 Stopped）纳入"停机未完成"：
  此前这个空窗里别的线程会看到 `Idle` 以为已停干净，转头 `Destroy()` 就与回调并发。
  同一线程重入（宿主在 `OnStop` 里再调 `Stop`）直接放行，不会自等。
- 修复数据竞争：`StallWatch::m_timeoutUs`（`SetAVTimeout` 运行期写、看护线程读）、
  `m_syncDropped`、`AudioMixer::SetOutputFormat` / `ChannelCount`。
- `Muxer::Close()` 改为先清 `m_opened` 再释放上下文（原顺序与 `WriteOne` 的检查顺序相反）。
- `m_lifecycleMutex` 由 `recursive_mutex` 降为 `mutex`，`m_mixThread` 声明移到末尾
  （成员逆序析构，线程必须最先 join）。

#### 平台层失败路径（Windows 已实测，见下方"Windows 回归"；iOS / Android 仍需在目标平台复测）
- Windows WASAPI：`InitWASAPI` / `RetryOpen` 的每个失败分支统一走 `ReleaseEndpoint()`
  （此前每失败一轮泄漏一个 `IMMDevice` + 一个已 `Initialize` 的 `IAudioClient`，后者还占着端点）；
  `RPC_E_CHANGED_MODE` 不再记为"COM 初始化成功"（它不增加引用计数，误记会让
  `CoUninitialize` 把宿主线程的 apartment 计数打负）。
- Windows GDI：删除 DIB 前先把原位图选回 DC（位图处于选中状态时 `DeleteObject` 会失败，
  4K 下每个采集对象漏约 33 MB）。
- iOS 音频：失败分支回滚 audio session、摘 tap；重复 `Open` 先拆上一轮
  （此前覆盖 `__bridge_retained` 指针，旧 engine 泄漏且仍在推流，两路 PCM 同时灌入）。
- iOS 录屏：`Close()` 同步等待 `stopCaptureWithHandler`（带 3s 超时）；重复 `Open` 与
  `-2` 失败分支不再漏 retain。
- Android：`StopJava()` 经 `Close()` 暴露，停机路径真正切断 Java 侧回调源。

### 接口契约变更（本轮）

> 对外头文件 `include/TGCapture/` 的**签名**没有改动，宿主代码无需修改即可编译。
> 以下是行为语义与内部接口的变化。

1. **`ITGCapturer::Stop()` 的阻塞语义收紧**（行为变更，对外可见）
   并发调用时，后来者会等待正在进行的停机**完整结束**（含事件收尾）再返回，而不是立刻返回。
   动机：让"Stop() 返回"真正意味着可以安全析构。
   例外：在 `OnStopCallback` 里重入 `Stop()` 会直接返回，不会自等。

2. **`ITGCapturer::Prepare()` / `Start()` 同样会等待在途的 `Stop()` 收尾**（行为变更）
   保证宿主观察到的事件顺序不会出现新一轮的 `Prepared` 排在上一轮 `Stopped` 之前。

3. **`Prepare()` 失败后对象状态定义为"干净的 Idle"**（行为变更）
   失败时内部已完成回滚，宿主**不需要**再调 `Stop()`；调了也是无副作用的空操作
   （不会重复上报错误、不会触发 `OnStopCallback`）。此前失败后资源会残留且无法释放。

4. **内部接口 `capture::VideoSourceBase` / `AudioSourceBase` 新增两个虚函数**
   （`src/capture/Source.h`，不在对外头文件中）：
   - `virtual void Close()` —— 默认空实现。释放设备资源，须幂等且 `Close` 后能重新 `Open`。
     自定义采集源**不实现也能编译和运行**，只是设备资源会留到析构才放。
   - `StartCapture()` —— 与既有的 `StopCapture()` 对称，内部调用点已全部切换。
     直接调 `Start()` 仍可工作（栅栏默认开闸），但不建议。

### 架构
- **Bus 做实为三个平面**：数据（`Submit` / `Forward` / `Link`）、命令（`Send` / `Broadcast`：
  `Flush` / `RequestKeyFrame` / `Abort`）、事件（`EventHub`：异步分发线程 + 有界队列，Error 不丢）。
- 编码器不再持有 `Muxer*`，改经 `Bus::Link` 下游转发；帧新增 `streamKind` 区分视频包 / 音频包。
- 异常通知统一走事件平面；宿主回调（Prepared / Error / Started / Stopped / Notify）改为**解锁后**触发。
- 热路径缓存入口节点，去掉逐帧的字符串查找。

### 稳定性
- Muxer 运行期写包持续失败会上报 `TGErr_MuxWriteFailed`（此前只有 `Prepare` 阶段会报错）。
- Muxer I/O 增加中断回调与 `rw_timeout`（`MuxerParams::connectTimeoutMs` / `ioTimeoutMs`），
  网络阻塞不再让 `Stop()` 无限等待；写失败时 `Stop()` 先发 `Abort`。
- 音频封装队列由 `Block` 改为 `DropOldest`，消除"音频编码器堵在 Submit / Stop 先 join 它"的互等。
- 低延迟刷包改为 `AVFMT_FLAG_FLUSH_PACKETS`（原先设在 `priv_data` 上对 flv / mpegts 无效）。
- `BoundedQueue::DroppedCount` 改为原子量（统计线程并发读取）。

### 接口
- 新增 `enum TGCaptureError`（错误码表见 README 4.2）。
- `MuxerParams` 新增 `connectTimeoutMs` / `ioTimeoutMs` / `writeFailReportMs`。
- 推流地址在日志与统计里统一脱敏（`core::RedactUrl`）。
- 心跳日志增加 `bus[pend=… evDrop=…]`。

### iOS 回归修复（模拟器 x86_64 实测）
- `IOSScreenCapture::Loop` 退出时调 `stopCaptureWithHandler:nil`，ReplayKit 回调 nil block 必崩（SIGSEGV）；
  启停改为只由 `Open` / `Close` 管理。
- `AudioEncoder`：指定编码器不可用回退到内置 `aac`（FLTP）时仍按 S16 拷贝，编码全部失败（-103、文件写不出）；
  非 S16/S16P 编码器改经 swresample 转换。
- 音频运行中断流：wall-clock 时间轴从 0 重起导致 PTS 回退被编码器整段丢弃（视频断档约 2.5s）；
  现在 fallback 与恢复都从最后一个已输出 PTS 接续（`VideoOnlyTimeline::Rebase` / `VideoPtsGenerator::Rebase`）。
- `VideoPtsGenerator` Pause 档钉在 `maxLead` 后仍按最小步长爬升、越过上限；改为到达上限后丢帧。
- `GetStatsInfo`：`metaInfo` / `pktInfo` 的 muxer 段多包一层花括号（非法 JSON）；变长段改为按实际长度分配缓冲，避免截断。
- iOS 链接补 `Foundation` 框架（`IOSAudioCapture` 使用 `NSNotificationCenter`）。

### 工程
- 新增单元测试 `tg_tests`（Bus / Queue / 视频 PTS / 混音 / URL 脱敏）与 `scripts/accept_av.sh`、`scripts/ci.sh`。
- `build/` 不再纳入版本管理。

### Windows 回归（MSVC 2022 / x64 Release）
本轮平台层改动此前只做过语法检查，这次在 Windows 上完整编译并实测：

- **编译**：`tgcapture` / `tg_capture_demo` / `tg_tests` 零警告零错误。
- **单元测试**：100 个用例，连跑 3 次全绿（含 Windows 平台层 12 个；已在合入 iOS / Android 回归提交后复跑）。
- **新增 `tests/test_win_platform.cpp`**（仅 Windows 编译）：直接驱动真实 WASAPI / GDI / DXGI 源，
  在固定状态点（Close 之后）比较进程句柄 / GDI 对象 / 私有内存，覆盖
  Open/Close 循环、同对象 Close→Open、非法设备 id 回落默认、loopback、非法窗口句柄、
  宿主 STA 线程上 Open/Close 不打乱 COM 计数、自有 COM 配平。
  **做过变异验证**：把 `RPC_E_CHANGED_MODE` 记账改回旧逻辑，`WasapiDoesNotUnbalanceHostCom` 立即失败。
- **GDI 修复的实际影响比描述的小**（实测）：未选回就 `DeleteObject` 确实失败、DIB 留着，
  但随后 `DeleteDC` 会把它一并释放；本代码 `ReleaseGdi` 总是紧接着删 DC，所以旧代码**并不会**
  每轮漏 33 MB。修复仍然正确（顺序符合 GDI 规定，且对"DC 复用"的改法是必要的前提），
  但不是一个可观测的泄漏。
- **生命周期**（真实 DXGI 屏幕 + WASAPI 麦克风）：
  `Prepare 失败 → 重配 → 原地 Prepare 成功`（新增 demo 开关 `--fail-first`）、
  `Stop → Prepare` 重开 4~6 轮、GDI 屏幕 / 窗口采集重开、GPU 零拷贝（nvenc）失败回滚 + 重开、
  Prepare/Stop 两线程交错 15s：均无死锁、无崩溃，每轮 Stop 后句柄数恒定（demo 新增 `RES` 快照行）。
- **媒体链路**：flip both 与 rotate 180 的包大小序列逐包一致；音视频 PTS 严格单调；
  推流中途被对端 RST → 3s 后上报 `TGErr_MuxWriteFailed(extra=-10054)`，`Stop` 正常返回且 `OnStop` 带 `-101`；
  连接被拒 → `TGErr_OpenMuxer(extra=-138)`，日志里推流地址已脱敏；`--log-dir` 落盘与 `--no-print-log` 生效。

本轮 Windows 回归发现两处问题：
- **`VideoPtsGenerator` Pause 档上限失效**：Windows 上 5/5 稳定复现（`AudioStallKeepsMonotonicAndBoundedLead`）。
  与上方"iOS 回归修复"中的同一条是同一个缺陷，代码修复以那次提交为准；Windows 上复验通过。
- **`test_error_codes` 写死 `/tmp`**：Windows 上解析为 `<当前盘>:\tmp`（通常不存在），
  9 个本该成功的 `Prepare` 全部 `TGErr_OpenMuxer`，掩盖了真正要测的逻辑。改用 `temp_directory_path()`。

### 已知限制
- 写失败后不自动重连（FFmpeg I/O 错误为粘性），需宿主 `Stop()` 后重新 `Prepare()`。
- 以上改动在 Linux、iOS（模拟器 x86_64 全链路回归 + 真机 arm64 编译）与 Windows（见上方"Windows 回归"）验证，
  Android 待复测。
- iOS 麦克风采集与 ReplayKit 出帧未在真机实测：模拟器命令行进程中 `AURemoteIO` 初始化会 RPC 超时 abort（系统限制），
  ReplayKit 无授权不出帧（仅验证了 Prepare / Stop / 重新 Prepare 不崩、断流看护上报 -104）。
- 音频运行中断流时，视频在 `maxLead`（500ms）到 `audioTimeoutMs` 之间按设计丢帧，产物中会有
  `audioTimeoutMs - 500ms` 的视频空档；需要更短空档可调小 `SetAVTimeout` 的 `audioTimeoutMs`。
- 部分平台的音频采集源在 `Open` 失败后仍会注入静音帧，混流照常出帧，断流看护探测不到。
  这种"有声道但全静音"只能靠日志排查，无法自动上报。
- Windows 下 `WinAudioCapture::RetryOpen`（默认设备热切换）未自动化覆盖：需要真实插拔 / 切换默认设备，
  只能人工验证。
