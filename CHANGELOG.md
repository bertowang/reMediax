# Changelog

版本号只在 `CMakeLists.txt` 的 `project(VERSION)` 写一处，`ITGCapturerMgr::GetVersion()` 由它注入。

## Unreleased

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

### 工程
- 新增单元测试 `tg_tests`（Bus / Queue / 视频 PTS / 混音 / URL 脱敏）与 `scripts/accept_av.sh`、`scripts/ci.sh`。
- `build/` 不再纳入版本管理。

### 已知限制
- 写失败后不自动重连（FFmpeg I/O 错误为粘性），需宿主 `Stop()` 后重新 `Prepare()`。
- 以上改动只在 Linux 上构建与回归，Windows 待复测。
