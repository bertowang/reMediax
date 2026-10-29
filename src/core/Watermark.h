// 水印/Logo overlay：预解码 PNG/JPG → NV12 Y + UV + alpha 三份缓存，
// 对 CPU 侧 NV12 帧做覆盖区域的像素级融合。
//
// 对齐 MediaX ESVPicFilter::AddOrRmLogo 的职责切分（ADD / UPDATE / RM 多 index 并存）；
// 本工程不走 filter graph，用 libyuv/手写循环实现 NV12 blend：
//   Y_out  = Y_src * (1-α) + Y_logo * α
//   UV_out = UV_src * (1-α) + UV_logo * α   （UV 用 2x2 块对应的平均 alpha）
//
// 不支持硬件帧直通路径下的水印（会打一行限频 Warn 跳过）。
// 若宿主需要 GPU 路径上的水印，可后续加 D3D11 compute shader / Metal performBlit 分支。
#ifndef TGCAPTURE_CORE_WATERMARK_H
#define TGCAPTURE_CORE_WATERMARK_H

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "TGCapture/TGCapturerInterface.h"
#include "core/Frame.h"

namespace TGCapture::core
{

struct WatermarkTexture
{
    uint32_t index = 0;
    int width = 0;
    int height = 0;
    int offsetX = 0;        // 按 Apply 时画布尺寸解析出的坐标（仅日志用）
    int offsetY = 0;
    // 定位参数原样保存：BlendOnto 按每帧的实际尺寸重新解析，画布尺寸变化（运行期切分辨率）后位置仍正确
    int32_t position = 0;
    int32_t rawX = 0;
    int32_t rawY = 0;
    float opacity = 1.0f;
    std::shared_ptr<uint8_t> yPlane;    // (width * height)
    std::shared_ptr<uint8_t> uvPlane;   // (width * height / 2) —— NV12 布局
    std::shared_ptr<uint8_t> alpha;     // (width * height)
};

class Watermark
{
public:
    // ADD / UPDATE / RM：对齐 MediaX WatermarkParams::type
    int Apply(const WatermarkParams& params, int canvasWidth, int canvasHeight);

    void Clear();

    // 对 NV12 CPU 帧叠加所有 index 的水印；frame 的 planes 必须可写。
    // 位置按 frame 的宽高解析（右下 / 底中等相对位置随画布尺寸变化）。
    // 水印 vector 内部已按 index 稳定排序，图层顺序 = 加入顺序。
    void BlendOnto(Frame& nv12) const;

    [[nodiscard]] bool Empty() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_layers.empty();
    }

private:
    static int ResolvePosition(int32_t pos, int32_t xy_x, int32_t xy_y,
                               int canvasW, int canvasH, int logoW, int logoH,
                               int& outX, int& outY);
    // 用 ffmpeg 把图片解到 BGRA → 本函数手动 BGRA→NV12 + 独立 alpha
    static bool DecodeImage(const std::string& url, int canvasW, int canvasH,
                            WatermarkTexture& out, float opacity, int32_t position,
                            int32_t offX, int32_t offY);

    mutable std::mutex m_mutex;
    std::unordered_map<uint32_t, WatermarkTexture> m_layers;
};

}  // namespace TGCapture::core

#endif  // TGCAPTURE_CORE_WATERMARK_H
