// 预览区：把 SDK 的预览帧（NV12）画到对话框里的一个 owner-draw 静态控件上。
//
//   - OnFrame 在内核的预览投递线程上调用：按控件大小等比缩放（最近邻采样）并转成 BGRA，存一份最新帧，
//     然后 InvalidateRect 让界面线程重绘。转换只做到控件大小（几百 x 几百），每帧耗时远小于 1ms。
//     SDK 只送"最新一帧"，这里慢了也只是跳帧，不影响推流。
//   - Paint 在界面线程（WM_DRAWITEM）上调用：居中贴图，四周补黑边，左上角标出源帧尺寸与预览帧率。
//   - GPU 性能模式下预览帧是显存纹理（ID3D11Texture2D），本 demo 不做回读，只显示提示文字。
#ifndef TGUI_PREVIEW_VIEW_H
#define TGUI_PREVIEW_VIEW_H

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "TGCapture/TGCapturerInterface.h"

namespace tgui
{

class PreviewView
{
public:
    // 界面线程：绑定控件（WM_INITDIALOG 里调用）
    void Attach(HWND view);
    // 界面线程：控件大小变化后调用（下一帧起按新尺寸转换）
    void UpdateTargetSize();
    // 界面线程：清掉画面，显示提示文字（预览关闭 / 未开启时）
    void Reset(const std::wstring& hint);
    // 界面线程：WM_DRAWITEM
    void Paint(const DRAWITEMSTRUCT& dis);

    // 预览投递线程：SDK 预览帧回调
    void OnFrame(const TGCapture::PreviewFrame& frame);

private:
    void Invalidate();

    HWND m_view = nullptr;
    std::atomic<int> m_targetW{0};
    std::atomic<int> m_targetH{0};
    std::atomic<bool> m_paintPending{false};

    std::mutex m_mutex;              // 保护以下字段（预览线程写、界面线程读）
    std::vector<uint32_t> m_pixels;  // BGRA，自上而下，m_imageW x m_imageH
    int m_imageW = 0;
    int m_imageH = 0;
    int m_srcW = 0;                  // 源帧尺寸（= 采集 / 合成尺寸）
    int m_srcH = 0;
    bool m_hardware = false;
    std::wstring m_hint = L"预览未开启";

    // 以下只在预览线程上读写
    std::vector<uint32_t> m_scratch; // 转换缓冲，转完与 m_pixels 交换
    std::vector<int> m_xMap;         // 目标列 → 源列
    int m_xMapSrcW = 0;
    int m_xMapDstW = 0;
    uint64_t m_fpsWindowStartMs = 0;
    int m_fpsFrames = 0;
    std::atomic<int> m_fps{0};
};

}  // namespace tgui

#endif  // TGUI_PREVIEW_VIEW_H
