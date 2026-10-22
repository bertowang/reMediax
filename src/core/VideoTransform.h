// 统一画面变换：裁剪 → 翻转 → 旋转 → 缩放 → 像素格式
//
// 之前这些能力散落在各平台实现里（Windows/Android/iOS 各自调 libyuv），
// 旋转 / 翻转 / 裁剪 / DPI 更是无处安放，外部注入帧还会绕过全部平台实现直接进总线。
// 这里收敛成一个集中层：所有视频帧在进入编码器之前都过一遍，
// 保证"各种可能的画面来源"得到一致的归一化结果。
//
// 语义：变换在**源帧坐标系**上描述，顺序固定为
//   crop(裁剪) → flip(翻转) → rotate(旋转) → scale(缩放到编码分辨率) → convert(转格式)
// 90/270 度旋转会交换宽高。
#ifndef TGCAPTURE_CORE_VIDEO_TRANSFORM_H
#define TGCAPTURE_CORE_VIDEO_TRANSFORM_H

#include <cstdint>

extern "C"
{
#include <libavutil/pixfmt.h>
}

#include "core/Frame.h"

namespace TGCapture::core
{

struct VideoTransform
{
    // 顺时针旋转角度，只接受 0 / 90 / 180 / 270，其它值按 0 处理
    int rotation = 0;
    bool flipHorizontal = false;   // 水平镜像（左右翻转）
    bool flipVertical = false;     // 垂直翻转（上下颠倒）

    // 裁剪矩形，源帧坐标系；宽高 <= 0 表示不裁剪取整帧
    int cropX = 0;
    int cropY = 0;
    int cropWidth = 0;
    int cropHeight = 0;

    [[nodiscard]] bool IsIdentity() const noexcept
    {
        return NormalizedRotation() == 0 && !flipHorizontal && !flipVertical &&
               cropWidth <= 0 && cropHeight <= 0;
    }

    // 把 rotation 收敛到 0/90/180/270
    [[nodiscard]] int NormalizedRotation() const noexcept;

    // 归一化裁剪矩形：夹到源尺寸内，并对齐到偶数（YUV420 的 UV 是 2x2 下采样）
    void ResolveCrop(int srcWidth, int srcHeight, int& outX, int& outY, int& outW,
                     int& outH) const noexcept;

    // 应用 crop + rotate 之后的尺寸（90/270 会交换宽高）
    void TransformedSize(int srcWidth, int srcHeight, int& outW, int& outH) const noexcept;
};

// CPU 归一化（libyuv 实现）。
//
// src 必须是系统内存帧，支持 NV12 / YUV420P / BGRA / RGBA / ARGB 输入。
// dstFmt 目前支持 NV12 / YUV420P。
// 成功返回 true 并把结果写入 out（planes[0]/[1] 共享同一块 buffer，strides 已填好）。
// 返回 false 表示无法处理，调用方应退回 sws_scale 或丢弃该帧。
bool ApplyVideoTransform(const Frame& src, const VideoTransform& transform, int dstWidth,
                         int dstHeight, AVPixelFormat dstFmt, FramePtr& out);

}  // namespace TGCapture::core

#endif  // TGCAPTURE_CORE_VIDEO_TRANSFORM_H
