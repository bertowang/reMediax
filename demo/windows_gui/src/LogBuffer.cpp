#include "LogBuffer.h"

#include <windows.h>

#include "StringUtil.h"

namespace tgui
{

namespace
{
// 积压上限：界面每 200ms 取一次，正常远到不了；Debug 档刷屏时只保留最新的部分
constexpr size_t kMaxPendingChars = 512 * 1024;
}  // namespace

LogBuffer& LogBuffer::Instance()
{
    static LogBuffer instance;
    return instance;
}

void LogBuffer::Push(wchar_t level, std::wstring_view msg)
{
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t prefix[32];
    swprintf_s(prefix, L"%02u:%02u:%02u.%03u [%c] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, level);

    std::wstring line(prefix);
    line.append(msg);
    while (!line.empty() && (line.back() == L'\n' || line.back() == L'\r')) {
        line.pop_back();
    }
    line.append(L"\r\n");

    OutputDebugStringW(line.c_str());

    std::lock_guard<std::mutex> lock(m_mutex);
    m_pending.append(line);
    if (m_pending.size() > kMaxPendingChars) {
        size_t cut = m_pending.size() - kMaxPendingChars / 2;
        const size_t nl = m_pending.find(L'\n', cut);
        cut = (nl == std::wstring::npos) ? cut : nl + 1;
        m_pending.erase(0, cut);
        m_droppedChars += cut;
    }
}

std::wstring LogBuffer::Drain()
{
    std::wstring out;
    size_t dropped = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        out.swap(m_pending);
        dropped = m_droppedChars;
        m_droppedChars = 0;
    }
    if (dropped > 0) {
        out.insert(0, Format(L"…（日志过多，界面省略了 %zu 个字符，完整内容见日志文件）\r\n", dropped));
    }
    return out;
}

}  // namespace tgui
