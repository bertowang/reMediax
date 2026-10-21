#include "platform/windows/WinScreenCapture.h"

#include <atomic>
#include <mutex>
#include <vector>

#include "core/Clock.h"
#include "core/Logger.h"

extern "C"
{
#include <libyuv.h>
}

namespace TGCapture::platform
{

namespace
{

// 采集尺寸必须是偶数（NV12 的 UV 是 2x2 下采样）
int Even(int v)
{
    return v > 0 ? (v & ~1) : 0;
}

struct MonitorEnumParam
{
    int target = 0;
    int current = 0;
    RECT rect{};
};

}  // namespace

// 显存纹理池：采集把桌面纹理拷到池里的纹理，交给编码器；
// 帧被释放（编码完成）时纹理自动归还，避免拷贝与复用打架，也避免每帧新建纹理。
struct WinScreenCapture::GpuTexturePool
{
    ID3D11Device* device = nullptr;
    UINT width = 0;
    UINT height = 0;
    std::mutex mutex;
    std::vector<ID3D11Texture2D*> idle;
    std::vector<ID3D11Texture2D*> all;

    ~GpuTexturePool()
    {
        for (auto* tex : all) {
            if (tex) {
                tex->Release();
            }
        }
    }

    ID3D11Texture2D* Acquire()
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!idle.empty()) {
            auto* tex = idle.back();
            idle.pop_back();
            return tex;
        }
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        ID3D11Texture2D* tex = nullptr;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &tex)) || !tex) {
            return nullptr;
        }
        all.push_back(tex);
        return tex;
    }

    void GiveBack(ID3D11Texture2D* tex)
    {
        std::lock_guard<std::mutex> lock(mutex);
        idle.push_back(tex);
    }
};

namespace
{

BOOL CALLBACK MonitorEnumProc(HMONITOR monitor, HDC, LPRECT, LPARAM lparam)
{
    auto* param = reinterpret_cast<MonitorEnumParam*>(lparam);
    if (param->current++ != param->target) {
        return TRUE;   // 继续枚举
    }
    MONITORINFO info{};
    info.cbSize = sizeof(MONITORINFO);
    if (GetMonitorInfoW(monitor, &info)) {
        param->rect = info.rcMonitor;
    }
    return FALSE;
}

}  // namespace

WinScreenCapture::WinScreenCapture(int monitorIndex, VideoCapMode mode, VideoScaleStrategy strategy)
    : capture::VideoSourceBase(TypeOfSelfScreen, strategy),
      m_mode(mode),
      m_monitorIndex(monitorIndex >= 0 ? monitorIndex : 0),
      m_isWindow(false)
{
    if (mode != DXGI && mode != GDI) {
        m_mode = DXGI;   // 屏幕采集只认 DXGI / GDI，其它按 DXGI 处理（内部会回落）
    }
}

WinScreenCapture::WinScreenCapture(HWND hwnd, VideoCapMode mode, VideoScaleStrategy strategy)
    : capture::VideoSourceBase(TypeOfWindow, strategy),
      m_mode(mode),
      m_hwnd(hwnd),
      m_isWindow(true),
      m_clientOnly(mode == WGC_No_TBar)
{
}

WinScreenCapture::~WinScreenCapture()
{
    Stop();
    ReleaseDXGI();
    if (m_bitmap) {
        DeleteObject(m_bitmap);
        m_bitmap = nullptr;
    }
    if (m_memDC) {
        DeleteDC(m_memDC);
        m_memDC = nullptr;
    }
    if (m_windowDC) {
        if (m_isWindow) {
            ReleaseDC(m_hwnd, m_windowDC);
        } else {
            DeleteDC(m_windowDC);
        }
        m_windowDC = nullptr;
    }
}

int WinScreenCapture::Open(int width, int height, float fps)
{
    m_fps = fps > 0 ? fps : 30.0f;

    int srcX = 0, srcY = 0, srcW = 0, srcH = 0;
    if (!ResolveSourceRect(srcX, srcY, srcW, srcH) || srcW <= 0 || srcH <= 0) {
        TG_LOGE("WinScreenCapture: 取不到有效的采集区域（窗口=%d 显示器=%d）",
                static_cast<int>(m_isWindow), m_monitorIndex);
        return -1;
    }

    m_bmpWidth = Even(srcW);
    m_bmpHeight = Even(srcH);

    // 屏幕采集优先 DXGI；失败自动回落 GDI
    // 注意：EnsureD3D11Device() 可能已经建好复制通道，这里必须复用而不是重复 DuplicateOutput
    m_activeMode = GDI;
    if (!m_isWindow && m_duplication) {
        m_activeMode = DXGI;
        m_bmpWidth = Even(static_cast<int>(m_dupDesc.ModeDesc.Width));
        m_bmpHeight = Even(static_cast<int>(m_dupDesc.ModeDesc.Height));
        if (m_gpuMode) {
            m_width = m_bmpWidth;
            m_height = m_bmpHeight;
            TG_LOGI("WinScreenCapture: 屏幕 GPU %dx%d fps=%.1f", m_bmpWidth, m_bmpHeight, m_fps);
            return 0;
        }
    } else if (!m_isWindow && (m_mode == DXGI || m_gpuMode) && InitDXGI(m_bmpWidth, m_bmpHeight)) {
        m_activeMode = DXGI;
        if (m_gpuMode) {
            // GPU 模式：不建系统内存位图（4K 屏能省掉几十 MB，且没有 GPU→CPU 回读）
            m_width = m_bmpWidth;
            m_height = m_bmpHeight;
            TG_LOGI("WinScreenCapture: %s GPU %dx%d fps=%.1f", "屏幕", m_bmpWidth, m_bmpHeight, m_fps);
            return 0;
        }
    } else {
        m_windowDC = m_isWindow ? (m_clientOnly ? GetDC(m_hwnd) : GetWindowDC(m_hwnd))
                                : CreateDCW(L"DISPLAY", nullptr, nullptr, nullptr);
        if (!m_windowDC) {
            TG_LOGE("WinScreenCapture: 获取源 DC 失败");
            return -2;
        }
        m_memDC = CreateCompatibleDC(m_windowDC);
        if (!m_memDC) {
            return -3;
        }

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = m_bmpWidth;
        bmi.bmiHeader.biHeight = -m_bmpHeight;  // 自上而下
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        m_bitmap = CreateDIBSection(m_memDC, &bmi, DIB_RGB_COLORS,
                                    reinterpret_cast<void**>(&m_bmpBits), nullptr, 0);
        if (!m_bitmap || !m_bmpBits) {
            TG_LOGE("WinScreenCapture: CreateDIBSection 失败");
            return -4;
        }
        SelectObject(m_memDC, m_bitmap);
    }

    m_width = m_bmpWidth;
    m_height = m_bmpHeight;
    TG_LOGI("WinScreenCapture: %s %s %dx%d fps=%.1f",
            m_isWindow ? (m_clientOnly ? "窗口(客户区)" : "窗口") : "屏幕",
            m_activeMode == DXGI ? "DXGI" : "GDI", m_bmpWidth, m_bmpHeight, m_fps);
    return 0;
}

bool WinScreenCapture::ResolveSourceRect(int& x, int& y, int& width, int& height)
{
    if (m_isWindow) {
        if (!m_hwnd || !IsWindow(m_hwnd)) {
            return false;
        }
        if (m_clientOnly) {
            RECT rect{};
            if (!GetClientRect(m_hwnd, &rect)) {
                return false;
            }
            x = 0;
            y = 0;
            width = rect.right - rect.left;
            height = rect.bottom - rect.top;
        } else {
            RECT rect{};
            if (!GetWindowRect(m_hwnd, &rect)) {
                return false;
            }
            x = 0;
            y = 0;   // 用窗口 DC 采集，原点即窗口左上角
            width = rect.right - rect.left;
            height = rect.bottom - rect.top;
        }
        return true;
    }

    MonitorEnumParam param;
    param.target = m_monitorIndex;
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, reinterpret_cast<LPARAM>(&param));
    if (param.rect.right <= param.rect.left || param.rect.bottom <= param.rect.top) {
        // 序号越界：回落主显示器
        x = 0;
        y = 0;
        width = GetSystemMetrics(SM_CXSCREEN);
        height = GetSystemMetrics(SM_CYSCREEN);
        return true;
    }
    x = param.rect.left;
    y = param.rect.top;
    width = param.rect.right - param.rect.left;
    height = param.rect.bottom - param.rect.top;
    return true;
}

bool WinScreenCapture::InitDXGI(int width, int height)
{
    (void)width;
    (void)height;

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                   nullptr, 0, D3D11_SDK_VERSION, &m_device, nullptr, &m_context);
    if (FAILED(hr) || !m_device) {
        TG_LOGW("WinScreenCapture: D3D11CreateDevice 失败 hr=0x%08X，回落 GDI", static_cast<unsigned>(hr));
        ReleaseDXGI();
        return false;
    }

    IDXGIDevice* dxgiDevice = nullptr;
    hr = m_device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice));
    if (FAILED(hr) || !dxgiDevice) {
        ReleaseDXGI();
        return false;
    }
    IDXGIAdapter* adapter = nullptr;
    hr = dxgiDevice->GetParent(__uuidof(IDXGIAdapter), reinterpret_cast<void**>(&adapter));
    dxgiDevice->Release();
    if (FAILED(hr) || !adapter) {
        ReleaseDXGI();
        return false;
    }

    IDXGIOutput* output = nullptr;
    hr = adapter->EnumOutputs(static_cast<UINT>(m_monitorIndex), &output);
    if (FAILED(hr)) {
        hr = adapter->EnumOutputs(0, &output);
    }
    adapter->Release();
    if (FAILED(hr) || !output) {
        TG_LOGW("WinScreenCapture: EnumOutputs 失败，回落 GDI");
        ReleaseDXGI();
        return false;
    }

    IDXGIOutput1* output1 = nullptr;
    hr = output->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&output1));
    output->Release();
    if (FAILED(hr) || !output1) {
        TG_LOGW("WinScreenCapture: 不支持 DXGI 1.2，回落 GDI");
        ReleaseDXGI();
        return false;
    }

    hr = output1->DuplicateOutput(m_device, &m_duplication);
    output1->Release();
    if (FAILED(hr) || !m_duplication) {
        TG_LOGW("WinScreenCapture: DuplicateOutput 失败 hr=0x%08X，回落 GDI", static_cast<unsigned>(hr));
        ReleaseDXGI();
        return false;
    }
    m_duplication->GetDesc(&m_dupDesc);

    m_bmpWidth = Even(static_cast<int>(m_dupDesc.ModeDesc.Width));
    m_bmpHeight = Even(static_cast<int>(m_dupDesc.ModeDesc.Height));

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = m_dupDesc.ModeDesc.Width;
    desc.Height = m_dupDesc.ModeDesc.Height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;

    if (m_gpuMode) {
        // GPU 模式：不需要 STAGING（那是给 CPU 回读用的），只留一张自留纹理，
        // 用于桌面静止时复用上一帧维持帧率。
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = 0;
        hr = m_device->CreateTexture2D(&desc, nullptr, &m_lastTex);
        if (FAILED(hr) || !m_lastTex) {
            TG_LOGW("WinScreenCapture: 创建自留纹理失败，回落 GDI");
            ReleaseDXGI();
            return false;
        }
        m_lastTexDirty = true;
        return true;
    }

    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = m_device->CreateTexture2D(&desc, nullptr, &m_staging);
    if (FAILED(hr) || !m_staging) {
        TG_LOGW("WinScreenCapture: 创建 staging 纹理失败，回落 GDI");
        ReleaseDXGI();
        return false;
    }
    return true;
}

void* WinScreenCapture::EnsureD3D11Device()
{
    if (m_device && m_duplication) {
        return m_device;
    }
    if (!InitDXGI(0, 0)) {
        ReleaseDXGI();
        return nullptr;
    }
    m_activeMode = DXGI;
    m_bmpWidth = Even(static_cast<int>(m_dupDesc.ModeDesc.Width));
    m_bmpHeight = Even(static_cast<int>(m_dupDesc.ModeDesc.Height));

    // 纹理池 + 自留纹理（GPU 模式专用）
    m_gpuPool = std::make_shared<GpuTexturePool>();
    m_gpuPool->device = m_device;
    m_gpuPool->width = static_cast<UINT>(m_dupDesc.ModeDesc.Width);
    m_gpuPool->height = static_cast<UINT>(m_dupDesc.ModeDesc.Height);
    m_lastTexDirty = true;
    return m_device;
}

void WinScreenCapture::ReleaseDXGI()
{
    m_lastBgra.reset();
    m_lastWidth = 0;
    m_lastHeight = 0;
    if (m_lastTex) {
        m_lastTex->Release();
        m_lastTex = nullptr;
    }
    m_gpuPool.reset();
    if (m_staging) {
        m_staging->Release();
        m_staging = nullptr;
    }
    if (m_duplication) {
        m_duplication->Release();
        m_duplication = nullptr;
    }
    if (m_context) {
        m_context->Release();
        m_context = nullptr;
    }
    if (m_device) {
        m_device->Release();
        m_device = nullptr;
    }
}

bool WinScreenCapture::GrabDXGI(std::shared_ptr<uint8_t>& bgra, int& width, int& height)
{
    if (!m_duplication) {
        return false;
    }
    IDXGIResource* resource = nullptr;
    DXGI_OUTDUPL_FRAME_INFO info{};
    ID3D11Texture2D* tex = nullptr;
    HRESULT hr = m_duplication->AcquireNextFrame(0, &info, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        // 桌面无变化：复用上一帧，避免直播流断帧
        if (m_lastBgra && m_lastWidth > 0) {
            bgra = m_lastBgra;
            width = m_lastWidth;
            height = m_lastHeight;
            return true;
        }
        return false;
    }
    if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL) {
        // 分辨率切换 / 全屏切换后需要重建
        TG_LOGW("WinScreenCapture: DXGI 失效(0x%08X)，重建复制通道", static_cast<unsigned>(hr));
        ReleaseDXGI();
        if (!InitDXGI(0, 0)) {
            m_activeMode = GDI;
        }
        return false;
    }
    if (FAILED(hr)) {
        return false;
    }

    hr = resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex));
    resource->Release();
    if (FAILED(hr) || !tex) {
        m_duplication->ReleaseFrame();
        return false;
    }
    m_context->CopyResource(m_staging, tex);
    tex->Release();
    m_duplication->ReleaseFrame();

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(m_context->Map(m_staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        return false;
    }

    const int w = Even(static_cast<int>(m_dupDesc.ModeDesc.Width));
    const int h = Even(static_cast<int>(m_dupDesc.ModeDesc.Height));
    const size_t rowBytes = static_cast<size_t>(w) * 4;
    const size_t total = rowBytes * static_cast<size_t>(h);
    bgra = std::shared_ptr<uint8_t>(new uint8_t[total], std::default_delete<uint8_t[]>());

    const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);
    uint8_t* dst = bgra.get();
    const size_t copyRow = (rowBytes < mapped.RowPitch) ? rowBytes : mapped.RowPitch;
    for (int row = 0; row < h; ++row) {
        memcpy(dst + static_cast<size_t>(row) * rowBytes,
               src + static_cast<size_t>(row) * mapped.RowPitch, copyRow);
    }
    m_context->Unmap(m_staging, 0);

    width = w;
    height = h;
    m_lastBgra = bgra;
    m_lastWidth = w;
    m_lastHeight = h;
    return true;
}

bool WinScreenCapture::GrabGDI(std::shared_ptr<uint8_t>& bgra, int& width, int& height)
{
    if (!m_bmpBits || m_bmpWidth <= 0 || m_bmpHeight <= 0) {
        return false;
    }

    int srcX = 0, srcY = 0;
    HDC srcDC = m_windowDC;
    if (!m_isWindow) {
        // 屏幕：BitBlt 的原点是虚拟桌面坐标
        int dummyW = 0, dummyH = 0;
        ResolveSourceRect(srcX, srcY, dummyW, dummyH);
    }
    if (!srcDC) {
        return false;
    }

    BOOL ok = BitBlt(m_memDC, 0, 0, m_bmpWidth, m_bmpHeight, srcDC, srcX, srcY, SRCCOPY | CAPTUREBLT);
    if (!ok && m_isWindow) {
        // 窗口被遮挡 / 最小化：用 PrintWindow 取内容（Win8.1+ 支持完整内容渲染）
        ok = PrintWindow(m_hwnd, m_memDC, PW_RENDERFULLCONTENT);
        if (!ok) {
            ok = PrintWindow(m_hwnd, m_memDC, 0);
        }
    }
    if (!ok) {
        return false;
    }

    const size_t bytes = static_cast<size_t>(m_bmpWidth) * m_bmpHeight * 4;
    bgra = std::shared_ptr<uint8_t>(new uint8_t[bytes], std::default_delete<uint8_t[]>());
    memcpy(bgra.get(), m_bmpBits, bytes);
    width = m_bmpWidth;
    height = m_bmpHeight;
    return true;
}

bool WinScreenCapture::GrabBGRA(std::shared_ptr<uint8_t>& bgra, int& width, int& height)
{
    if (m_activeMode == DXGI) {
        return GrabDXGI(bgra, width, height);
    }
    return GrabGDI(bgra, width, height);
}

// GPU 路径：桌面纹理 -> 池里的纹理（纯显存拷贝），纹理由帧的 owner 在编码完成后归还
bool WinScreenCapture::GrabGpu(ID3D11Texture2D** tex, std::shared_ptr<void>& owner, int& width, int& height)
{
    if (!tex || !m_duplication || !m_gpuPool) {
        return false;
    }
    *tex = nullptr;

    IDXGIResource* resource = nullptr;
    DXGI_OUTDUPL_FRAME_INFO info{};
    ID3D11Texture2D* src = nullptr;
    HRESULT hr = m_duplication->AcquireNextFrame(0, &info, &resource);

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        // 桌面无变化：复用自留纹理，维持帧率（否则静止画面会断流）
        if (!m_lastTex) {
            return false;
        }
        ID3D11Texture2D* reuse = m_gpuPool->Acquire();
        if (!reuse) {
            return false;
        }
        m_context->CopyResource(reuse, m_lastTex);
        m_lastTexDirty = true;
        owner = MakeTextureOwner(reuse);
        *tex = reuse;
        width = m_bmpWidth;
        height = m_bmpHeight;
        return true;
    }
    if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL) {
        TG_LOGW("WinScreenCapture: DXGI 失效(0x%08X)，重建复制通道", static_cast<unsigned>(hr));
        ReleaseDXGI();
        if (!EnsureD3D11Device()) {
            m_activeMode = GDI;
        }
        return false;
    }
    if (FAILED(hr)) {
        return false;
    }

    hr = resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&src));
    resource->Release();
    if (FAILED(hr) || !src) {
        m_duplication->ReleaseFrame();
        return false;
    }

    ID3D11Texture2D* dst = m_gpuPool->Acquire();
    if (!dst) {
        src->Release();
        m_duplication->ReleaseFrame();
        return false;
    }
    m_context->CopyResource(dst, src);
    if (m_lastTexDirty && m_lastTex) {
        m_context->CopyResource(m_lastTex, src);
        m_lastTexDirty = false;
    }
    src->Release();
    m_duplication->ReleaseFrame();

    owner = MakeTextureOwner(dst);
    *tex = dst;
    width = m_bmpWidth;
    height = m_bmpHeight;
    return true;
}

std::shared_ptr<void> WinScreenCapture::MakeTextureOwner(ID3D11Texture2D* tex)
{
    std::shared_ptr<GpuTexturePool> pool = m_gpuPool;
    return std::shared_ptr<void>(tex, [pool](void* p) {
        if (p) {
            pool->GiveBack(static_cast<ID3D11Texture2D*>(p));
        }
    });
}

void WinScreenCapture::Loop(std::stop_token token)
{
    const int64_t intervalUs = static_cast<int64_t>(1000000.0 / (m_fps > 0 ? m_fps : 30.0));
    int64_t nextUs = core::MonotonicClock::NowUs();

    while (!token.stop_requested()) {
        const int64_t nowUs = core::MonotonicClock::NowUs();
        if (nowUs < nextUs) {
            std::this_thread::sleep_for(std::chrono::microseconds(nextUs - nowUs));
            continue;
        }
        nextUs += intervalUs;
        if (nextUs < nowUs) {
            nextUs = nowUs + intervalUs;   // 落后过多则重新对齐，避免追帧风暴
        }

        auto frame = std::make_shared<core::Frame>();
        frame->kind = core::MediaKind::Video;
        frame->captureUs = core::MonotonicClock::NowUs();

        if (m_gpuMode && m_activeMode == DXGI) {
            // GPU 路径：只传纹理句柄，不做像素搬运、不占系统内存
            ID3D11Texture2D* tex = nullptr;
            std::shared_ptr<void> owner;
            int width = 0, height = 0;
            if (!GrabGpu(&tex, owner, width, height)) {
                if (m_grabFail++ % 60 == 0) {
                    TG_LOGW("WinScreenCapture: GPU 抓取失败 count=%d", m_grabFail);
                }
                continue;
            }
            frame->width = width;
            frame->height = height;
            frame->pixFmt = AV_PIX_FMT_NONE;   // 硬件帧，格式由编码器侧帧池决定
            frame->hwKind = core::HwKind::D3D11Texture;
            frame->hwHandle = tex;
            frame->hwOwner = std::move(owner);
        } else {
            std::shared_ptr<uint8_t> bgra;
            int width = 0, height = 0;
            if (!GrabBGRA(bgra, width, height)) {
                if (m_grabFail++ % 60 == 0) {
                    TG_LOGW("WinScreenCapture: 抓取失败 count=%d", m_grabFail);
                }
                continue;
            }
            ConvertToNV12(bgra.get(), width, height, frame);
            if (!frame->planes[0]) {
                continue;
            }
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

void WinScreenCapture::ConvertToNV12(const uint8_t* bgra, int width, int height, core::FramePtr& out)
{
    const int strideY = width;
    const int strideUV = width;   // NV12 的 UV 交错，行宽等于 width
    const size_t ySize = static_cast<size_t>(strideY) * height;
    const size_t uvSize = static_cast<size_t>(strideUV) * height / 2;

    auto buffer = std::shared_ptr<uint8_t>(new uint8_t[ySize + uvSize], std::default_delete<uint8_t[]>());
    uint8_t* y = buffer.get();
    uint8_t* uv = buffer.get() + ySize;

    if (libyuv::ARGBToNV12(bgra, width * 4, y, strideY, uv, strideUV, width, height) != 0) {
        TG_LOGW("WinScreenCapture: ARGBToNV12 失败");
        return;
    }

    out->width = width;
    out->height = height;
    out->pixFmt = AV_PIX_FMT_NV12;
    out->planes[0] = buffer;
    out->strides[0] = strideY;
    out->planes[1] = std::shared_ptr<uint8_t>(buffer, uv);   // 别名：共用同一块 buffer
    out->strides[1] = strideUV;
}

}  // namespace TGCapture::platform
