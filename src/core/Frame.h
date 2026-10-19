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

    // ---- 编码后 ----
    std::shared_ptr<AVPacket> packet;
    bool keyFrame = false;

    // ---- 元信息 ----
    int rotation = 0;
    uint32_t sourceId = 0;
    bool endOfStream = false;
};

using FramePtr = std::shared_ptr<Frame>;

inline FramePtr MakeVideoFrame() { return std::make_shared<Frame>(); }

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
