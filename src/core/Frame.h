// 帧/包统一表示：以 shared_ptr 管理生命周期，避免裸指针与手工引用计数
#ifndef TGCAPTURE_CORE_FRAME_H
#define TGCAPTURE_CORE_FRAME_H

#include <cstdint>
#include <memory>
#include <vector>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
}

namespace TGCapture::core
{

enum class MediaKind
{
    Video,
    Audio,
    Packet,
};

// 带内控制消息：与数据走同一条通道、严格保序（例如分辨率切换必须排在旧分辨率的最后一帧之后）。
// 控制帧不携带媒体数据，队列满时也不会被丢弃（见 Bus 的 Queued 连线）。
enum class ControlKind : uint8_t
{
    None = 0,        // 普通数据帧
    FormatChanged,   // 格式变化（分辨率 / 帧率 / 码率），controlData 指向新格式描述
    Flush,           // 丢弃该点之前尚未处理的积压
    EOS,             // 流结束
};

// 零拷贝硬件载体类型
enum class HwKind
{
    None = 0,
    CVPixelBuffer,   // iOS: CVPixelBufferRef
    D3D11Texture,    // Windows: ID3D11Texture2D*
    AndroidSurface,  // Android: 编码器输入 Surface（帧由 Surface 内部流转）
};

struct Frame
{
    MediaKind kind = MediaKind::Video;

    // 时间基：统一以采集单调时钟的微秒为准
    int64_t ptsUs = AV_NOPTS_VALUE;
    // PTS 已由生产者（外部注入方）确定，下游不要再拿音频主时钟/采集时钟改写它
    bool ptsLocked = false;
    int64_t captureUs = AV_NOPTS_VALUE;   // 采集时刻（用于音画同步与统计）

    // ---- 视频（系统内存） ----
    int width = 0;
    int height = 0;
    AVPixelFormat pixFmt = AV_PIX_FMT_NONE;
    std::shared_ptr<uint8_t> planes[4];  // 带数组删除器；planes[1] 等可为同一 buffer 的别名
    int strides[4] = {0, 0, 0, 0};

    // ---- 视频（硬件直连，零 CPU 拷贝） ----
    HwKind hwKind = HwKind::None;
    void* hwHandle = nullptr;             // 不拥有所有权，由生产者保证生命周期
    std::shared_ptr<void> hwOwner;        // 可选：持有底层对象（如 CVPixelBuffer）

    // ---- 音频 ----
    int sampleRate = 0;
    int channels = 0;
    int nbSamples = 0;
    AVSampleFormat sampleFmt = AV_SAMPLE_FMT_NONE;
    bool planar = false;
    // 统一用 shared_ptr<uint8_t> + 数组删除器，避免 shared_ptr<T[]> 与 shared_ptr<T> 混用
    std::shared_ptr<uint8_t> audioData;
    size_t audioBytes = 0;

    // ---- 编码后（kind == Packet）----
    std::shared_ptr<AVPacket> packet;
    bool keyFrame = false;
    MediaKind streamKind = MediaKind::Video;   // 该包属于视频流还是音频流，封装节点据此分流

    // ---- 元信息 ----
    int rotation = 0;
    uint32_t sourceId = 0;
    bool endOfStream = false;

    // ---- 带内控制（control != None 时本帧只是控制消息）----
    ControlKind control = ControlKind::None;
    std::shared_ptr<const void> controlData;   // 控制消息的载荷，类型由 control 约定

    [[nodiscard]] bool IsControl() const noexcept { return control != ControlKind::None; }
};

using FramePtr = std::shared_ptr<Frame>;

inline FramePtr MakeVideoFrame() { return std::make_shared<Frame>(); }

// 构造控制帧。kind 表示它属于哪条流（视频 / 音频 / 编码包），便于节点按流分发
inline FramePtr MakeControlFrame(ControlKind control, MediaKind kind,
                                 std::shared_ptr<const void> data = {})
{
    auto f = std::make_shared<Frame>();
    f->kind = kind;
    f->streamKind = kind == MediaKind::Audio ? MediaKind::Audio : MediaKind::Video;
    f->control = control;
    f->controlData = std::move(data);
    f->endOfStream = control == ControlKind::EOS;
    return f;
}

// 用 shared_ptr 托管的 AVFrame（RAII，杜绝 av_frame_free 漏调用）
using AVFramePtr = std::shared_ptr<AVFrame>;

inline AVFramePtr MakeAVFrame()
{
    return AVFramePtr(av_frame_alloc(), [](AVFrame* f) { av_frame_free(&f); });
}

// 用 shared_ptr 托管的 AVPacket
using AVPacketPtr = std::shared_ptr<AVPacket>;

inline AVPacketPtr MakeAVPacket()
{
    return AVPacketPtr(av_packet_alloc(), [](AVPacket* p) { av_packet_free(&p); });
}

}  // namespace TGCapture::core

#endif  // TGCAPTURE_CORE_FRAME_H
