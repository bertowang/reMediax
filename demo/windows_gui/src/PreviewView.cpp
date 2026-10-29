#include "PreviewView.h"

#include <algorithm>

#include "StringUtil.h"

namespace tgui
{

namespace
{

inline uint32_t Clamp8(int v)
{
    return static_cast<uint32_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

}  // namespace

void PreviewView::Attach(HWND view)
{
    m_view = view;
    UpdateTargetSize();
}

void PreviewView::UpdateTargetSize()
{
    RECT rc{};
    if (m_view && GetClientRect(m_view, &rc)) {
        m_targetW.store(static_cast<int>(rc.right - rc.left));
        m_targetH.store(static_cast<int>(rc.bottom - rc.top));
    }
    if (m_view) {
        InvalidateRect(m_view, nullptr, FALSE);
    }
}

void PreviewView::Reset(const std::wstring& hint)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pixels.clear();
        m_imageW = m_imageH = 0;
        m_srcW = m_srcH = 0;
        m_hardware = false;
        m_hint = hint;
    }
    m_fps.store(0);
    if (m_view) {
        InvalidateRect(m_view, nullptr, FALSE);
    }
}

void PreviewView::Invalidate()
{
    // 界面线程还没画完上一帧就不再投递：重绘请求不会堆积，画的永远是最新一帧
    if (m_view && !m_paintPending.exchange(true)) {
        InvalidateRect(m_view, nullptr, FALSE);
    }
}

// ------------------------------------------------------------------ 预览投递线程
void PreviewView::OnFrame(const TGCapture::PreviewFrame& frame)
{
    const uint64_t nowMs = GetTickCount64();
    if (m_fpsWindowStartMs == 0) {
        m_fpsWindowStartMs = nowMs;
    }
    ++m_fpsFrames;
    if (nowMs - m_fpsWindowStartMs >= 1000) {
        m_fps.store(static_cast<int>(m_fpsFrames * 1000 / (nowMs - m_fpsWindowStartMs)));
        m_fpsWindowStartMs = nowMs;
        m_fpsFrames = 0;
    }

    if (frame.format == TGCapture::PreviewPixelFormat::Hardware) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_hardware = true;
            m_pixels.clear();
            m_srcW = frame.width;
            m_srcH = frame.height;
        }
        Invalidate();
        return;
    }
    if (!frame.y || !frame.uv || frame.width <= 0 || frame.height <= 0) {
        return;
    }
    const int tw = m_targetW.load();
    const int th = m_targetH.load();
    if (tw <= 0 || th <= 0) {
        return;
    }

    // 等比缩放到控件内（长边贴边，另一边留黑）
    int dw = tw;
    int dh = static_cast<int>(static_cast<int64_t>(tw) * frame.height / frame.width);
    if (dh > th) {
        dh = th;
        dw = static_cast<int>(static_cast<int64_t>(th) * frame.width / frame.height);
    }
    dw = (std::max)(dw, 1);
    dh = (std::max)(dh, 1);

    if (m_xMapSrcW != frame.width || m_xMapDstW != dw) {
        m_xMap.resize(static_cast<size_t>(dw));
        for (int x = 0; x < dw; ++x) {
            // 取目标像素中心对应的源列
            m_xMap[x] = static_cast<int>(static_cast<int64_t>(2 * x + 1) * frame.width / (2 * dw));
        }
        m_xMapSrcW = frame.width;
        m_xMapDstW = dw;
    }

    // NV12（BT.601 限幅）→ BGRA。合成器输出的都是限幅 YUV（全幅的 yuvj 输入在采集侧已压到限幅）
    m_scratch.resize(static_cast<size_t>(dw) * dh);
    for (int y = 0; y < dh; ++y) {
        const int sy = static_cast<int>(static_cast<int64_t>(2 * y + 1) * frame.height / (2 * dh));
        const uint8_t* yRow = frame.y + static_cast<size_t>(sy) * frame.yStride;
        const uint8_t* uvRow = frame.uv + static_cast<size_t>(sy / 2) * frame.uvStride;
        uint32_t* out = m_scratch.data() + static_cast<size_t>(y) * dw;
        for (int x = 0; x < dw; ++x) {
            const int sx = m_xMap[x];
            const int c = 298 * (yRow[sx] - 16);
            const int d = uvRow[sx & ~1] - 128;
            const int e = uvRow[(sx & ~1) + 1] - 128;
            const uint32_t r = Clamp8((c + 409 * e + 128) >> 8);
            const uint32_t g = Clamp8((c - 100 * d - 208 * e + 128) >> 8);
            const uint32_t b = Clamp8((c + 516 * d + 128) >> 8);
            out[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pixels.swap(m_scratch);
        m_imageW = dw;
        m_imageH = dh;
        m_srcW = frame.width;
        m_srcH = frame.height;
        m_hardware = false;
    }
    Invalidate();
}

// ------------------------------------------------------------------ 界面线程
void PreviewView::Paint(const DRAWITEMSTRUCT& dis)
{
    m_paintPending.store(false);
    HDC dc = dis.hDC;
    const RECT rc = dis.rcItem;
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    HBRUSH black = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));

    HFONT font = reinterpret_cast<HFONT>(SendMessageW(m_view, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
    SetBkMode(dc, TRANSPARENT);

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_hardware && !m_pixels.empty()) {
        const int x0 = rc.left + (w - m_imageW) / 2;
        const int y0 = rc.top + (h - m_imageH) / 2;
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = m_imageW;
        bmi.bmiHeader.biHeight = -m_imageH;   // 负数 = 自上而下
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        SetDIBitsToDevice(dc, x0, y0, static_cast<DWORD>(m_imageW), static_cast<DWORD>(m_imageH), 0, 0, 0,
                          static_cast<UINT>(m_imageH), m_pixels.data(), &bmi, DIB_RGB_COLORS);
        // 只补图片四周的黑边（不整块先涂黑），避免每帧闪烁
        const RECT bars[] = {
            {rc.left, rc.top, rc.right, y0},
            {rc.left, y0 + m_imageH, rc.right, rc.bottom},
            {rc.left, y0, x0, y0 + m_imageH},
            {x0 + m_imageW, y0, rc.right, y0 + m_imageH},
        };
        for (const RECT& r : bars) {
            if (r.right > r.left && r.bottom > r.top) {
                FillRect(dc, &r, black);
            }
        }
        const std::wstring label = Format(L" %dx%d  %dfps ", m_srcW, m_srcH, m_fps.load());
        SetTextColor(dc, RGB(255, 255, 0));
        TextOutW(dc, rc.left + 4, rc.top + 3, label.c_str(), static_cast<int>(label.size()));
    } else {
        FillRect(dc, &rc, black);
        const std::wstring text =
            m_hardware ? Format(L"预览中（%dx%d）\nGPU 性能模式下预览帧是显存纹理，本工具不做回读，不显示画面", m_srcW, m_srcH)
                       : m_hint;
        RECT box = rc;
        InflateRect(&box, -8, 0);
        RECT calc = box;
        DrawTextW(dc, text.c_str(), -1, &calc, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
        box.top = rc.top + (h - (calc.bottom - calc.top)) / 2;
        SetTextColor(dc, RGB(170, 170, 170));
        DrawTextW(dc, text.c_str(), -1, &box, DT_CENTER | DT_WORDBREAK);
    }
    if (oldFont) {
        SelectObject(dc, oldFont);
    }
}

}  // namespace tgui
