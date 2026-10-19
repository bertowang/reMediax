// Windows 屏幕/窗口采集：GDI 位图采集 + libyuv 转 NV12（稳定兜底路径）
#ifndef TGCAPTURE_PLATFORM_WIN_WIN_SCREEN_CAPTURE_H
#define TGCAPTURE_PLATFORM_WIN_WIN_SCREEN_CAPTURE_H

#include <windows.h>

#include "capture/Source.h"

namespace TGCapture::platform
{

class WinScreenCapture : public capture::VideoSourceBase
{
public:
    WinScreenCapture(int monitorIndex, VideoScaleStrategy strategy);
    explicit WinScreenCapture(HWND hwnd, VideoScaleStrategy strategy);
    ~WinScreenCapture() override;

    int Open(int width, int height, float fps) override;

protected:
    void Loop(std::stop_token token) override;

private:
    bool GrabBGRA(std::shared_ptr<uint8_t>& bgra, int& width, int& height);
    void ConvertToNV12(const uint8_t* bgra, int width, int height, core::FramePtr& out);

private:
    HWND m_hwnd = nullptr;
    int m_monitorIndex = 0;
    bool m_isWindow = false;

    HDC m_screenDC = nullptr;
    HDC m_memDC = nullptr;
    HBITMAP m_bitmap = nullptr;
    int m_bmpWidth = 0;
    int m_bmpHeight = 0;
    uint8_t* m_bmpBits = nullptr;
    int m_grabFail = 0;
    int m_frameCount = 0;
};

}  // namespace TGCapture::platform

#endif  // TGCAPTURE_PLATFORM_WIN_WIN_SCREEN_CAPTURE_H
