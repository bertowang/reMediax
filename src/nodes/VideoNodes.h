// 视频链路的总线节点
//
//   vsrc.main         VideoSourceNode      —— 主画面源（原生采集 / 外部注入），capture 组，受控输出
//   video_compositor  VideoCompositorNode  —— 图层合成，capture 组。输入 0 = 主画面（其余输入留给叠加层）；
//                                             图层：主画面 → 暂停图（MuteVideo 时替换）→ 水印 → …
//                                             out0 = 推流画面，out1 = 预览画面
//   video_sync        VideoSyncNode        —— 编码前的背压与音画同步（视频 PTS 由音频主时钟反推），output 组
//
//   vsrc.main ──> video_compositor ──out0──> video_sync ──> video_encoder ──> muxer
//                                  └─out1──> (preview)
//
// 三段都是 Direct 连线：在产帧线程里同步完成，与节点化之前的 OnVideoFrame 行为一致。
// video_sync 属于 output 组：只开预览不开播时它不运行，合成后的画面到它这里就停下。
#ifndef TGCAPTURE_NODES_VIDEO_NODES_H
#define TGCAPTURE_NODES_VIDEO_NODES_H

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "capture/Source.h"
#include "core/Bus.h"

namespace TGCapture::core
{
class PauseFrame;
class Watermark;
class VideoPtsGenerator;
class VideoOnlyTimeline;
class AudioClock;
class AVSync;
}  // namespace TGCapture::core

namespace TGCapture::nodes
{

// ------------------------------------------------------------------ vsrc.*
class VideoSourceNode : public core::BusNode
{
public:
    explicit VideoSourceNode(std::string name);
    ~VideoSourceNode() override;

    [[nodiscard]] size_t InputPorts() const override { return 0; }
    bool Submit(core::FramePtr) override { return false; }
    [[nodiscard]] size_t Pending() const override { return 0; }

    [[nodiscard]] core::NodeGroup Group() const override { return core::NodeGroup::Capture; }
    [[nodiscard]] bool GatedOutput() const override { return true; }

    // 绑定 / 换绑原生采集源（不拥有）。只能在未运行时调用；传空即解绑
    void SetSource(capture::VideoSourceBase* source);
    [[nodiscard]] capture::VideoSourceBase* Source() const noexcept { return m_source; }

    // 原生源的 Open 带 GPU 链路协商，由门面在 Prepare 里直接调用；这里只管启停关
    bool NodeStart() override;
    void NodeStop() override;
    void NodeClose() override;

    // 外部注入（PushExternalVideoFrame）。节点停止（关闸）后返回 false
    bool Push(core::FramePtr frame);

    [[nodiscard]] uint64_t Emitted() const noexcept { return m_emitted.load(); }
    [[nodiscard]] std::string DumpState() const override;

private:
    capture::VideoSourceBase* m_source = nullptr;
    std::atomic<uint64_t> m_emitted{0};
};

// ------------------------------------------------------------------ video_compositor
class VideoCompositorNode : public core::BusNode
{
public:
    static constexpr size_t kMainInput = 0;
    static constexpr size_t kProgramOutput = 0;   // 推流画面（只含已提交的图层）
    static constexpr size_t kPreviewOutput = 1;   // 预览画面

    VideoCompositorNode();

    [[nodiscard]] size_t InputPorts() const override { return core::kDynamicPorts; }
    [[nodiscard]] size_t OutputPorts() const override { return 2; }
    [[nodiscard]] core::NodeGroup Group() const override { return core::NodeGroup::Capture; }

    bool Submit(core::FramePtr frame) override { return SubmitInput(kMainInput, std::move(frame)); }
    bool SubmitInput(size_t port, core::FramePtr frame) override;
    [[nodiscard]] size_t Pending() const override { return 0; }

    bool NodeStart() override;
    void NodeStop() override;

    // ---- 图层 ----
    // 暂停图层：MuteVideo 期间用它替换主画面像素（没设图片则"冻结"——原样输出最新一帧）
    void SetPauseLayer(std::shared_ptr<core::PauseFrame> pause);
    void SetMuted(bool muted) noexcept { m_muted.store(muted); }
    [[nodiscard]] bool IsMuted() const noexcept { return m_muted.load(); }
    // 水印图层：仅对 NV12 CPU 帧生效
    void SetWatermarkLayer(std::shared_ptr<core::Watermark> watermark);

    // 主画面每到一帧回调一次（产帧线程内，参数为墙钟 us）：断流看护判定"采集端还在不在产出"
    void SetMainActivityObserver(std::function<void(int64_t)> fn) { m_onMain = std::move(fn); }

    [[nodiscard]] uint64_t Composed() const noexcept { return m_composed.load(); }
    [[nodiscard]] std::string DumpState() const override;

private:
    std::atomic_bool m_running{false};
    std::atomic_bool m_muted{false};
    mutable std::mutex m_layerMutex;   // 保护图层指针的替换（帧路径上拷一份 shared_ptr 再用）
    std::shared_ptr<core::PauseFrame> m_pause;
    std::shared_ptr<core::Watermark> m_watermark;
    std::function<void(int64_t)> m_onMain;
    std::atomic<uint64_t> m_composed{0};
};

// ------------------------------------------------------------------ video_sync
class VideoSyncNode : public core::BusNode
{
public:
    struct Config
    {
        bool audioEnabled = true;
        int64_t frameIntervalUs = 33333;
        size_t maxEncoderPending = 8;   // 编码器积压达到它就丢帧（保证直播实时性）
    };

    VideoSyncNode();

    [[nodiscard]] core::NodeGroup Group() const override { return core::NodeGroup::Output; }

    bool Submit(core::FramePtr frame) override;
    [[nodiscard]] size_t Pending() const override { return 0; }

    bool NodeStart() override;
    void NodeStop() override;

    void SetConfig(const Config& config);
    // 音频主时钟判活 / 首帧等待的时长（SetAVTimeout 运行期可改）
    void SetAudioTimeoutMs(int64_t ms) noexcept { m_audioTimeoutMs.store(ms); }
    // 每轮 Prepare 复位：计数、首帧标志、时间轴接续点
    void Reset();

    [[nodiscard]] uint64_t SyncDropped() const noexcept { return m_syncDropped.load(); }
    [[nodiscard]] uint64_t BackpressureDropped() const noexcept { return m_bpDropped.load(); }
    [[nodiscard]] std::string DumpState() const override;

private:
    bool Deliver(core::FramePtr frame, bool externalTimeline);

    std::atomic_bool m_running{false};
    std::mutex m_mutex;   // 外灌线程与原生采集线程可能并发送帧：时间轴状态串行化
    Config m_config;
    std::atomic<int64_t> m_audioTimeoutMs{3000};
    std::atomic<uint64_t> m_syncDropped{0};
    std::atomic<uint64_t> m_bpDropped{0};
    std::atomic_bool m_firstFrameSent{false};
    // 音频主时钟首帧等待：音频未就绪时视频不发帧（避免就绪后 PTS 回退被整段丢弃）
    int64_t m_audioWaitStartUs = 0;
    // 最后一个已提交编码的视频帧（内部时间轴）：主时钟 ↔ wall-clock 切换时据此接续
    int64_t m_lastVideoPtsUs = -1;
    int64_t m_lastVideoCaptureUs = -1;
};

}  // namespace TGCapture::nodes

#endif  // TGCAPTURE_NODES_VIDEO_NODES_H
