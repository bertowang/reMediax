// TGCapture Windows 开播测试工具：程序入口
#include <windows.h>
#include <commctrl.h>
#include <objbase.h>

#include "MainDialog.h"

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int)
{
    // 界面线程用 STA：文件对话框 / Shell 接口要求；WASAPI 设备枚举在 STA 下同样可用
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icc);

    tgui::MainDialog dialog;
    const int ret = dialog.Run(instance);

    if (SUCCEEDED(hr)) {
        CoUninitialize();
    }
    return ret;
}
