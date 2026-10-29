// 视频叠加层的场景描述与像素工具（video_compositor 用）。
//
//   OverlayScene  一个完整、不可变的场景：已排好绘制顺序的图层列表。合成器持有两个场景指针 ——
//                 program（推流画面，只含已提交的）/ preview（预览画面，含暂存的修改）；
//                 提交 = 原子地换一个指针，帧路径上拷一份 shared_ptr 再用，所以同一帧里不会看到半个场景。
//   Nv12AImage    预解码的图片：NV12 + 独立 alpha（偶数宽高）
//   BlendNv12A    把一块 NV12（可带 alpha）按透明度混合到 NV12 帧的指定位置（越界部分裁掉）
#ifndef TGCAPTURE_CORE_OVERLAY_H
#define TGCAPTURE_CORE_OVERLAY_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "TGCapture/TGCapturerInterface.h"
#include "core/Frame.h"

namespace TGCapture::core
{

struct Nv12AImage
{
    int width = 0;
    int height = 0;
    std::vector<uint8_t> y;    // width * height
    std::vector<uint8_t> uv;   // width * height / 2（NV12 交错）
    std::vector<uint8_t> a;    // width * height；空 = 完全不透明
};

// 用 ffmpeg 解码图片文件（png 等）到 NV12A。失败返回 false，why 写原因
bool LoadImageNv12A(const std::string& path, Nv12AImage& out, std::string* why = nullptr);

// 一块待混合的 NV12 像素（a 可空 = 不透明）
struct Nv12AView
{
    const uint8_t* y = nullptr;
    int yStride = 0;
    const uint8_t* uv = nullptr;
    int uvStride = 0;
    const uint8_t* a = nullptr;
    int aStride = 0;
    int width = 0;
    int height = 0;
};

// 混合到 dst（NV12 CPU 帧，必须可写）的 (x, y)，x / y 按偶数对齐；opacity 0 ~ 255
void BlendNv12A(Frame& dst, int x, int y, const Nv12AView& src, int opacity);

// 叠加层在画布上的像素矩形（偶数对齐；w / h ≤ 0 表示不可见）
struct PixelRect
{
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};
PixelRect ResolveOverlayRect(const OverlayDesc& desc, int srcWidth, int srcHeight, int canvasWidth,
                             int canvasHeight);

struct OverlayLayer
{
    OverlayDesc desc;
    int srcWidth = 0;    // 素材原始尺寸（height = 0 时据此保持宽高比）
    int srcHeight = 0;
    std::shared_ptr<const Nv12AImage> image;   // Image
    size_t inputPort = 0;                      // VideoFile：合成器的输入端口（≥ 1）
};

struct OverlayScene
{
    std::vector<OverlayLayer> layers;   // 绘制顺序：下层在前（zOrder 升序，相同按 id）
    uint64_t version = 0;               // 每次换场景递增（日志 / Dump 用）
};
using OverlayScenePtr = std::shared_ptr<const OverlayScene>;

// 合法性检查（id / 几何 / 透明度 / 路径），不碰文件。失败返回原因
bool ValidateOverlayDesc(const OverlayDesc& desc, std::string* why);
// 两个描述画出来是否一样（不比较内容来源，内容由调用方比较）
bool SameOverlayDesc(const OverlayDesc& a, const OverlayDesc& b);

}  // namespace TGCapture::core

#endif  // TGCAPTURE_CORE_OVERLAY_H
