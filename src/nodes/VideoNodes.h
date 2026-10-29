// 视频链路的总线节点
//
//   vsrc.main         VideoSourceNode      —— 主画面源（原生采集 / 外部注入 / 本地文件），preview 组，受控输出
//   vsrc.ovl.<n>      VideoSourceNode      —— 视频叠加层的源（本地文件），preview 组，接合成器的输入 n（≥ 1）
//   video_compositor  VideoCompositorNode  —— 图层合成，preview 组。输入 0 = 主画面，输入 n = 视频叠加层；
//                                             图层：主画面 → 暂停图（MuteVideo 时替换）→ 叠加层（按 zOrder）→ 水印 → 时间水印
//                                             out0 = 推流画面（已提交的场景），out1 = 预览画面（暂存的场景）
//   video_sync        VideoSyncNode        —— 编码前的背压与音画同步（视频 PTS 由音频主时钟反推），output 组
//   preview_sink      PreviewSinkNode      —— 把预览画面交给宿主回调（Latest 连线：只送最新一帧，宿主慢了就跳帧）
//
//   vsrc.main ──> video_compositor ──out0──> video_sync ──> video_encoder ──> muxer
//                                  └─out1──> preview_sink（StartPreview 时接上）
//
// 推流这一路都是 Direct 连线：在产帧线程里同步完成，与节点化之前的 OnVideoFrame 行为一致。
// video_sync 属于 output 组：只开预览不开播时它不运行，合成后的画面到它这里就停下。
#ifndef TGCAPTURE_NODES_VIDEO_NODES_H
#define TGCAPTURE_NODES_VIDEO_NODES_H

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "capture/Source.h"
#include "core/Bus.h"
#include "core/Overlay.h"

namespace TGCapture::core
{
class PauseFrame;
class Watermark;
class TimestampOverlay;
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

    [[nodiscard]] core::NodeGroup Group() const override { return core::NodeGroup::Preview; }
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
    [[nodiscard]] core::NodeGroup Group() const override { return core::NodeGroup::Preview; }

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
    // 时间水印（最上层，水印之上）：仅对 NV12 CPU 帧生效。时间取帧的采集时刻换算成的墙钟
    void SetTimestampLayer(std::shared_ptr<core::TimestampOverlay> timestamp);

    // 叠加层场景：program 画在推流画面上，preview 画在预览画面上（两者相同时预览直接共享推流画面的像素）。
    // 两个指针一起原子替换，下一帧起生效。视频叠加层的帧从对应输入端口进来，只保留最新一帧
    void SetOverlayScenes(core::OverlayScenePtr program, core::OverlayScenePtr preview);
    [[nodiscard]] core::OverlayScenePtr ProgramScene() const;
    [[nodiscard]] core::OverlayScenePtr PreviewScene() const;
    // 某个叠加输入口已收到的帧数（测试 / Dump 用）
    [[nodiscard]] uint64_t OverlayFramesOf(size_t port) const;

    void OnInputDetached(size_t port, const std::string& from) override;

    // 主画面每到一帧回调一次（产帧线程内，参数为墙钟 us）：断流看护判定"采集端还在不在产出"
    void SetMainActivityObserver(std::function<void(int64_t)> fn) { m_onMain = std::move(fn); }

    [[nodiscard]] uint64_t Composed() const noexcept { return m_composed.load(); }
    [[nodiscard]] std::string DumpState() const override;

private:
    // 视频叠加层的输入：最新一帧
    struct OverlayInput
    {
        core::FramePtr frame;
        uint64_t frames = 0;
    };
    // 缩放后的叠加层像素缓存：同一素材（同一张图 / 同一帧视频）+ 同一目标尺寸只缩放一次，
    // 推流与预览两个场景里同一层共用
    struct ScaledLayer
    {
        std::shared_ptr<const void> source;   // 素材本身（持有引用，地址不会被复用）
        int width = 0;
        int height = 0;
        std::vector<uint8_t> y, uv, a;
        uint64_t lastUse = 0;
    };

    // 把场景画到 frame 上（frame 必须是可写的 NV12 CPU 帧）
    void ComposeOverlays(core::Frame& frame, const core::OverlayScene& scene);
    const ScaledLayer* Scaled(const core::OverlayLayer& layer, int w, int h);

    std::atomic_bool m_running{false};
    std::atomic_bool m_muted{false};
    mutable std::mutex m_layerMutex;   // 保护图层指针的替换（帧路径上拷一份 shared_ptr 再用）
    std::shared_ptr<core::PauseFrame> m_pause;
    std::shared_ptr<core::Watermark> m_watermark;
    std::shared_ptr<core::TimestampOverlay> m_timestamp;
    core::OverlayScenePtr m_programScene;
    core::OverlayScenePtr m_previewScene;
    mutable std::mutex m_inputMutex;   // 保护叠加输入的最新帧
    std::unordered_map<size_t, OverlayInput> m_inputs;
    std::mutex m_renderMutex;          // 保护缩放缓存（主画面可能来自原生采集与外部注入两个线程）
    std::unordered_map<std::string, ScaledLayer> m_scaled;
    uint64_t m_renderTick = 0;
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
    // 受控输出：预览期间停播时采集线程仍在送帧，停止后必须保证没有线程还在往编码器里送
    [[nodiscard]] bool GatedOutput() const override { return true; }

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

// ------------------------------------------------------------------ preview_sink
class PreviewSinkNode : public core::BusNode
{
public:
    using Callback = std::function<void(const PreviewFrame&)>;

    PreviewSinkNode() : core::BusNode("preview_sink") {}

    bool Submit(core::FramePtr frame) override;
    [[nodiscard]] size_t Pending() const override { return 0; }

    // 宿主回调（在 Latest 连线的投递线程上调用；回调里不得调用 StopPreview / Destroy）
    void SetCallback(Callback cb);

    [[nodiscard]] uint64_t Delivered() const noexcept { return m_delivered.load(); }
    [[nodiscard]] std::string DumpState() const override;

private:
    mutable std::mutex m_mutex;
    Callback m_callback;
    std::atomic<uint64_t> m_delivered{0};
};

}  // namespace TGCapture::nodes

#endif  // TGCAPTURE_NODES_VIDEO_NODES_H
