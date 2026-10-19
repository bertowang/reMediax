#include "platform/windows/WinScreenCapture.h"

#include <atomic>

#include "core/Clock.h"
#include "core/Logger.h"

extern "C"
{
#include <libyuv.h>
}

namespace TGCapture::platform
{

WinScreenCapture::WinScreenCapture(int monitorIndex, VideoScaleStrategy strategy)
    : capture::VideoSourceBase(TypeOfSelfScreen, strategy), m_monitorIndex(monitorIndex), m_isWindow(false)
{
}

WinScreenCapture::WinScreenCapture(HWND hwnd, VideoScaleStrategy strategy)
    : capture::VideoSourceBase(TypeOfWindow, strategy), m_hwnd(hwnd), m_isWindow(true)
{
}

WinScreenCapture::~WinScreenCapture()
{
    Stop();
    if (m_bitmap) {
        DeleteObject(m_bitmap);
    }
    if (m_memDC) {
        DeleteDC(m_memDC);
    }
    if (m_screenDC) {
        ReleaseDC(nullptr, m_screenDC);
    }
}

int WinScreenCapture::Open(int width, int height, float fps)
{
    m_fps = fps > 0 ? fps : 30.0f;
    m_screenDC = GetDC(nullptr);
    if (!m_screenDC) {
        return -1;
    }
    m_memDC = CreateCompatibleDC(m_screenDC);

    m_bmpWidth = GetSystemMetrics(SM_CXSCREEN);
    m_bmpHeight = GetSystemMetrics(SM_CYSCREEN);

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = m_bmpWidth;
    bmi.bmiHeader.biHeight = -m_bmpHeight;  // 自上而下
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    m_bitmap = CreateDIBSection(m_memDC, &bmi, DIB_RGB_COLORS, reinterpret_cast<void**>(&m_bmpBits), nullptr, 0);
    if (!m_bitmap || !m_bmpBits) {
        TG_LOGE("WinScreenCapture: CreateDIBSection 失败");
        return -2;
    }
    SelectObject(m_memDC, m_bitmap);

    TG_LOGI("WinScreenCapture: %dx%d fps=%.1f", m_bmpWidth, m_bmpHeight, m_fps);
    return 0;
}

void WinScreenCapture::Loop(std::stop_token token)
{
    const int64_t intervalUs = static_cast<int64_t>(1000000.0 / (m_fps > 0 ? m_fps : 30.0));
    int64_t nextUs = core::MonotonicClock::NowUs();

    while (!token.stop_requested()) {
        const int64_t nowUs = core::MonotonicClock::NowUs();
        if (nowUs < nextUs) {
            const int64_t waitUs = nextUs - nowUs;
            std::this_thread::sleep_for(std::chrono::microseconds(waitUs));
            continue;
        }
        nextUs += intervalUs;
        if (nextUs < nowUs) {
            nextUs = nowUs + intervalUs;  // 落后过多则重新对齐，避免追帧风暴
        }

        if (m_frameCount < 5) {
            TG_LOGD("WinScreenCapture: 抓取第 %d 帧", m_frameCount + 1);
        }
        std::shared_ptr<uint8_t> bgra;
        int width = 0, height = 0;
        if (!GrabBGRA(bgra, width, height)) {
            if (m_grabFail++ % 30 == 0) {
                TG_LOGW("WinScreenCapture: 抓取失败 count=%d", m_grabFail);
            }
            continue;
        }
        if (m_frameCount < 5) {
            TG_LOGD("WinScreenCapture: 抓取成功 -> 转换");
        }

        auto frame = std::make_shared<core::Frame>();
        frame->kind = core::MediaKind::Video;
        frame->captureUs = core::MonotonicClock::NowUs();
        ConvertToNV12(bgra.get(), width, height, frame);
        if (!frame->planes[0]) {
            continue;
        }
        frame->ptsUs = frame->captureUs;
        m_width = frame->width;
        m_height = frame->height;
        if (m_frameCount++ == 0) {
            TG_LOGI("WinScreenCapture: 首帧 %dx%d", frame->width, frame->height);
        }
        Emit(std::move(frame));
    }
}

bool WinScreenCapture::GrabBGRA(std::shared_ptr<uint8_t>& bgra, int& width, int& height)
{
    if (m_frameCount == 0) {
        TG_LOGI("WinScreenCapture: GrabBGRA enter bmp=%dx%d bits=%p", m_bmpWidth, m_bmpHeight,
                static_cast<void*>(m_bmpBits));
    }
    if (!m_bmpBits || m_bmpWidth <= 0 || m_bmpHeight <= 0) {
        return false;
    }
    HDC srcDC = nullptr;
    int srcX = 0, srcY = 0, srcW = 0, srcH = 0;

    if (m_isWindow && m_hwnd) {
        RECT rect{};
        if (!GetWindowRect(m_hwnd, &rect)) {
            return false;
        }
        srcX = rect.left;
        srcY = rect.top;
        srcW = rect.right - rect.left;
        srcH = rect.bottom - rect.top;
        srcDC = GetDC(m_hwnd);
        if (!srcDC) {
            return false;
        }
    } else {
        srcW = GetSystemMetrics(SM_CXSCREEN);
        srcH = GetSystemMetrics(SM_CYSCREEN);
        srcDC = m_screenDC;
    }

    if (srcW <= 0 || srcH <= 0) {
        if (m_isWindow && srcDC) {
            ReleaseDC(m_hwnd, srcDC);
        }
        return false;
    }

    // 缩放到目标位图尺寸（保持全屏位图，按窗口比例拉伸）
    const int dstW = m_bmpWidth;
    const int dstH = m_bmpHeight;
    SetStretchBltMode(m_memDC, COLORONCOLOR);
    BOOL ok = StretchBlt(m_memDC, 0, 0, dstW, dstH, srcDC, srcX, srcY, srcW, srcH, SRCCOPY);
    if (m_isWindow && srcDC) {
        ReleaseDC(m_hwnd, srcDC);
    }
    if (!ok) {
        return false;
    }

    const size_t bytes = static_cast<size_t>(dstW) * dstH * 4;
    bgra = std::shared_ptr<uint8_t>(new uint8_t[bytes], std::default_delete<uint8_t[]>());
    memcpy(bgra.get(), m_bmpBits, bytes);
    width = dstW;
    height = dstH;
    if (m_frameCount == 0) {
        TG_LOGI("WinScreenCapture: GrabBGRA ok %dx%d bytes=%zu", width, height, bytes);
    }
    return true;
}

void WinScreenCapture::ConvertToNV12(const uint8_t* bgra, int width, int height, core::FramePtr& out)
{
    const int strideY = width;
    const int strideUV = width;  // NV12 的 UV 交错，行宽等于 width
    const size_t ySize = static_cast<size_t>(strideY) * height;
    const size_t uvSize = static_cast<size_t>(strideUV) * height / 2;

    auto buffer = std::shared_ptr<uint8_t>(new uint8_t[ySize + uvSize], std::default_delete<uint8_t[]>());
    uint8_t* y = buffer.get();
    uint8_t* uv = buffer.get() + ySize;

    if (m_frameCount == 0) {
        TG_LOGI("WinScreenCapture: 转换 NV12 %dx%d", width, height);
    }
    if (libyuv::ARGBToNV12(bgra, width * 4, y, strideY, uv, strideUV, width, height) != 0) {
        TG_LOGW("WinScreenCapture: ARGBToNV12 失败");
        return;
    }
    if (m_frameCount == 0) {
        TG_LOGI("WinScreenCapture: 转换完成");
    }

    out->width = width;
    out->height = height;
    out->pixFmt = AV_PIX_FMT_NV12;
    out->planes[0] = buffer;
    out->strides[0] = strideY;
    out->planes[1] = std::shared_ptr<uint8_t>(buffer, uv);  // 别名：共用同一块 buffer
    out->strides[1] = strideUV;
}

}  // namespace TGCapture::platform
