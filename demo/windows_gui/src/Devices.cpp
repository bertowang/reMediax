#include "Devices.h"

#include <windows.h>
#include <dwmapi.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
// PKEY_Device_FriendlyName 需要在本编译单元实例化 GUID
#include <initguid.h>
#include <functiondiscoverykeys_devpkey.h>

#include "StringUtil.h"

using Microsoft::WRL::ComPtr;

namespace tgui
{

// ------------------------------------------------------------------ 显示器
std::vector<MonitorInfo> EnumerateMonitors()
{
    std::vector<MonitorInfo> out;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR monitor, HDC, LPRECT, LPARAM lparam) -> BOOL {
            auto* list = reinterpret_cast<std::vector<MonitorInfo>*>(lparam);
            MONITORINFOEXW info{};
            info.cbSize = sizeof(info);
            GetMonitorInfoW(monitor, &info);
            const int w = info.rcMonitor.right - info.rcMonitor.left;
            const int h = info.rcMonitor.bottom - info.rcMonitor.top;
            const bool primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
            list->push_back({Format(L"显示器 %zu   %dx%d%ls", list->size() + 1, w, h, primary ? L"（主显示器）" : L"")});
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&out));
    return out;
}

// ------------------------------------------------------------------ 窗口
namespace
{

std::wstring ProcessNameOf(HWND hwnd)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        return {};
    }
    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    std::wstring name;
    if (QueryFullProcessImageNameW(process, 0, path, &size)) {
        name = path;
        const size_t slash = name.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            name = name.substr(slash + 1);
        }
    }
    CloseHandle(process);
    return name;
}

bool IsCapturableWindow(HWND hwnd, HWND exclude)
{
    if (hwnd == exclude || hwnd == GetShellWindow() || !IsWindowVisible(hwnd)) {
        return false;
    }
    if (GetAncestor(hwnd, GA_ROOT) != hwnd || GetWindowTextLengthW(hwnd) == 0) {
        return false;
    }
    const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_TOOLWINDOW) {
        return false;
    }
    // UWP 挂起 / 其它虚拟桌面上的窗口会被 DWM "隐身"，采不到画面
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0) {
        return false;
    }
    return true;
}

struct EnumWindowsCtx
{
    HWND exclude = nullptr;
    std::vector<WindowInfo>* out = nullptr;
};

}  // namespace

std::vector<WindowInfo> EnumerateWindows(HWND exclude)
{
    std::vector<WindowInfo> out;
    EnumWindowsCtx ctx{exclude, &out};
    EnumWindows(
        [](HWND hwnd, LPARAM lparam) -> BOOL {
            auto* c = reinterpret_cast<EnumWindowsCtx*>(lparam);
            if (!IsCapturableWindow(hwnd, c->exclude)) {
                return TRUE;
            }
            const int len = GetWindowTextLengthW(hwnd);
            std::wstring title(static_cast<size_t>(len) + 1, L'\0');
            GetWindowTextW(hwnd, title.data(), len + 1);
            title.resize(static_cast<size_t>(len));
            const std::wstring process = ProcessNameOf(hwnd);
            WindowInfo info;
            info.hwnd = hwnd;
            info.label = process.empty() ? title : title + L"  —  " + process;
            c->out->push_back(std::move(info));
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return out;
}

// ------------------------------------------------------------------ 音频端点
std::vector<AudioDevice> EnumerateAudioDevices(bool capture)
{
    std::vector<AudioDevice> out;
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) {
        return out;
    }
    const EDataFlow flow = capture ? eCapture : eRender;

    std::wstring defaultId;
    ComPtr<IMMDevice> defaultDevice;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(flow, eConsole, &defaultDevice))) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(defaultDevice->GetId(&id)) && id) {
            defaultId = id;
            CoTaskMemFree(id);
        }
    }

    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection))) {
        return out;
    }
    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, &device))) {
            continue;
        }
        AudioDevice dev;
        LPWSTR id = nullptr;
        if (FAILED(device->GetId(&id)) || !id) {
            continue;
        }
        dev.id = id;
        CoTaskMemFree(id);

        ComPtr<IPropertyStore> props;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT name;
            PropVariantInit(&name);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &name)) && name.vt == VT_LPWSTR) {
                dev.name = name.pwszVal;
            }
            PropVariantClear(&name);
        }
        if (dev.name.empty()) {
            dev.name = dev.id;
        }
        dev.isDefault = (dev.id == defaultId);
        out.push_back(std::move(dev));
    }
    return out;
}

}  // namespace tgui
