// Windows 平台层实测：直接驱动真实的 WASAPI / GDI / DXGI 采集源，验证 CHANGELOG 里
// "平台层失败路径（需在目标平台复测）"那几条修复。只在 Windows 编译。
//
// 方法：同一个对象反复 Open → (Start/Stop) → Close N 轮，在固定状态点（Close 之后）取
// 进程句柄数 / GDI 对象数 / 私有内存。任何"每轮漏一点"的资源都会表现为随轮数线性增长。
// 比较的是"预热一轮之后"的基线：首轮会有系统 DLL、音频服务连接、驱动等一次性开销。
#if defined(_WIN32)

#include <chrono>
#include <string>
#include <thread>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <psapi.h>

#include "platform/windows/WinAudioCapture.h"
#include "platform/windows/WinScreenCapture.h"
#include "tg_test.h"

using namespace TGCapture;
using namespace std::chrono_literals;

namespace
{

struct Res
{
    long handles = 0;
    long gdi = 0;
    double privMB = 0;
};

Res Snapshot()
{
    Res r;
    DWORD h = 0;
    GetProcessHandleCount(GetCurrentProcess(), &h);
    r.handles = static_cast<long>(h);
    r.gdi = static_cast<long>(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS));
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
    r.privMB = static_cast<double>(pmc.PrivateUsage) / (1024.0 * 1024.0);
    return r;
}

void PrintRes(const char* tag, int round, const Res& r)
{
    std::printf("    %-22s round=%2d handles=%ld gdi=%ld privMB=%.1f\n", tag, round, r.handles, r.gdi, r.privMB);
}

// 读当前线程的 COM apartment 引用计数是否仍然"干净"：
// CoInitializeEx 成功(S_OK) 说明本线程此前未初始化 COM（计数为 0）。
// 测完立刻配对 CoUninitialize，不改变线程状态。
bool ComIsUninitializedOnThisThread()
{
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) {
        CoUninitialize();
    }
    return hr == S_OK;
}

bool HasDefaultEndpoint(bool capture)
{
    for (const auto& d : platform::ListAudioDevices(capture)) {
        if (d.isDefault) {
            return true;
        }
    }
    return false;
}

}  // namespace

TG_TEST(WinPlatform, WasapiOpenCloseCyclesDoNotLeak)
{
    // 回归：InitWASAPI 各失败分支 / Close 都要走 ReleaseEndpoint。
    // 之前每轮泄漏一个 IMMDevice + 一个已 Initialize 的 IAudioClient（还占着端点）。
    if (!HasDefaultEndpoint(true)) {
        std::printf("    SKIP: 无默认麦克风端点\n");
        return;
    }
    constexpr int kRounds = 12;
    Res base;
    Res last;
    int opened = 0;
    for (int r = 0; r < kRounds; ++r) {
        platform::WinAudioCapture cap(TypeOfSelfMicphone, false);
        if (cap.Open(44100, 2) == 0) {
            ++opened;
        }
        cap.Close();
        cap.Close();   // 幂等：重复 Close 不应二次 Release / 二次 CoUninitialize
        last = Snapshot();
        if (r == 1) {
            base = last;   // 第 0 轮含音频服务连接等一次性开销，从第 1 轮起算
        }
    }
    PrintRes("wasapi base", 1, base);
    PrintRes("wasapi last", kRounds - 1, last);
    TG_EXPECT_EQ(opened, kRounds);
    // 每轮漏一个 IAudioClient 至少带出若干内核句柄（事件、共享段），10 轮必然可见
    TG_EXPECT(last.handles - base.handles <= 8);
    TG_EXPECT(last.privMB - base.privMB < 8.0);
}

TG_TEST(WinPlatform, WasapiReopenSameObjectAfterClose)
{
    // Close 之后同一个对象再 Open 必须走完整 InitWASAPI（Stop → 重新 Prepare 的平台侧语义）
    if (!HasDefaultEndpoint(true)) {
        std::printf("    SKIP: 无默认麦克风端点\n");
        return;
    }
    platform::WinAudioCapture cap(TypeOfSelfMicphone, false);
    // 不挂 sink 的话 Emit 直接丢帧、不计数（基类约定），这里用一个空 sink 让计数生效
    cap.SetEmitCallback([](core::FramePtr) {});
    Res base;
    Res last;
    for (int r = 0; r < 8; ++r) {
        TG_ASSERT(cap.Open(44100, 2) == 0);
        TG_ASSERT(cap.StartCapture());
        std::this_thread::sleep_for(60ms);
        cap.StopCapture();
        cap.Close();
        last = Snapshot();
        if (r == 1) {
            base = last;
        }
    }
    PrintRes("wasapi reopen base", 1, base);
    PrintRes("wasapi reopen last", 7, last);
    TG_EXPECT(cap.EmittedFrames() > 0);
    TG_EXPECT(last.handles - base.handles <= 8);
}

TG_TEST(WinPlatform, WasapiBadDeviceIdFallsBackAndReleases)
{
    // 失败分支：指定一个不存在的端点 id → GetDevice 失败 → 回落默认设备。
    // 走的是 InitWASAPI 里"拿不到 device 时先 Release 再取默认"的那条路径
    if (!HasDefaultEndpoint(true)) {
        std::printf("    SKIP: 无默认麦克风端点\n");
        return;
    }
    Res base;
    Res last;
    for (int r = 0; r < 10; ++r) {
        platform::WinAudioCapture cap(TypeOfSelfMicphone, false);
        cap.SetDeviceId(L"{0.0.1.00000000}.{deadbeef-0000-0000-0000-000000000000}");
        TG_EXPECT_EQ(cap.Open(44100, 2), 0);   // 回落默认设备，仍然成功
        cap.Close();
        last = Snapshot();
        if (r == 1) {
            base = last;
        }
    }
    PrintRes("wasapi badid base", 1, base);
    PrintRes("wasapi badid last", 9, last);
    TG_EXPECT(last.handles - base.handles <= 8);
}

TG_TEST(WinPlatform, WasapiLoopbackOpenCloseCycles)
{
    if (!HasDefaultEndpoint(false)) {
        std::printf("    SKIP: 无默认扬声器端点\n");
        return;
    }
    Res base;
    Res last;
    for (int r = 0; r < 10; ++r) {
        platform::WinAudioCapture cap(TypeOfSpeaker, true);
        TG_EXPECT_EQ(cap.Open(44100, 2), 0);
        cap.Close();
        last = Snapshot();
        if (r == 1) {
            base = last;
        }
    }
    PrintRes("loopback base", 1, base);
    PrintRes("loopback last", 9, last);
    TG_EXPECT(last.handles - base.handles <= 8);
}

TG_TEST(WinPlatform, WasapiDoesNotUnbalanceHostCom)
{
    // 回归：RPC_E_CHANGED_MODE 不能记成"COM 初始化成功"。
    // 宿主线程先以 STA 初始化 COM，再在同一线程上 Open/Close 采集对象（MTA 请求会得到
    // RPC_E_CHANGED_MODE）。修复前 Close 会多调一次 CoUninitialize，把宿主的 STA 拆掉。
    std::thread host([] {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        TG_ASSERT(SUCCEEDED(hr));
        {
            platform::WinAudioCapture cap(TypeOfSelfMicphone, false);
            cap.Open(44100, 2);   // 有无设备都要走到 InitWASAPI 的 COM 记账
            cap.Close();
        }
        // 宿主的 STA 必须还在：再以 STA 请求应得到 S_FALSE（已初始化、同模式）
        const HRESULT again = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        TG_EXPECT(again == S_FALSE);
        if (SUCCEEDED(again)) {
            CoUninitialize();
        }
        CoUninitialize();   // 配对宿主自己的那次
        // 全部配平后本线程回到"未初始化"
        TG_EXPECT(ComIsUninitializedOnThisThread());
    });
    host.join();
}

TG_TEST(WinPlatform, WasapiOwnComIsBalanced)
{
    // 采集对象在"干净线程"上自己初始化 COM：Close 后必须配平，线程回到未初始化
    std::thread worker([] {
        TG_ASSERT(ComIsUninitializedOnThisThread());
        for (int r = 0; r < 3; ++r) {
            platform::WinAudioCapture cap(TypeOfSelfMicphone, false);
            cap.Open(44100, 2);
            cap.Close();
            TG_EXPECT(ComIsUninitializedOnThisThread());
        }
    });
    worker.join();
}

TG_TEST(WinPlatform, GdiScreenOpenCloseReleasesDib)
{
    // 回归：DIB 处于选中状态时 DeleteObject 失败，4K 下每个采集对象漏 ~33MB + 若干 GDI 对象
    Res base;
    Res last;
    Res peak;
    for (int r = 0; r < 10; ++r) {
        platform::WinScreenCapture cap(0, GDI, VideoScaleStrategy::SCALE_FIT_V0);
        TG_ASSERT(cap.Open(1280, 720, 30.0f) == 0);
        const Res during = Snapshot();
        if (during.gdi > peak.gdi) {
            peak = during;
        }
        cap.Close();
        last = Snapshot();
        if (r == 1) {
            base = last;
        }
    }
    PrintRes("gdi open(peak)", 0, peak);
    PrintRes("gdi base", 1, base);
    PrintRes("gdi last", 9, last);
    // 打开期间确实建了 GDI 对象（证明这个指标测得到东西，不是恒为 0）
    TG_EXPECT(peak.gdi >= 2);
    TG_EXPECT_EQ(last.gdi, base.gdi);
    TG_EXPECT(last.privMB - base.privMB < 16.0);   // 漏一张 4K DIB 就是 33MB
}

TG_TEST(WinPlatform, GdiSameObjectReopen)
{
    // 同一对象 Close → Open：InitGdi 要能补建（m_bmpBits 已在 ReleaseGdi 里置空）
    platform::WinScreenCapture cap(0, GDI, VideoScaleStrategy::SCALE_FIT_V0);
    cap.SetEmitCallback([](core::FramePtr) {});
    Res base;
    Res last;
    for (int r = 0; r < 6; ++r) {
        TG_ASSERT(cap.Open(1280, 720, 30.0f) == 0);
        TG_ASSERT(cap.StartCapture());
        std::this_thread::sleep_for(120ms);
        cap.StopCapture();
        cap.Close();
        last = Snapshot();
        if (r == 1) {
            base = last;
        }
    }
    PrintRes("gdi reopen base", 1, base);
    PrintRes("gdi reopen last", 5, last);
    TG_EXPECT(cap.EmittedFrames() > 0);
    TG_EXPECT_EQ(last.gdi, base.gdi);
    TG_EXPECT(last.handles - base.handles <= 4);
}

TG_TEST(WinPlatform, DxgiScreenOpenCloseCycles)
{
    Res base;
    Res last;
    int dxgi = 0;
    for (int r = 0; r < 8; ++r) {
        platform::WinScreenCapture cap(0, DXGI, VideoScaleStrategy::SCALE_FIT_V0);
        TG_ASSERT(cap.Open(1280, 720, 30.0f) == 0);
        if (cap.ActiveMode() == DXGI) {
            ++dxgi;
        }
        cap.Close();
        last = Snapshot();
        if (r == 1) {
            base = last;
        }
    }
    PrintRes("dxgi base", 1, base);
    PrintRes("dxgi last", 7, last);
    std::printf("    dxgi 实际生效 %d/8 轮（其余回落 GDI）\n", dxgi);
    TG_EXPECT(last.handles - base.handles <= 6);
    TG_EXPECT(last.privMB - base.privMB < 16.0);
}

TG_TEST(WinPlatform, DxgiSameObjectReopenProducesFrames)
{
    platform::WinScreenCapture cap(0, DXGI, VideoScaleStrategy::SCALE_FIT_V0);
    cap.SetEmitCallback([](core::FramePtr) {});
    Res base;
    Res last;
    for (int r = 0; r < 5; ++r) {
        TG_ASSERT(cap.Open(1280, 720, 30.0f) == 0);
        TG_ASSERT(cap.StartCapture());
        std::this_thread::sleep_for(150ms);
        cap.StopCapture();
        cap.Close();
        last = Snapshot();
        if (r == 1) {
            base = last;
        }
    }
    PrintRes("dxgi reopen base", 1, base);
    PrintRes("dxgi reopen last", 4, last);
    TG_EXPECT(cap.EmittedFrames() > 0);
    TG_EXPECT(last.handles - base.handles <= 6);
}

TG_TEST(WinPlatform, WindowCaptureRejectsGpuDirect)
{
    // 契约：窗口采集不支持 GPU 直连（Desktop Duplication 只能整屏）
    platform::WinScreenCapture win(GetDesktopWindow(), WGC, VideoScaleStrategy::SCALE_FIT_V0);
    TG_EXPECT(!win.SupportsGpuDirect());
    platform::WinScreenCapture screen(0, DXGI, VideoScaleStrategy::SCALE_FIT_V0);
    TG_EXPECT(screen.SupportsGpuDirect());
}

TG_TEST(WinPlatform, InvalidWindowOpenFailsCleanly)
{
    // 失败分支：已销毁的窗口句柄 → Open 返回错误，且不残留 GDI 对象
    const Res before = Snapshot();
    for (int r = 0; r < 5; ++r) {
        platform::WinScreenCapture cap(reinterpret_cast<HWND>(static_cast<uintptr_t>(0x7FFFFFF0)), WGC,
                                       VideoScaleStrategy::SCALE_FIT_V0);
        TG_EXPECT(cap.Open(1280, 720, 30.0f) != 0);
        cap.Close();
    }
    const Res after = Snapshot();
    PrintRes("badwnd before", 0, before);
    PrintRes("badwnd after", 4, after);
    TG_EXPECT_EQ(after.gdi, before.gdi);
}

#endif  // _WIN32
