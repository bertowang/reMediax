// 画面归一化的 CPU 实现：libyuv
//
// 管线：源格式 → I420 → [裁剪] → [翻转] → [旋转] → [缩放] → 目标格式
// 之所以先统一到 I420：libyuv 的 Mirror / Rotate / Scale 家族在 I420 上最全，
// 而 NV12 只有转换接口。多两次转换的开销换来的是"任意变换组合都能表达"。
#include "core/VideoTransform.h"

#include <algorithm>
#include <cstring>

#include <libyuv.h>

#include "core/Logger.h"

namespace TGCapture::core
{
namespace
{

// I420 的一个视图：指针 + stride + 尺寸。指针可能指向外部帧，也可能指向自己分配的缓冲。
// libyuv 的目标平面参数是 uint8_t*，所以这里不用 const。
struct I420View
{
    uint8_t* y = nullptr;
    uint8_t* u = nullptr;
    uint8_t* v = nullptr;
    int strideY = 0;
    int strideU = 0;
    int strideV = 0;
    int width = 0;
    int height = 0;
};

// 中间缓冲：I420 三个平面放在同一块内存里，由 keepAlive 持有，避免变换过程中被释放。
// Allocate 是"按需扩容"语义：尺寸没变大就复用上一次的内存 —— 变换在每帧路径上，
// 每帧 4 次 new[]（1080p 约 12 MB）的分配与首次触碰缺页开销比像素拷贝还贵。
struct I420Buffer
{
    std::shared_ptr<uint8_t> data;
    size_t capacityBytes = 0;
    I420View view;

    bool Allocate(int width, int height)
    {
        const int w = std::max(2, width & ~1);
        const int h = std::max(2, height & ~1);
        const int strideY = w;
        const int strideUV = w / 2;
        const size_t total = static_cast<size_t>(strideY) * h +
                             static_cast<size_t>(strideUV) * (h / 2) * 2;
        // 只复用、不缩小：分辨率偶尔变小也保留已分配的大块，避免反复分配
        if (!data || total > capacityBytes) {
            data.reset(new uint8_t[total], std::default_delete<uint8_t[]>());
            capacityBytes = total;
        }
        uint8_t* base = data.get();
        view.y = base;
        view.strideY = strideY;
        view.u = base + static_cast<size_t>(strideY) * h;
        view.strideU = strideUV;
        view.v = view.u + static_cast<size_t>(strideUV) * (h / 2);
        view.strideV = strideUV;
        view.width = w;
        view.height = h;
        return true;
    }

    I420View ConstView() const
    {
        I420View out;
        out.y = view.y;
        out.u = view.u;
        out.v = view.v;
        out.strideY = view.strideY;
        out.strideU = view.strideU;
        out.strideV = view.strideV;
        out.width = view.width;
        out.height = view.height;
        return out;
    }
};

// 源帧 → I420（输入支持 NV12 / I420 / YUV420P / BGRA / RGBA / ARGB）
bool ToI420(const Frame& src, I420Buffer& buf, I420View& out)
{
    const int w = src.width & ~1;
    const int h = src.height & ~1;
    if (w <= 0 || h <= 0 || !src.planes[0]) {
        return false;
    }
    if (!buf.Allocate(w, h)) {
        return false;
    }

    const uint8_t* p0 = src.planes[0].get();
    const uint8_t* p1 = src.planes[1].get();
    const uint8_t* p2 = src.planes[2].get();
    const int s0 = src.strides[0] > 0 ? src.strides[0] : w;
    const int s1 = src.strides[1] > 0 ? src.strides[1] : w / 2;
    const int s2 = src.strides[2] > 0 ? src.strides[2] : w / 2;

    int ret = -1;
    switch (src.pixFmt) {
        case AV_PIX_FMT_NV12:
            ret = libyuv::NV12ToI420(p0, s0, p1, s1, buf.view.y, buf.view.strideY, buf.view.u,
                                     buf.view.strideU, buf.view.v, buf.view.strideV, w, h);
            break;
        case AV_PIX_FMT_YUV420P:   // 与 I420 等价（planar YUV 4:2:0）
            ret = libyuv::I420Copy(p0, s0, p1, s1, p2, s2, buf.view.y, buf.view.strideY,
                                   buf.view.u, buf.view.strideU, buf.view.v, buf.view.strideV, w, h);
            break;
        case AV_PIX_FMT_BGRA:
            ret = libyuv::BGRAToI420(p0, s0, buf.view.y, buf.view.strideY, buf.view.u,
                                     buf.view.strideU, buf.view.v, buf.view.strideV, w, h);
            break;
        case AV_PIX_FMT_RGBA:
        case AV_PIX_FMT_ARGB:
            ret = libyuv::ARGBToI420(p0, s0, buf.view.y, buf.view.strideY, buf.view.u,
                                     buf.view.strideU, buf.view.v, buf.view.strideV, w, h);
            break;
        default:
            return false;
    }
    if (ret != 0) {
        return false;
    }
    out = buf.ConstView();
    return true;
}

// 在 I420 视图上做裁剪（只挪指针、改尺寸，不拷贝像素）
I420View CropView(const I420View& in, int x, int y, int w, int h)
{
    I420View out = in;
    const int ox = std::max(0, (x & ~1));
    const int oy = std::max(0, (y & ~1));
    out.y = in.y + static_cast<size_t>(oy) * in.strideY + ox;
    out.u = in.u + static_cast<size_t>(oy / 2) * in.strideU + ox / 2;
    out.v = in.v + static_cast<size_t>(oy / 2) * in.strideV + ox / 2;
    out.width = w & ~1;
    out.height = h & ~1;
    return out;
}

libyuv::RotationMode ToRotationMode(int rotation)
{
    switch (rotation) {
        case 90:
            return libyuv::kRotate90;
        case 180:
            return libyuv::kRotate180;
        case 270:
            return libyuv::kRotate270;
        default:
            return libyuv::kRotate0;
    }
}

// 每帧变换要用的 4 块中间缓冲：源 I420、翻转级、旋转级、缩放级。
// thread_local：变换只在编码线程上跑，每个线程各自一套，既复用又不需要加锁。
struct TransformCache
{
    I420Buffer src;
    I420Buffer flip;
    I420Buffer rotate;
    I420Buffer scaled;
};

TransformCache& Cache()
{
    static thread_local TransformCache cache;
    return cache;
}

// 翻转 + 旋转：libyuv 的 I420Mirror 与 I420Rotate 无法一次表达组合，
// 所以按 flip → rotate 串联；每一级都写进"与当前源不同"的那块缓冲。
bool FlipAndRotate(const I420View& in, const VideoTransform& t, I420Buffer& bufA, I420Buffer& bufB,
                   I420View& out)
{
    I420View cur = in;

    // libyuv 没有独立的"只垂直翻转"接口，用镜像 + 旋转 180 组合出来：
    //   水平镜像           = I420Mirror
    //   垂直翻转           = I420Mirror + I420Rotate(180)
    //   水平 + 垂直        = I420Rotate(180)
    const bool both = t.flipHorizontal && t.flipVertical;
    if (t.flipHorizontal || t.flipVertical) {
        if (!bufA.Allocate(cur.width, cur.height)) {
            return false;
        }
        if (both) {
            if (libyuv::I420Rotate(cur.y, cur.strideY, cur.u, cur.strideU, cur.v, cur.strideV,
                                   bufA.view.y, bufA.view.strideY, bufA.view.u, bufA.view.strideU,
                                   bufA.view.v, bufA.view.strideV, cur.width, cur.height,
                                   libyuv::kRotate180) != 0) {
                return false;
            }
        } else if (libyuv::I420Mirror(cur.y, cur.strideY, cur.u, cur.strideU, cur.v, cur.strideV,
                                      bufA.view.y, bufA.view.strideY, bufA.view.u,
                                      bufA.view.strideU, bufA.view.v, bufA.view.strideV, cur.width,
                                      cur.height) != 0) {
            return false;
        }
        cur = bufA.ConstView();

        if (t.flipVertical && !both) {
            // 再转 180 才是"上下颠倒"。必须用另一块缓冲：libyuv 没有"原地旋转"的契约，
            // 之前把同一块 buffer 既当源又当目的，是在赌它的内部实现按对称下标读写。
            if (!bufB.Allocate(cur.width, cur.height)) {
                return false;
            }
            if (libyuv::I420Rotate(cur.y, cur.strideY, cur.u, cur.strideU, cur.v, cur.strideV,
                                   bufB.view.y, bufB.view.strideY, bufB.view.u, bufB.view.strideU,
                                   bufB.view.v, bufB.view.strideV, cur.width, cur.height,
                                   libyuv::kRotate180) != 0) {
                return false;
            }
            cur = bufB.ConstView();
        }
    }

    const int rot = t.NormalizedRotation();
    if (rot != 0) {
        const int w = (rot == 90 || rot == 270) ? cur.height : cur.width;
        const int h = (rot == 90 || rot == 270) ? cur.width : cur.height;
        // 同理：目的缓冲必须与 cur 所在的缓冲不同
        I420Buffer& dst = (cur.y == bufA.view.y) ? bufB : bufA;
        if (!dst.Allocate(w, h)) {
            return false;
        }
        if (libyuv::I420Rotate(cur.y, cur.strideY, cur.u, cur.strideU, cur.v, cur.strideV, dst.view.y,
                               dst.view.strideY, dst.view.u, dst.view.strideU, dst.view.v,
                               dst.view.strideV, cur.width, cur.height, ToRotationMode(rot)) != 0) {
            return false;
        }
        cur = dst.ConstView();
    }

    out = cur;
    return true;
}

}  // namespace

int VideoTransform::NormalizedRotation() const noexcept
{
    // 归一化到 0..359 再对齐到 90 的倍数
    int r = rotation % 360;
    if (r < 0) {
        r += 360;
    }
    if (r < 45) {
        return 0;
    }
    if (r < 135) {
        return 90;
    }
    if (r < 225) {
        return 180;
    }
    if (r < 315) {
        return 270;
    }
    return 0;
}

void VideoTransform::ResolveCrop(int srcWidth, int srcHeight, int& outX, int& outY, int& outW,
                                 int& outH) const noexcept
{
    if (cropWidth <= 0 || cropHeight <= 0 || srcWidth <= 0 || srcHeight <= 0) {
        outX = 0;
        outY = 0;
        outW = srcWidth & ~1;
        outH = srcHeight & ~1;
        return;
    }
    int x = std::max(0, cropX);
    int y = std::max(0, cropY);
    int w = cropWidth;
    int h = cropHeight;
    if (x + w > srcWidth) {
        w = srcWidth - x;
    }
    if (y + h > srcHeight) {
        h = srcHeight - y;
    }
    // 对齐到偶数，避免 UV 平面错位
    w &= ~1;
    h &= ~1;
    if (w <= 0 || h <= 0) {
        outX = 0;
        outY = 0;
        outW = srcWidth & ~1;
        outH = srcHeight & ~1;
        return;
    }
    outX = x;
    outY = y;
    outW = w;
    outH = h;
}

void VideoTransform::TransformedSize(int srcWidth, int srcHeight, int& outW, int& outH) const noexcept
{
    int x = 0, y = 0, w = 0, h = 0;
    ResolveCrop(srcWidth, srcHeight, x, y, w, h);
    const int rot = NormalizedRotation();
    if (rot == 90 || rot == 270) {
        outW = h;
        outH = w;
    } else {
        outW = w;
        outH = h;
    }
}

bool ApplyVideoTransform(const Frame& src, const VideoTransform& transform, int dstWidth,
                         int dstHeight, AVPixelFormat dstFmt, FramePtr& out)
{
    if (dstWidth <= 0 || dstHeight <= 0) {
        return false;
    }
    if (dstFmt != AV_PIX_FMT_NV12 && dstFmt != AV_PIX_FMT_YUV420P) {
        return false;
    }

    // 1) 源 → I420（中间缓冲走线程本地缓存，每帧只有最后的输出帧是新分配）
    TransformCache& cache = Cache();
    I420Buffer& srcBuf = cache.src;
    I420View cur;
    if (!ToI420(src, srcBuf, cur)) {
        TG_LOGW_THROTTLED(2000000, "VideoTransform: 不支持的源格式 %d，跳过归一化",
                          static_cast<int>(src.pixFmt));
        return false;
    }

    // 2) 裁剪
    int cx = 0, cy = 0, cw = 0, ch = 0;
    transform.ResolveCrop(src.width, src.height, cx, cy, cw, ch);
    cur = CropView(cur, cx, cy, cw, ch);

    // 3) 翻转 + 旋转
    if (!FlipAndRotate(cur, transform, cache.flip, cache.rotate, cur)) {
        return false;
    }

    // 4) 缩放
    I420Buffer& scaledBuf = cache.scaled;
    const int dstW = dstWidth & ~1;
    const int dstH = dstHeight & ~1;
    if (cur.width != dstW || cur.height != dstH) {
        if (!scaledBuf.Allocate(dstW, dstH)) {
            return false;
        }
        if (libyuv::I420Scale(cur.y, cur.strideY, cur.u, cur.strideU, cur.v, cur.strideV, cur.width,
                              cur.height, scaledBuf.view.y, scaledBuf.view.strideY,
                              scaledBuf.view.u, scaledBuf.view.strideU, scaledBuf.view.v,
                              scaledBuf.view.strideV, dstW, dstH, libyuv::kFilterBilinear) != 0) {
            return false;
        }
        cur = scaledBuf.ConstView();
    }

    // 5) 输出
    out = MakeVideoFrame();
    out->kind = MediaKind::Video;
    out->width = dstW;
    out->height = dstH;
    out->ptsUs = src.ptsUs;
    out->captureUs = src.captureUs;
    out->rotation = src.rotation;
    out->sourceId = src.sourceId;

    if (dstFmt == AV_PIX_FMT_NV12) {
        const int strideY = dstW;
        const int strideUV = dstW;
        const size_t ySize = static_cast<size_t>(strideY) * dstH;
        const size_t uvSize = static_cast<size_t>(strideUV) * (dstH / 2);
        auto buffer = std::shared_ptr<uint8_t>(new uint8_t[ySize + uvSize],
                                               std::default_delete<uint8_t[]>());
        uint8_t* dy = buffer.get();
        uint8_t* duv = dy + ySize;
        if (libyuv::I420ToNV12(cur.y, cur.strideY, cur.u, cur.strideU, cur.v, cur.strideV, dy,
                               strideY, duv, strideUV, dstW, dstH) != 0) {
            return false;
        }
        out->pixFmt = AV_PIX_FMT_NV12;
        out->planes[0] = buffer;
        out->strides[0] = strideY;
        out->planes[1] = std::shared_ptr<uint8_t>(buffer, duv);   // 别名：共用同一块 buffer
        out->strides[1] = strideUV;
        return true;
    }

    // I420 / YUV420P 输出：直接拷进三平面
    const int strideY = dstW;
    const int strideUV = dstW / 2;
    const size_t total = static_cast<size_t>(strideY) * dstH +
                         static_cast<size_t>(strideUV) * (dstH / 2) * 2;
    auto buffer = std::shared_ptr<uint8_t>(new uint8_t[total], std::default_delete<uint8_t[]>());
    uint8_t* base = buffer.get();
    uint8_t* dy = base;
    uint8_t* du = dy + static_cast<size_t>(strideY) * dstH;
    uint8_t* dv = du + static_cast<size_t>(strideUV) * (dstH / 2);
    if (libyuv::I420Copy(cur.y, cur.strideY, cur.u, cur.strideU, cur.v, cur.strideV, dy, strideY,
                         du, strideUV, dv, strideUV, dstW, dstH) != 0) {
        return false;
    }
    out->pixFmt = dstFmt;
    out->planes[0] = buffer;
    out->strides[0] = strideY;
    out->planes[1] = std::shared_ptr<uint8_t>(buffer, du);
    out->strides[1] = strideUV;
    out->planes[2] = std::shared_ptr<uint8_t>(buffer, dv);
    out->strides[2] = strideUV;
    return true;
}

}  // namespace TGCapture::core
