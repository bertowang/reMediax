// 暂停静帧：一次性把图片解码并转成 NV12，MuteVideo 期间用它的像素覆盖当前帧。
//
// 对齐 MediaX ESGeneratePicture::SetPausePic 的策略 —— 在视频帧注入阶段替换 PCM-plane，
// 保留原帧的 PTS/时间戳，让编码/封装/音画同步链路完全无感。
//
// 支持 PNG / JPG / BMP（走 ffmpeg av_read_frame + sws_scale 一次性解码到目标分辨率）。
// 解码后的原图常驻内存：帧尺寸变化（运行期切分辨率、采集尺寸与编码尺寸不同）时按新尺寸重新缩放，
// 不再读文件。
#ifndef TGCAPTURE_CORE_PAUSE_FRAME_H
#define TGCAPTURE_CORE_PAUSE_FRAME_H

#include <memory>
#include <mutex>
#include <string>

#include "core/Frame.h"

struct AVFrame;

namespace TGCapture::core
{

class PauseFrame
{
public:
    PauseFrame() = default;
    ~PauseFrame();
    PauseFrame(const PauseFrame&) = delete;
    PauseFrame& operator=(const PauseFrame&) = delete;

    // 解码图片并预缩放到 dstWidth×dstHeight 的 NV12。失败返回 false，内部状态复位。
    // rotate: 0 / 90 / 180 / 270（顺时针，与采集的 videoRotation 语义一致）
    bool Load(const std::string& path, int dstWidth, int dstHeight, int rotate);

    // 清空缓存（SetPauseInfo 传入空路径时调用）
    void Clear();

    [[nodiscard]] bool Valid() const;
    [[nodiscard]] int Width() const;
    [[nodiscard]] int Height() const;

    // 用缓存的 NV12 像素覆盖 target 的 planes[0]/planes[1]。
    // target 尺寸与缓存不同时先按 target 尺寸重新缩放（结果缓存，下一帧同尺寸直接用）。
    // 不改 ptsUs / captureUs / sourceId / rotation —— 保持原帧时间戳语义。
    bool ApplyTo(Frame& target);

private:
    // 由 m_src 缩放 + 旋转出 dstWidth×dstHeight 的 NV12，写入 m_frame
    bool RebuildLocked(int dstWidth, int dstHeight);
    void ClearLocked();

    mutable std::mutex m_mutex;   // Load / Clear 在接口线程，ApplyTo 在合成线程
    AVFrame* m_src = nullptr;     // 解码出的原图
    int m_rotate = 0;
    FramePtr m_frame;             // 当前尺寸的 NV12 缓存（持有 planes/strides 的 shared_ptr）
    int m_width = 0;
    int m_height = 0;
};

}  // namespace TGCapture::core

#endif  // TGCAPTURE_CORE_PAUSE_FRAME_H
