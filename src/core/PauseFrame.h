// 暂停静帧：一次性把图片解码并转成 NV12，MuteVideo 期间用它的像素覆盖当前帧。
//
// 对齐 MediaX ESGeneratePicture::SetPausePic 的策略 —— 在视频帧注入阶段替换 PCM-plane，
// 保留原帧的 PTS/时间戳，让编码/封装/音画同步链路完全无感。
//
// 支持 PNG / JPG / BMP（走 ffmpeg av_read_frame + sws_scale 一次性解码到目标分辨率）。
#ifndef TGCAPTURE_CORE_PAUSE_FRAME_H
#define TGCAPTURE_CORE_PAUSE_FRAME_H

#include <memory>
#include <string>

#include "core/Frame.h"

namespace TGCapture::core
{

class PauseFrame
{
public:
    // 加载并缩放到 dstWidth×dstHeight 的 NV12。失败返回 false，内部状态复位。
    // rotate: 0 / 90 / 180 / 270（顺时针，与采集的 videoRotation 语义一致）
    bool Load(const std::string& path, int dstWidth, int dstHeight, int rotate);

    // 清空缓存（SetPauseInfo 传入空路径时调用）
    void Clear();

    [[nodiscard]] bool Valid() const noexcept { return m_frame != nullptr; }
    [[nodiscard]] int Width() const noexcept { return m_width; }
    [[nodiscard]] int Height() const noexcept { return m_height; }

    // 用缓存的 NV12 像素覆盖 target 的 planes[0]/planes[1]（必须预先对齐分辨率）。
    // 不改 ptsUs / captureUs / sourceId / rotation —— 保持原帧时间戳语义。
    bool ApplyTo(Frame& target) const;

private:
    FramePtr m_frame;   // 预解码的 NV12 帧（持有 planes/strides 的 shared_ptr）
    int m_width = 0;
    int m_height = 0;
};

}  // namespace TGCapture::core

#endif  // TGCAPTURE_CORE_PAUSE_FRAME_H
