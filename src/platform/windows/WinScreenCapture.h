// Windows 屏幕 / 窗口采集（纯系统能力，不依赖任何第三方 SDK）
//
// 屏幕：
//   VideoCapMode::DXGI —— Desktop Duplication（IDXGIOutputDuplication），GPU 直接给帧，
//                         性能最好、不遮挡、支持多显示器；不可用时自动回落 GDI。
//   VideoCapMode::GDI  —— BitBlt 兜底（远程桌面 / 部分虚拟显卡 / 无 DXGI 1.2 环境）。
// 窗口：
//   VideoCapMode::WGC          —— 整窗（含标题栏边框）
//   VideoCapMode::WGC_No_TBar  —— 仅客户区（去标题栏/边框）
//   实现走 GetWindowDC / GetDC + BitBlt，窗口被遮挡或最小化时回落 PrintWindow(PW_RENDERFULLCONTENT)。
//
// 输出：统一转 NV12 系统内存帧（libyuv），与 openh264 / NVENC / AMF 的输入要求一致。
#ifndef TGCAPTURE_PLATFORM_WIN_WIN_SCREEN_CAPTURE_H
#define TGCAPTURE_PLATFORM_WIN_WIN_SCREEN_CAPTURE_H

#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>

#include "capture/Source.h"

namespace TGCapture::platform
{

class WinScreenCapture : public capture::VideoSourceBase
{
public:
    // 屏幕：monitorIndex 为显示器序号（0 = 主显示器），mode 决定 DXGI / GDI
    WinScreenCapture(int monitorIndex, VideoCapMode mode, VideoScaleStrategy strategy);
    // 窗口：WGC = 整窗，WGC_No_TBar = 仅客户区
    WinScreenCapture(HWND hwnd, VideoCapMode mode, VideoScaleStrategy strategy);
    ~WinScreenCapture() override;

    int Open(int width, int height, float fps) override;

    // 当前实际使用的采集路径（Open 之后有效），便于排查"为什么没走 DXGI"
    VideoCapMode ActiveMode() const noexcept { return m_activeMode; }

    // GPU 模式：帧留在显存（ID3D11Texture2D），不再 GPU→CPU 回读、不再分配系统内存位图
    void SetGpuMode(bool gpu) noexcept { m_gpuMode = gpu; }
    [[nodiscard]] bool GpuMode() const noexcept { return m_gpuMode; }
    // 建好 D3D11 设备与桌面复制通道并返回设备（幂等）。
    // 编码器要用同一个设备，才能在显存内直拷（跨设备无法 CopyResource）。
    void* EnsureD3D11Device();
    [[nodiscard]] void* D3D11Device() const noexcept { return m_device; }

protected:
    void Loop(std::stop_token token) override;

private:
    bool ResolveSourceRect(int& x, int& y, int& width, int& height);
    bool InitDXGI(int width, int height);
    void ReleaseDXGI();
    bool GrabDXGI(std::shared_ptr<uint8_t>& bgra, int& width, int& height);
    bool AcquireDesktopTexture();                 // 取一帧桌面纹理到 m_desktopTex（GPU 路径用）
    bool GrabGpu(ID3D11Texture2D** tex, std::shared_ptr<void>& owner, int& width, int& height);
    std::shared_ptr<void> MakeTextureOwner(ID3D11Texture2D* tex);
    bool GrabGDI(std::shared_ptr<uint8_t>& bgra, int& width, int& height);
    bool GrabBGRA(std::shared_ptr<uint8_t>& bgra, int& width, int& height);
    void ConvertToNV12(const uint8_t* bgra, int width, int height, core::FramePtr& out);

private:
    VideoCapMode m_mode = GDI;
    VideoCapMode m_activeMode = GDI;
    HWND m_hwnd = nullptr;
    int m_monitorIndex = 0;
    bool m_isWindow = false;
    bool m_clientOnly = false;

    // GDI
    HDC m_windowDC = nullptr;    // 窗口 DC（整窗）或显示器 DC
    HDC m_memDC = nullptr;
    HBITMAP m_bitmap = nullptr;
    int m_bmpWidth = 0;
    int m_bmpHeight = 0;
    uint8_t* m_bmpBits = nullptr;

    // DXGI Desktop Duplication（裸 COM 指针，与 WinAudioCapture 保持同一风格）
    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;
    IDXGIOutputDuplication* m_duplication = nullptr;
    ID3D11Texture2D* m_staging = nullptr;
    DXGI_OUTDUPL_DESC m_dupDesc{};

    // 桌面无变化时 DXGI 不给新帧，复用上一帧以维持恒定帧率（直播必需）
    std::shared_ptr<uint8_t> m_lastBgra;
    int m_lastWidth = 0;
    int m_lastHeight = 0;

    bool m_gpuMode = false;
    ID3D11Texture2D* m_lastTex = nullptr;   // 采集自留的一张纹理（GPU 路径：桌面静止时复用）
    bool m_lastTexDirty = true;             // 自留纹理是否需要用新帧刷新
    struct GpuTexturePool;
    std::shared_ptr<GpuTexturePool> m_gpuPool;

    int m_grabFail = 0;
    int m_frameCount = 0;
};

}  // namespace TGCapture::platform

#endif  // TGCAPTURE_PLATFORM_WIN_WIN_SCREEN_CAPTURE_H
