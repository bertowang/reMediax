// 宽字符（Win32 UI）与 UTF-8（TGCapture SDK）互转
#ifndef TGUI_STRING_UTIL_H
#define TGUI_STRING_UTIL_H

#include <windows.h>

#include <string>
#include <string_view>

namespace tgui
{

inline std::wstring Utf8ToWide(std::string_view s)
{
    if (s.empty()) {
        return {};
    }
    const int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len);
    return out;
}

inline std::string WideToUtf8(std::wstring_view s)
{
    if (s.empty()) {
        return {};
    }
    const int len = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len, nullptr, nullptr);
    return out;
}

// printf 风格格式化为 std::wstring（界面文案拼接用）
template <typename... Args>
std::wstring Format(const wchar_t* fmt, Args... args)
{
    wchar_t buf[1024];
    swprintf_s(buf, fmt, args...);
    return buf;
}

}  // namespace tgui

#endif  // TGUI_STRING_UTIL_H
