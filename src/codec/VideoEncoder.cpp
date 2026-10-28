#include "codec/VideoEncoder.h"

#include "codec/EncoderProbe.h"

#include <algorithm>
#include <limits>

#include <cstdio>

#include "core/AvError.h"
#include "core/Logger.h"

#ifdef _WIN32
#include <d3d11.h>
#endif
#if defined(__APPLE__)
#include <CoreVideo/CoreVideo.h>
#endif

extern "C"
{
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#ifdef _WIN32
#include <libavutil/hwcontext_d3d11va.h>
#endif
}

namespace TGCapture::codec
{

namespace
{
// 编码器名 -> 期望的硬件输入格式（用于零拷贝判断）
AVPixelFormat HwInputFormat(const std::string& name)
{
    if (name == "h264_nvenc" || name == "h264_mf" || name == "h264_amf" || name == "hevc_nvenc") {
        return AV_PIX_FMT_D3D11;
    }
    if (name == "h264_videotoolbox") {
        return AV_PIX_FMT_VIDEOTOOLBOX;
    }
    // h264_mediacodec 不走硬件帧：Android 的采集源产出的都是系统内存 NV12（ImageReader 回读 / 外部注入），
    // 编码器 Surface 直连尚未实现。原先这里返回 AV_PIX_FMT_MEDIACODEC，GPU 模式下会白建一个
    // MEDIACODEC hwdevice，再在 hw_frames_ctx 上失败（mediacodec 不支持帧池）后回落 —— 结果一样是 CPU 帧，
    // 只多了一条误导性的 Warn。MediaCodec 吃 NV12 内存帧本身就是硬编，性能模式对它没有区别。
    return AV_PIX_FMT_NONE;
}

// 编码器是否接受 BGRA 硬件帧（决定 D3D11 帧池的 sw_format，避免多余的格式转换）
bool CodecAcceptsBgra(const std::string& name)
{
    return name == "h264_nvenc" || name == "hevc_nvenc";
}

// 让 AVFrame 持有一份 core::Frame 的强引用。
// 硬件直连时 data[3] 只是一个裸句柄，AVFrame 本身不拥有它：一旦 AVFrame 比 Frame 活得久
// （或将来把提交改成异步），底层 CVPixelBuffer / 纹理的 owner 会被提前释放。
// 用 av_buffer_create 挂一个自定义释放器，把持有关系交给 AVFrame 自己管。
bool AttachFrameOwner(AVFrame* avFrame, const core::FramePtr& frame)
{
    if (!avFrame || !frame) {
        return false;
    }
    auto* holder = new core::FramePtr(frame);
    AVBufferRef* ref = av_buffer_create(
        nullptr, 0,
        [](void* opaque, uint8_t*) { delete static_cast<core::FramePtr*>(opaque); }, holder, 0);
    if (!ref) {
        delete holder;
        return false;
    }
    avFrame->buf[0] = ref;
    return true;
}

AVHWDeviceType HwDeviceType(AVPixelFormat fmt)
{
    switch (fmt) {
        case AV_PIX_FMT_D3D11:
            return AV_HWDEVICE_TYPE_D3D11VA;
        case AV_PIX_FMT_VIDEOTOOLBOX:
            return AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
        case AV_PIX_FMT_MEDIACODEC:
            return AV_HWDEVICE_TYPE_MEDIACODEC;
        default:
            return AV_HWDEVICE_TYPE_NONE;
    }
}

#if defined(__APPLE__)
// CVPixelBuffer（ReplayKit / 外部注入）→ 系统内存帧。
// CPU 模式（软编 libopenh264，或 VideoToolbox 走系统内存输入）下编码器拿不到硬件句柄里的像素，
// 原先这类帧在下面"收到硬件帧但编码器为软件链路"分支被整帧丢弃 —— iOS 上表现为推流成功但没有画面。
// 这里锁住像素缓冲逐行拷出，交给通用的 sws 归一化（缩放 + 转编码器像素格式）。
core::FramePtr ReadBackCVPixelBuffer(const core::FramePtr& frame)
{
    auto pb = static_cast<CVPixelBufferRef>(frame->hwHandle);
    if (!pb) {
        return nullptr;
    }
    const OSType fourcc = CVPixelBufferGetPixelFormatType(pb);
    AVPixelFormat fmt = AV_PIX_FMT_NONE;
    if (fourcc == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange ||
        fourcc == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange) {
        fmt = AV_PIX_FMT_NV12;
    } else if (fourcc == kCVPixelFormatType_32BGRA) {
        fmt = AV_PIX_FMT_BGRA;
    } else {
        TG_LOGW_THROTTLED(5000000, "VideoEncoder: 不支持回读的 CVPixelBuffer 格式 0x%08x", static_cast<unsigned>(fourcc));
        return nullptr;
    }
    const int w = static_cast<int>(CVPixelBufferGetWidth(pb));
    const int h = static_cast<int>(CVPixelBufferGetHeight(pb));
    if (w <= 0 || h <= 0) {
        return nullptr;
    }
    if (CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) {
        return nullptr;
    }
    auto out = std::make_shared<core::Frame>(*frame);
    out->hwKind = core::HwKind::None;
    out->hwHandle = nullptr;
    out->hwOwner.reset();
    out->width = w;
    out->height = h;
    out->pixFmt = fmt;

    const bool planar = CVPixelBufferIsPlanar(pb);
    const size_t planeCount = planar ? CVPixelBufferGetPlaneCount(pb) : 1;
    // 单块分配、按紧凑 stride 存放，planes[i] 是同一 buffer 的别名
    size_t rowBytes[2] = {0, 0};
    size_t rows[2] = {0, 0};
    size_t total = 0;
    for (size_t p = 0; p < planeCount && p < 2; ++p) {
        const size_t pw = planar ? CVPixelBufferGetWidthOfPlane(pb, p) : static_cast<size_t>(w);
        rows[p] = planar ? CVPixelBufferGetHeightOfPlane(pb, p) : static_cast<size_t>(h);
        rowBytes[p] = (fmt == AV_PIX_FMT_BGRA) ? pw * 4 : (p == 0 ? pw : pw * 2);
        total += rowBytes[p] * rows[p];
    }
    std::shared_ptr<uint8_t> buffer(new uint8_t[total], std::default_delete<uint8_t[]>());
    size_t offset = 0;
    for (size_t p = 0; p < planeCount && p < 2; ++p) {
        const auto* src = static_cast<const uint8_t*>(planar ? CVPixelBufferGetBaseAddressOfPlane(pb, p)
                                                             : CVPixelBufferGetBaseAddress(pb));
        const size_t srcStride = planar ? CVPixelBufferGetBytesPerRowOfPlane(pb, p) : CVPixelBufferGetBytesPerRow(pb);
        uint8_t* dst = buffer.get() + offset;
        for (size_t y = 0; y < rows[p]; ++y) {
            memcpy(dst + y * rowBytes[p], src + y * srcStride, rowBytes[p]);
        }
        out->planes[p] = (p == 0) ? buffer : std::shared_ptr<uint8_t>(buffer, dst);
        out->strides[p] = static_cast<int>(rowBytes[p]);
        offset += rowBytes[p] * rows[p];
    }
    CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    return out;
}
#endif
}  // namespace

VideoEncoder::VideoEncoder(size_t queueCapacity)
    : core::BusNode("video_encoder"),
      core::Worker("video_encoder"),
      m_queue(std::make_unique<core::BoundedQueue<core::FramePtr>>(queueCapacity, core::DropPolicy::DropOldest))
{
}

VideoEncoder::~VideoEncoder()
{
    Stop();
    Close();
}

int VideoEncoder::Open(const VideoEncoderParams& params, const core::VideoMediaType& srcType)
{
    const int ret = OpenCodec(params, srcType);
    if (ret == 0) {
        return 0;
    }
    // 硬编能否打开取决于设备 / 驱动（模拟器、老机型、无对应显卡），属于运行环境问题，
    // 不该让开播直接失败：回落软编。软编失败是配置问题（名字写错 / 未编进），照常报错。
    const std::string requested = params.vCodec.empty() ? kSoftwareVideoEncoder : params.vCodec;
    if (!IsHardwareVideoEncoder(requested) || !avcodec_find_encoder_by_name(kSoftwareVideoEncoder)) {
        return ret;
    }
    TG_LOGW("VideoEncoder: 硬编 %s 在本机打不开（ret=%d averr=%d），回退 %s", requested.c_str(), ret,
            static_cast<int>(m_lastAvError.load()), kSoftwareVideoEncoder);
    Close();   // 失败路径可能残留 ctx / 硬件上下文
    VideoEncoderParams soft = params;
    soft.vCodec = kSoftwareVideoEncoder;
    return OpenCodec(soft, srcType);
}

int VideoEncoder::OpenCodec(const VideoEncoderParams& params, const core::VideoMediaType& srcType)
{
    if (m_opened.load()) {
        return 0;
    }
    // 重开：队列在 Close() 时被永久关闭，必须显式重开，否则这一轮的 Submit 全部失败
    m_queue->Reopen();
    m_encoded.store(0);
    m_dropped.store(0);
    // 运行期失败判定：连续失败 1s 上报，恢复观察期 200ms（编码是无缓冲的同步调用，
    // 不像 AVIO 那样需要等缓冲刷过几轮）
    m_encodeFail.Reset();
    m_encodeFail.Configure(1000000, 200000);
    m_lastAvError.store(0);
    m_srcType = srcType;
    m_codecName = params.vCodec.empty() ? "libopenh264" : params.vCodec;

    const AVCodec* codec = avcodec_find_encoder_by_name(m_codecName.c_str());
    if (!codec) {
        TG_LOGE("VideoEncoder: 找不到编码器 %s", m_codecName.c_str());
        m_lastAvError.store(AVERROR_ENCODER_NOT_FOUND);
        return -1;
    }

    m_ctx = avcodec_alloc_context3(codec);
    if (!m_ctx) {
        TG_LOGE("VideoEncoder: 分配编码器上下文失败 codec=%s", m_codecName.c_str());
        m_lastAvError.store(AVERROR(ENOMEM));
        return -2;
    }

    m_ctx->width = params.width > 0 ? params.width : srcType.Width();
    m_ctx->height = params.height > 0 ? params.height : srcType.Height();
    m_ctx->time_base = AVRational{1, 1000000};  // 微秒时间基，简化 PTS 换算
    m_ctx->framerate = srcType.FrameRate();
    m_ctx->gop_size = params.gop > 0 ? params.gop : 15;
    m_ctx->max_b_frames = 0;  // 直播低延迟（ffmpeg 6.x 字段名为 max_b_frames）
    m_ctx->bit_rate = params.vBitrate;
    m_ctx->rc_max_rate = static_cast<int64_t>(params.vBitrate * (params.vBitrateMaxFactor > 0 ? params.vBitrateMaxFactor : 1.0f));
    // rc_buffer_size 是 int，rc_max_rate 是 int64_t：夹到 int 上界再转，避免 C4244 静默截断
    m_ctx->rc_buffer_size = static_cast<int>(
        std::min<int64_t>(m_ctx->rc_max_rate * 2, std::numeric_limits<int>::max()));
    m_ctx->thread_count = params.threads > 0 ? params.threads : 4;
    m_ctx->pix_fmt = AV_PIX_FMT_NONE;

    // GPU 模式：采集帧留在显存，直接喂硬编（CPU 不做像素搬运）
    // CPU 模式：一律走系统内存帧（NV12），保留既有链路
    AVPixelFormat hwFmt = (m_perfMode == PerformanceMode::Gpu) ? HwInputFormat(m_codecName) : AV_PIX_FMT_NONE;
    if (hwFmt != AV_PIX_FMT_NONE && InitHardwareContext(hwFmt) && InitHardwareFrames(hwFmt)) {
        m_ctx->pix_fmt = hwFmt;
        m_useHwFrame = true;
        TG_LOGI("VideoEncoder: GPU 链路就绪（硬件帧直通，pix_fmt=%d）", static_cast<int>(hwFmt));
    } else {
        // hwdevice 建好了但帧池失败：设备上下文必须一并撤掉，否则软件帧编码器身上挂着一个
        // 用不上的 hw_device_ctx（部分编码器据此改走硬件输入分支，表现为 open 失败或丢帧）
        if (m_hwDeviceCtx) {
            av_buffer_unref(&m_ctx->hw_device_ctx);
            av_buffer_unref(&m_hwDeviceCtx);
#ifdef _WIN32
            m_d3dContext = nullptr;
#endif
        }
        // 软编或硬编降级：优先 NV12（硬编友好），否则 YUV420P
        m_ctx->pix_fmt = AV_PIX_FMT_NV12;
        if (!codec->pix_fmts || !avcodec_find_best_pix_fmt_of_list(codec->pix_fmts, AV_PIX_FMT_NV12, 0, nullptr)) {
            m_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
        }
        m_swPixFmt = m_ctx->pix_fmt;
    }

    // 低延迟/实时参数：不同编码器私有参数差别很大，无脑 set 会打一堆 "Unable to parse option value"。
    // 这里按编码器族做白名单：
    //   - libopenh264 / libx264        : preset + tune + rc 全套
    //   - *_nvenc                      : preset（p1） + rc（无 tune）
    //   - *_mf (Microsoft Media Foundation) : 完全不吃 x264 风格 opt，只走 bit_rate；
    //   - *_videotoolbox (Apple)       : 同上，只认 "realtime"/"allow_sw"（这里保守全跳过）
    //   - *_amf (AMD) / *_qsv (Intel)  : 有自己的 preset/profile，跳过 x264 风格 opt
    const bool isNvenc        = m_codecName.find("nvenc") != std::string::npos;
    const bool isMediaFound   = m_codecName.find("_mf") != std::string::npos;        // h264_mf / hevc_mf
    const bool isVideoToolbox = m_codecName.find("videotoolbox") != std::string::npos;
    const bool isAmf          = m_codecName.find("_amf") != std::string::npos;
    const bool isQsv          = m_codecName.find("_qsv") != std::string::npos;
    const bool isHwFamily     = isNvenc || isMediaFound || isVideoToolbox || isAmf || isQsv;

    if (!isMediaFound && !isVideoToolbox && !isAmf && !isQsv) {
        av_opt_set(m_ctx->priv_data, "preset", isNvenc ? "p1" : "veryfast", 0);
    }
    // tune 只对 x264 / openh264 一类生效，其他编码器一律跳过（nvenc 等私有参数单独设）
    if (!isHwFamily) {
        av_opt_set(m_ctx->priv_data, "tune", "zerolatency", 0);
    }
    if (!isMediaFound && !isVideoToolbox) {
        // mf/videotoolbox 的 bitrate 走 m_ctx->bit_rate；rc opt 它们不认
        av_opt_set(m_ctx->priv_data, "rc", "cbr", 0);
    }
    // x264-params 只能喂给 libx264；其他家喂了就是 warning，没副作用但很吵
    if (!params.vExtParams.empty() && m_codecName == "libx264") {
        av_opt_set(m_ctx->priv_data, "x264-params", params.vExtParams.c_str(), 0);
    }

    m_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    int ret = avcodec_open2(m_ctx, codec, nullptr);
    if (ret < 0) {
        TG_LOGE("VideoEncoder: avcodec_open2 失败 %d (%s) codec=%s", ret, core::AvErr(ret),
                m_codecName.c_str());
        m_lastAvError.store(ret);
        // 走 Close() 而不是只 free ctx：此前 InitHardwareContext / InitHardwareFrames 已经建起
        // m_hwDeviceCtx / m_hwFramesCtx（Windows 上还有 staging 纹理），只放 ctx 的话它们会残留，
        // 下一轮 Open 直接覆盖指针 —— 每失败一次泄漏一整套硬件帧池。
        Close();
        return -3;
    }

    m_dstType.SetWidth(m_ctx->width);
    m_dstType.SetHeight(m_ctx->height);
    m_dstType.SetPixelFormat(m_ctx->pix_fmt);
    m_dstType.SetFrameRate(m_ctx->framerate.num, m_ctx->framerate.den);

    m_opened.store(true);
    TG_LOGI("VideoEncoder: %s %dx%d fps=%.2f hw=%d",
            m_codecName.c_str(), m_ctx->width, m_ctx->height, m_dstType.FrameRateFloat(), m_useHwFrame ? 1 : 0);
    return 0;
}

bool VideoEncoder::InitHardwareContext(AVPixelFormat srcFmt)
{
    AVHWDeviceType type = HwDeviceType(srcFmt);
    if (type == AV_HWDEVICE_TYPE_NONE) {
        return false;
    }

#ifdef _WIN32
    // Windows GPU 链路：复用采集侧的 D3D11 设备，这样采集纹理可以只在显存里拷贝，
    // 既不需要 GPU→CPU 回读，也不会出现跨设备纹理无法 CopyResource 的问题。
    if (type == AV_HWDEVICE_TYPE_D3D11VA && m_externalDevice) {
        auto* device = static_cast<ID3D11Device*>(m_externalDevice);
        m_hwDeviceCtx = av_hwdevice_ctx_alloc(type);
        if (m_hwDeviceCtx) {
            auto* deviceCtx = reinterpret_cast<AVHWDeviceContext*>(m_hwDeviceCtx->data);
            auto* d3d11Ctx = static_cast<AVD3D11VADeviceContext*>(deviceCtx->hwctx);
            d3d11Ctx->device = device;
            device->AddRef();
            device->GetImmediateContext(&d3d11Ctx->device_context);
            m_d3dContext = d3d11Ctx->device_context;
            if (av_hwdevice_ctx_init(m_hwDeviceCtx) < 0) {
                TG_LOGW("VideoEncoder: 复用采集 D3D11 设备失败，改用独立设备");
                av_buffer_unref(&m_hwDeviceCtx);
                m_d3dContext = nullptr;
            }
        }
    }
#endif

    if (!m_hwDeviceCtx && av_hwdevice_ctx_create(&m_hwDeviceCtx, type, nullptr, nullptr, 0) < 0) {
        TG_LOGW("VideoEncoder: hwdevice 创建失败，回退 CPU 路径");
        m_hwDeviceCtx = nullptr;
        return false;
    }
    m_ctx->hw_device_ctx = av_buffer_ref(m_hwDeviceCtx);
    TG_LOGI("VideoEncoder: 硬件上下文就绪 type=%d", static_cast<int>(type));
    return true;
}

bool VideoEncoder::InitHardwareFrames(AVPixelFormat hwFmt)
{
    if (!m_hwDeviceCtx) {
        return false;
    }
    AVBufferRef* frames = av_hwframe_ctx_alloc(m_hwDeviceCtx);
    if (!frames) {
        return false;
    }
    auto* framesCtx = reinterpret_cast<AVHWFramesContext*>(frames->data);
    framesCtx->format = hwFmt;
    // D3D11 链路：采集纹理是 BGRA，硬件帧也按 BGRA 分配，显存内 CopySubresourceRegion 才能直拷；
    // 若编码器不接受 BGRA（如 AMF 只吃 nv12），则按 NV12 分配，由采集侧转好格式再拷。
    framesCtx->sw_format = (hwFmt == AV_PIX_FMT_D3D11 && m_ctx->pix_fmt != AV_PIX_FMT_NV12 &&
                            CodecAcceptsBgra(m_codecName))
                               ? AV_PIX_FMT_BGRA
                               : AV_PIX_FMT_NV12;
    framesCtx->width = m_ctx->width;
    framesCtx->height = m_ctx->height;
    framesCtx->initial_pool_size = 16;

    if (av_hwframe_ctx_init(frames) < 0) {
        TG_LOGW("VideoEncoder: hw_frames_ctx 初始化失败，回退 CPU 路径");
        av_buffer_unref(&frames);
        return false;
    }
    m_hwFramesCtx = frames;
    m_ctx->hw_frames_ctx = av_buffer_ref(frames);
    TG_LOGI("VideoEncoder: 硬件帧池就绪 fmt=%d sw=%d %dx%d",
            static_cast<int>(framesCtx->format), static_cast<int>(framesCtx->sw_format),
            framesCtx->width, framesCtx->height);
    return true;
}

#ifdef _WIN32
bool VideoEncoder::CopyFromD3D11Texture(const core::FramePtr& frame, AVFrame* dst)
{
    if (!dst || !m_d3dContext || !frame || !frame->hwHandle) {
        return false;
    }
    auto* src = static_cast<ID3D11Texture2D*>(frame->hwHandle);
    auto* dstTex = reinterpret_cast<ID3D11Texture2D*>(dst->data[0]);
    if (!dstTex) {
        return false;
    }

    // CopySubresourceRegion 是 1:1 块拷贝：尺寸或像素格式不一致都必须走回退。
    // 格式尤其容易漏：采集纹理恒为 BGRA，而编码器帧池的 sw_format 可能是 NV12
    // （h264_mf / h264_amf 不吃 BGRA）。只比宽高的话会把 BGRA 数据当 NV12 编码，画面全乱。
    D3D11_TEXTURE2D_DESC srcDesc = {};
    D3D11_TEXTURE2D_DESC dstDesc = {};
    src->GetDesc(&srcDesc);
    dstTex->GetDesc(&dstDesc);
    if (srcDesc.Width != dstDesc.Width || srcDesc.Height != dstDesc.Height) {
        TG_LOGW_THROTTLED(2000000, "VideoEncoder: GPU 帧尺寸 %ux%u != 编码器 %ux%u，需回退归一化",
                          srcDesc.Width, srcDesc.Height, dstDesc.Width, dstDesc.Height);
        return false;
    }
    if (srcDesc.Format != dstDesc.Format) {
        TG_LOGW_THROTTLED(2000000, "VideoEncoder: GPU 帧格式 %u != 编码器 %u，需回退归一化",
                          static_cast<unsigned>(srcDesc.Format),
                          static_cast<unsigned>(dstDesc.Format));
        return false;
    }

    const UINT subResource = static_cast<UINT>(reinterpret_cast<uintptr_t>(dst->data[1]));
    // 纯显存拷贝：无 CPU 回读、无像素搬运。
    // 注意：ID3D11DeviceContext::CopySubresourceRegion 返回 void，D3D11 的错误不通过返回值暴露
    // （只体现在 device-removed 上），所以"尺寸 + 格式"的前置校验就是这里唯一的防线。
    m_d3dContext->CopySubresourceRegion(dstTex, subResource, 0, 0, 0, src, 0, nullptr);
    return true;
}

bool VideoEncoder::EnsureStagingTexture(int width, int height)
{
    if (m_stagingTex && m_stagingWidth == width && m_stagingHeight == height) {
        return true;
    }
    if (m_stagingTex) {
        m_stagingTex->Release();
        m_stagingTex = nullptr;
    }
    auto* device = static_cast<ID3D11Device*>(m_externalDevice);
    if (!device) {
        return false;
    }
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &m_stagingTex))) {
        m_stagingTex = nullptr;
        return false;
    }
    m_stagingWidth = width;
    m_stagingHeight = height;
    return true;
}

bool VideoEncoder::ReadBackD3D11Texture(const core::FramePtr& frame, core::FramePtr& out)
{
    if (!m_d3dContext || !frame || !frame->hwHandle) {
        return false;
    }
    auto* src = static_cast<ID3D11Texture2D*>(frame->hwHandle);
    D3D11_TEXTURE2D_DESC desc = {};
    src->GetDesc(&desc);
    if (desc.Width == 0 || desc.Height == 0) {
        return false;
    }
    if (!EnsureStagingTexture(static_cast<int>(desc.Width), static_cast<int>(desc.Height))) {
        return false;
    }

    m_d3dContext->CopyResource(m_stagingTex, src);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(m_d3dContext->Map(m_stagingTex, 0, D3D11_MAP_READ, 0, &mapped))) {
        return false;
    }

    const int w = static_cast<int>(desc.Width);
    const int h = static_cast<int>(desc.Height);
    const size_t bytes = static_cast<size_t>(mapped.RowPitch) * h;
    auto buffer = std::shared_ptr<uint8_t>(new uint8_t[bytes], std::default_delete<uint8_t[]>());
    const uint8_t* srcPtr = static_cast<const uint8_t*>(mapped.pData);
    if (mapped.RowPitch == static_cast<UINT>(w * 4)) {
        memcpy(buffer.get(), srcPtr, bytes);
    } else {
        for (int y = 0; y < h; ++y) {
            memcpy(buffer.get() + static_cast<size_t>(y) * w * 4, srcPtr + static_cast<size_t>(y) * mapped.RowPitch,
                   static_cast<size_t>(w) * 4);
        }
    }
    m_d3dContext->Unmap(m_stagingTex, 0);

    out = core::MakeVideoFrame();
    out->width = w;
    out->height = h;
    out->pixFmt = AV_PIX_FMT_BGRA;
    out->planes[0] = buffer;
    out->strides[0] = w * 4;
    out->ptsUs = frame->ptsUs;
    out->captureUs = frame->captureUs;
    out->rotation = frame->rotation;
    out->sourceId = frame->sourceId;
    return true;
}
#endif

void VideoEncoder::Close()
{
    if (m_ctx) {
        avcodec_free_context(&m_ctx);
    }
    if (m_sws) {
        sws_freeContext(m_sws);
        m_sws = nullptr;
    }
    if (m_hwFramesCtx) {
        av_buffer_unref(&m_hwFramesCtx);
        m_hwFramesCtx = nullptr;
    }
    if (m_hwDeviceCtx) {
        av_buffer_unref(&m_hwDeviceCtx);
        m_hwDeviceCtx = nullptr;
    }
#ifdef _WIN32
    // staging 纹理是裸 COM 指针，必须在这里释放：漏掉的话每轮 Stop→Prepare 泄漏一张纹理，
    // 而且它引用着采集侧的 D3D11 设备，会比设备活得更久。
    if (m_stagingTex) {
        m_stagingTex->Release();
        m_stagingTex = nullptr;
    }
    m_stagingWidth = 0;
    m_stagingHeight = 0;
    // 上面 av_buffer_unref(m_hwDeviceCtx) 已经把 ffmpeg 持有的 device_context Release 掉了，
    // 这里必须置空，否则下一轮走 CPU 路径时 CopyFromD3D11Texture 会拿到悬垂指针。
    m_d3dContext = nullptr;
#endif
    m_useHwFrame = false;
    m_queue->Close();
    m_opened.store(false);
    // 关键：PTS 单调保护的状态必须随流复位。
    // 否则 Stop→Start 重开时，新一轮时间轴已从 0 起，而 m_lastPtsUs 还是上一轮的末值，
    // 所有视频帧都会被判成"PTS 回退"而丢弃，第二个文件只有音频甚至 0 字节。
    m_lastPtsUs = -1;
}

bool VideoEncoder::Submit(core::FramePtr frame)
{
    if (!m_opened.load() || !frame) {
        return false;
    }
    return m_queue->Push(std::move(frame));
}

size_t VideoEncoder::Pending() const
{
    return m_queue ? m_queue->Size() : 0;
}

bool VideoEncoder::OnCommand(const core::BusCommand& command)
{
    switch (command.type) {
        case core::BusCommandType::Flush:
            if (m_queue) {
                m_queue->Clear();
            }
            return true;
        case core::BusCommandType::RequestKeyFrame:
            m_forceKeyFrame.store(true);
            return true;
        default:
            return false;
    }
}

void VideoEncoder::Loop(std::stop_token token)
{
    while (!token.stop_requested()) {
        auto item = m_queue->TryPop(std::chrono::milliseconds(50));
        if (!item.has_value()) {
            continue;
        }
        EncodeOne(*item);   // 失败的归集与上报在 EncodeOne 内部（OnEncodeResult）
    }
    DrainLocked();
}

void VideoEncoder::OnThreadExit()
{
    TG_LOGI("VideoEncoder: 线程退出 codec=%s encoded=%llu dropped=%llu latency_us=%lld",
            m_codecName.c_str(),
            static_cast<unsigned long long>(m_encoded.load()),
            static_cast<unsigned long long>(m_dropped.load()),
            static_cast<long long>(m_encodeLatencyUs.load()));
}

int VideoEncoder::EncodeOne(const core::FramePtr& frame)
{
    if (!m_ctx || !frame) {
        TG_LOGW_THROTTLED(1000000, "VideoEncoder: 编码器未打开或空帧，丢弃（ctx=%d frame=%d）",
                          m_ctx ? 1 : 0, frame ? 1 : 0);
        return -1;
    }

    // 音画同步：视频落后音频过多则丢弃
    if (m_sync && frame->ptsUs >= 0) {
        // 领先/落后阈值都用同一套配置，不在这里硬编码魔法数字
        auto decision = m_sync->Judge(frame->ptsUs, m_maxLagUs, m_maxLagUs, nullptr);
        if (decision == core::AVSync::Decision::Drop) {
            m_dropped.fetch_add(1);
            TG_LOGW_THROTTLED(1000000, "VideoEncoder: 视频落后音频 %lldms 超阈值，丢帧",
                              static_cast<long long>(m_maxLagUs / 1000));
            return 0;
        }
    }
    // PTS 回退保护：dts 必须单调递增，否则 flv/ts 封装会直接报错写不出文件
    if (frame->ptsUs >= 0 && frame->ptsUs <= m_lastPtsUs) {
        m_dropped.fetch_add(1);
        TG_LOGW_THROTTLED(1000000, "VideoEncoder: PTS 回退 %lld <= %lld，丢帧",
                          static_cast<long long>(frame->ptsUs), static_cast<long long>(m_lastPtsUs));
        return 0;
    }
    if (frame->ptsUs >= 0) {
        m_lastPtsUs = frame->ptsUs;
    }

    const uint64_t frameNo = m_encoded.load();
    const bool trace = frameNo < 5;
    if (trace) {
        TG_LOGD("VideoEncoder[#%llu]: 编码 %dx%d pts=%lld", static_cast<unsigned long long>(frameNo),
                frame->width, frame->height, static_cast<long long>(frame->ptsUs));
    }
    auto avFrame = BuildAVFrame(frame);
    if (!avFrame) {
        // 每帧路径：必须限频，否则掉帧时会以帧率刷屏，反而拖慢采集
        TG_LOGW_THROTTLED(1000000, "VideoEncoder: BuildAVFrame 返回空，丢帧");
        return -2;
    }
    if (trace) {
        TG_LOGD("VideoEncoder[#%llu]: AVFrame fmt=%d %dx%d", static_cast<unsigned long long>(frameNo),
                avFrame->format, avFrame->width, avFrame->height);
    }

    // 总线命令 RequestKeyFrame：libopenh264 / nvenc / x264 等都认 pict_type=I 作为强制 IDR 的信号
    if (m_forceKeyFrame.exchange(false)) {
        avFrame->pict_type = AV_PICTURE_TYPE_I;
        TG_LOGI("VideoEncoder: 响应 RequestKeyFrame，本帧强制关键帧");
    }

    int ret = avcodec_send_frame(m_ctx, avFrame.get());
    if (ret < 0) {
        TG_LOGE_THROTTLED(1000000, "VideoEncoder: send_frame 失败 %d (%s)", ret, core::AvErr(ret));
        OnEncodeResult(false, ret);
        return -3;
    }
    if (trace) {
        TG_LOGD("VideoEncoder[#%llu]: send ok", static_cast<unsigned long long>(frameNo));
    }

    while (ret >= 0) {
        core::AVPacketPtr packet = core::MakeAVPacket();
        ret = avcodec_receive_packet(m_ctx, packet.get());
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            TG_LOGE_THROTTLED(1000000, "VideoEncoder: receive_packet 失败 %d (%s)", ret, core::AvErr(ret));
            OnEncodeResult(false, ret);
            return -4;
        }
        packet->flags |= (packet->flags & AV_PKT_FLAG_KEY) ? AV_PKT_FLAG_KEY : 0;
        DeliverPacket(std::move(packet));
    }

    m_encoded.fetch_add(1);
    OnEncodeResult(true, 0);
    // 端到端编码延迟 EMA（μs）：capture → encode out；captureUs 无效时跳过。
    // α=1/8：抗抖动但也能快速跟新；首次采样直接写入
    if (frame->captureUs != AV_NOPTS_VALUE) {
        const int64_t latencyUs = core::MonotonicClock::NowUs() - frame->captureUs;
        if (latencyUs >= 0 && latencyUs < 10000000) {   // >10s 视为异常忽略
            int64_t cur = m_encodeLatencyUs.load();
            int64_t next = (cur == 0) ? latencyUs : (cur + (latencyUs - cur) / 8);
            m_encodeLatencyUs.store(next);
        }
    }
    return 0;
}

// 编码失败对宿主是"画面停了"，和写包失败同等严重，必须上报；
// 但偶发一两帧失败不该惊动宿主，所以沿用统一的失败判定（连续 1s）。
void VideoEncoder::OnEncodeResult(bool ok, int ret)
{
    const int64_t nowUs = core::MonotonicClock::NowUs();
    if (ok) {
        if (m_encodeFail.OnSuccess(nowUs)) {
            TG_LOGI("VideoEncoder: 编码已恢复 codec=%s", m_codecName.c_str());
        }
        return;
    }
    m_lastAvError.store(ret);
    if (m_encodeFail.OnFailure(nowUs)) {
        char message[160];
        std::snprintf(message, sizeof(message), "video encode failing for %lldms (%s)",
                      static_cast<long long>(m_encodeFail.FailingUs(nowUs) / 1000),
                      core::AvErr(ret));
        ReportError(TGErr_VideoEncodeFailed, message, ret);
    }
}

int VideoEncoder::DrainLocked()
{
    if (!m_ctx) {
        return 0;
    }
    avcodec_send_frame(m_ctx, nullptr);
    while (true) {
        core::AVPacketPtr packet = core::MakeAVPacket();
        int ret = avcodec_receive_packet(m_ctx, packet.get());
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            TG_LOGW("VideoEncoder: drain 收包失败 %d (%s)", ret, core::AvErr(ret));
            break;
        }
        DeliverPacket(std::move(packet));
    }
    return 0;
}

void VideoEncoder::DeliverPacket(core::AVPacketPtr packet)
{
    if (!packet) {
        return;
    }
    auto out = std::make_shared<core::Frame>();
    out->kind = core::MediaKind::Packet;
    out->streamKind = core::MediaKind::Video;
    out->keyFrame = (packet->flags & AV_PKT_FLAG_KEY) != 0;
    out->packet = std::move(packet);
    Forward(std::move(out));
}

core::AVFramePtr VideoEncoder::BuildAVFrame(const core::FramePtr& frame)
{
    auto avFrame = core::MakeAVFrame();
    avFrame->format = m_ctx->pix_fmt;
    avFrame->width = m_ctx->width;
    avFrame->height = m_ctx->height;
    // 按编码器实际 time_base 换算（openh264 等可能调整 time_base）
    avFrame->pts = frame->ptsUs >= 0
                       ? av_rescale_q(frame->ptsUs, AVRational{1, 1000000}, m_ctx->time_base)
                       : AV_NOPTS_VALUE;
    avFrame->pkt_dts = AV_NOPTS_VALUE;

    // ---- 硬件直连（GPU 模式）----
    if (m_useHwFrame && frame->hwHandle != nullptr) {
        // 需要变换或分辨率不一致时，纯显存拷贝无法完成归一化。
        // 允许回读的话把纹理搬回内存交给归一化层，否则这一帧只能放弃。
        const bool needTransform = !m_transform.IsIdentity();
        if (needTransform && !m_gpuReadbackFallback) {
            TG_LOGW_THROTTLED(3000000, "VideoEncoder: GPU 模式需变换但回退已关闭，丢弃该帧");
            return nullptr;
        }
        if (needTransform) {
            core::FramePtr cpuFrame;
#ifdef _WIN32
            if (m_ctx->pix_fmt == AV_PIX_FMT_D3D11 && frame->hwKind == core::HwKind::D3D11Texture) {
                ReadBackD3D11Texture(frame, cpuFrame);
            }
#endif
            core::FramePtr normalized;
            if (cpuFrame &&
                core::ApplyVideoTransform(*cpuFrame, m_transform, m_ctx->width, m_ctx->height,
                                          AV_PIX_FMT_NV12, normalized) &&
                normalized) {
                // 硬件路径下 avFrame->format 是 AV_PIX_FMT_D3D11，sws_scale 写不进去。
                // 回退时必须换成软件帧：ffmpeg 会在 send_frame 时自动 upload 回显存。
                auto swFrame = core::MakeAVFrame();
                swFrame->format = AV_PIX_FMT_NV12;
                swFrame->width = m_ctx->width;
                swFrame->height = m_ctx->height;
                swFrame->pkt_dts = AV_NOPTS_VALUE;
                if (FillFromCpuFrame(normalized, swFrame)) {
                    return swFrame;
                }
            }
        }

#ifdef _WIN32
        // Windows：D3D11 纹理。从帧池取一个硬件帧，把采集纹理在显存里拷进去（无 CPU 回读）
        if (m_ctx->pix_fmt == AV_PIX_FMT_D3D11 && m_hwFramesCtx &&
            frame->hwKind == core::HwKind::D3D11Texture) {
            if (av_hwframe_get_buffer(m_hwFramesCtx, avFrame.get(), 0) < 0) {
                // 必须在这里结束本帧。原先只打一条"本帧走系统内存"就往下落穿，
                // 可 CPU 侧根本没有像素：控制流会走到下面的泛型硬件分支，
                // 把 D3D11 纹理写进 data[3]（D3D11 的约定是 data[0]=纹理、data[1]=子资源下标），
                // 等于把一个格式错误的硬件帧交给编码器。丢帧比喂坏数据安全。
                TG_LOGW_THROTTLED(3000000, "VideoEncoder: 硬件帧池取帧失败，丢弃本帧");
                return nullptr;
            }
            if (!CopyFromD3D11Texture(frame, avFrame.get())) {
                // 尺寸不匹配等原因导致显存拷贝失败：允许回读就转 CPU 归一化，否则放弃
                core::FramePtr cpuFrame;
                core::FramePtr normalized;
                if (m_gpuReadbackFallback && ReadBackD3D11Texture(frame, cpuFrame) && cpuFrame &&
                    core::ApplyVideoTransform(*cpuFrame, m_transform, m_ctx->width, m_ctx->height,
                                              AV_PIX_FMT_NV12, normalized) &&
                    normalized) {
                    auto swFrame = core::MakeAVFrame();
                    swFrame->format = AV_PIX_FMT_NV12;
                    swFrame->width = m_ctx->width;
                    swFrame->height = m_ctx->height;
                    swFrame->pkt_dts = AV_NOPTS_VALUE;
                    if (FillFromCpuFrame(normalized, swFrame)) {
                        return swFrame;
                    }
                }
                return nullptr;
            } else {
                return avFrame;
            }
        }
#endif
        // iOS VideoToolbox / Android MediaCodec：data[3] 存 native handle（零拷贝直通）
        if (m_ctx->pix_fmt == AV_PIX_FMT_VIDEOTOOLBOX &&
            frame->hwKind == core::HwKind::CVPixelBuffer) {
            avFrame->format = m_ctx->pix_fmt;
            avFrame->data[3] = reinterpret_cast<uint8_t*>(frame->hwHandle);
            AttachFrameOwner(avFrame.get(), frame);
            return avFrame;
        }
        if (m_ctx->hw_frames_ctx) {
            avFrame->hw_frames_ctx = av_buffer_ref(m_ctx->hw_frames_ctx);
        }
        if (avFrame->hw_frames_ctx) {
            avFrame->format = m_ctx->pix_fmt;
            avFrame->data[3] = reinterpret_cast<uint8_t*>(frame->hwHandle);
            AttachFrameOwner(avFrame.get(), frame);
            return avFrame;
        }
        TG_LOGW("VideoEncoder: 硬件帧不可用，本帧走系统内存");
    }

    // 硬件帧但编码器没走硬件链路（hwdevice / 帧池初始化失败时会出现）：
    // 这种情况 CPU 侧拿不到任何像素，只能丢弃，必须显式说出来 ——
    // 否则现象是"流正常、包在写、画面全黑"，排查时完全没有线索。
#if defined(__APPLE__)
    // iOS 例外：CVPixelBuffer 可以锁住直接读，回读后照常走系统内存链路
    if (frame->hwHandle != nullptr && frame->hwKind == core::HwKind::CVPixelBuffer) {
        if (const core::FramePtr cpu = ReadBackCVPixelBuffer(frame)) {
            const core::FramePtr source = NormalizeFrame(cpu);
            return FillFromCpuFrame(source ? source : cpu, avFrame) ? avFrame : nullptr;
        }
    }
#endif
    if (frame->hwHandle != nullptr) {
        TG_LOGW_THROTTLED(2000000, "VideoEncoder: 收到硬件帧但编码器为软件链路（hw=%d），丢帧；"
                                   "请把采集侧一并切回 CPU 模式",
                          m_useHwFrame ? 1 : 0);
        return nullptr;
    }

    // 系统内存帧：先做画面变换（若配置），再经 sws_scale 归一到编码器的尺寸与像素格式
    // （采集分辨率常与推流分辨率不同，直接 memcpy 会越界，这里统一做缩放/格式转换）
    if (frame->planes[0]) {
        const core::FramePtr source = NormalizeFrame(frame);
        return FillFromCpuFrame(source ? source : frame, avFrame) ? avFrame : nullptr;
    }

    // 空帧（仅用于 flush）
    return nullptr;
}

core::FramePtr VideoEncoder::NormalizeFrame(const core::FramePtr& frame)
{
    if (!frame || m_transform.IsIdentity()) {
        return frame;   // 恒等变换：不做任何额外处理（零开销）
    }
    core::FramePtr out;
    if (!core::ApplyVideoTransform(*frame, m_transform, m_ctx->width, m_ctx->height,
                                   AV_PIX_FMT_NV12, out) ||
        !out) {
        TG_LOGW_THROTTLED(3000000, "VideoEncoder: 画面归一化失败，本帧按原样编码");
        return frame;
    }
    return out;
}

bool VideoEncoder::FillFromCpuFrame(const core::FramePtr& src, core::AVFramePtr& avFrame)
{
    if (!src || !src->planes[0] || !avFrame || !m_ctx) {
        return false;
    }
    const int srcW = src->width > 0 ? src->width : m_ctx->width;
    const int srcH = src->height > 0 ? src->height : m_ctx->height;
    const AVPixelFormat srcFmt = src->pixFmt != AV_PIX_FMT_NONE ? src->pixFmt : AV_PIX_FMT_NV12;
    const AVPixelFormat dstFmt = static_cast<AVPixelFormat>(avFrame->format);
    avFrame->width = m_ctx->width;
    avFrame->height = m_ctx->height;

    // NV12/YUV420 子采样要求宽高为偶数：奇数 stride 会让 sws_scale 把末列截掉。
    // 这里不丢帧（下游时间轴依赖持续产出），但限频告警出来便于定位画面"莫名错位/发绿"
    // （对齐 MediaX FillFrom*** 的奇数尺寸告警）
    if ((srcW & 1) || (srcH & 1) || (src->strides[0] > 0 && (src->strides[0] & 1))) {
        TG_LOGW_THROTTLED(5000000,
                          "VideoEncoder: 奇数源尺寸/步幅 src=%dx%d stride[0]=%d dst=%dx%d，sws 会截末列",
                          srcW, srcH, src->strides[0], m_ctx->width, m_ctx->height);
    }

    // 归一化层的输出已经是编码器的尺寸与目标格式时，直接拷平面，
    // 不要再过一遍 sws_scale —— GPU 回退路径上这一帧可能几十 MB，省一次全帧搬运很值。
    if (srcFmt == dstFmt && srcW == m_ctx->width && srcH == m_ctx->height && src->planes[0] &&
        src->planes[1]) {
        if (av_frame_get_buffer(avFrame.get(), 32) < 0) {
            return false;
        }
        const int planes = (srcFmt == AV_PIX_FMT_NV12) ? 2 : 3;
        // 只有源的 stride 能完整放进目标 linesize 时才能逐行 memcpy。
        // 之前这里用 min(stride, linesize) 截行拷贝：源比目标宽时每行只拷一部分，
        // 画面右侧被裁掉并整体错位，而且没有任何报错 —— 属于最难排查的一类问题。
        bool strideFit = true;
        for (int p = 0; p < planes; ++p) {
            if (!src->planes[p] || src->strides[p] <= 0 || src->strides[p] > avFrame->linesize[p]) {
                strideFit = false;
                break;
            }
        }
        if (strideFit) {
            for (int p = 0; p < planes; ++p) {
                const int copyH = (p == 0) ? srcH : srcH / 2;
                const size_t rowBytes = static_cast<size_t>(src->strides[p]);
                for (int y = 0; y < copyH; ++y) {
                    memcpy(avFrame->data[p] + static_cast<size_t>(y) * avFrame->linesize[p],
                           src->planes[p].get() + static_cast<size_t>(y) * src->strides[p], rowBytes);
                }
            }
            avFrame->pts = src->ptsUs >= 0
                               ? av_rescale_q(src->ptsUs, AVRational{1, 1000000}, m_ctx->time_base)
                               : AV_NOPTS_VALUE;
            return true;
        }
        // stride 放不下：放弃这次分配的缓冲，交给 sws_scale 重新处理（它会按源 stride 正确重采样）
        TG_LOGW_THROTTLED(2000000, "VideoEncoder: 源 stride %d > 目标 linesize %d，改走 sws_scale",
                          src->strides[0], avFrame->linesize[0]);
        av_frame_unref(avFrame.get());
        avFrame->format = dstFmt;
        avFrame->width = m_ctx->width;
        avFrame->height = m_ctx->height;
    }

    if (!EnsureSwsContext(srcW, srcH, srcFmt)) {
        return false;
    }
    if (av_frame_get_buffer(avFrame.get(), 32) < 0) {
        return false;
    }
    const uint8_t* srcData[4] = {src->planes[0].get(), src->planes[1].get(),
                                 src->planes[2].get(), nullptr};
    int srcLinesize[4] = {src->strides[0], src->strides[1], src->strides[2], 0};
    sws_scale(m_sws, srcData, srcLinesize, 0, srcH, avFrame->data, avFrame->linesize);
    avFrame->pts = src->ptsUs >= 0
                       ? av_rescale_q(src->ptsUs, AVRational{1, 1000000}, m_ctx->time_base)
                       : AV_NOPTS_VALUE;
    return true;
}

bool VideoEncoder::EnsureSwsContext(int srcWidth, int srcHeight, AVPixelFormat srcFmt)
{
    if (m_sws && m_swsSrcWidth == srcWidth && m_swsSrcHeight == srcHeight && m_swsSrcFmt == srcFmt) {
        return true;
    }
    if (m_sws) {
        sws_freeContext(m_sws);
        m_sws = nullptr;
    }
    m_sws = sws_getContext(srcWidth, srcHeight, srcFmt, m_ctx->width, m_ctx->height, m_ctx->pix_fmt,
                           SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!m_sws) {
        TG_LOGE("VideoEncoder: sws_getContext 失败 %dx%d -> %dx%d", srcWidth, srcHeight, m_ctx->width, m_ctx->height);
        return false;
    }
    m_swsSrcWidth = srcWidth;
    m_swsSrcHeight = srcHeight;
    m_swsSrcFmt = srcFmt;
    return true;
}

}  // namespace TGCapture::codec
