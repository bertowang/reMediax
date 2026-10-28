#include "LiveConfig.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>

#include "StringUtil.h"

namespace tgui
{

namespace
{

std::wstring ToLower(std::wstring_view s)
{
    std::wstring out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return out;
}

bool EndsWith(std::wstring_view s, std::wstring_view suffix)
{
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

}  // namespace

bool IsNetworkUrl(std::wstring_view url)
{
    return url.find(L"://") != std::wstring_view::npos;
}

std::string GuessMuxer(std::wstring_view url)
{
    const std::wstring lower = ToLower(url);
    if (lower.rfind(L"srt://", 0) == 0 || lower.rfind(L"udp://", 0) == 0) {
        return "mpegts";
    }
    // 去掉查询串再看扩展名
    const std::wstring path = lower.substr(0, lower.find(L'?'));
    if (EndsWith(path, L".ts") || EndsWith(path, L".m2ts")) {
        return "mpegts";
    }
    return "flv";
}

std::wstring ExpandOutput(const std::wstring& text, const std::wstring& baseDir)
{
    std::wstring out = text;
    const size_t first = out.find_first_not_of(L" \t");
    const size_t last = out.find_last_not_of(L" \t");
    out = (first == std::wstring::npos) ? std::wstring() : out.substr(first, last - first + 1);

    SYSTEMTIME st{};
    GetLocalTime(&st);
    const std::wstring stamp =
        Format(L"%04u%02u%02u_%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    for (size_t pos = out.find(L"{time}"); pos != std::wstring::npos; pos = out.find(L"{time}", pos)) {
        out.replace(pos, 6, stamp);
        pos += stamp.size();
    }

    if (out.empty() || IsNetworkUrl(out)) {
        return out;
    }
    std::filesystem::path p(out);
    if (p.is_relative()) {
        p = std::filesystem::path(baseDir) / p;
    }
    return p.lexically_normal().wstring();
}

}  // namespace tgui
