// 日志中转：内核日志 sink / SDK 回调（任意线程）→ 缓冲 → 界面线程定时取走显示
#ifndef TGUI_LOG_BUFFER_H
#define TGUI_LOG_BUFFER_H

#include <mutex>
#include <string>
#include <string_view>

namespace tgui
{

class LogBuffer
{
public:
    static LogBuffer& Instance();

    // level：'D' / 'I' / 'W' / 'E'，界面自身的事件用 'U'。
    // 线程安全；同时输出到调试器（VS「输出」窗口可见），便于断点调试时对照。
    void Push(wchar_t level, std::wstring_view msg);

    // 取走全部待显示文本（每行以 \r\n 结尾）；没有新日志时返回空串
    std::wstring Drain();

private:
    LogBuffer() = default;

    std::mutex m_mutex;
    std::wstring m_pending;
    size_t m_droppedChars = 0;   // 界面来不及取时丢弃的字符数（只保留最新的）
};

}  // namespace tgui

#endif  // TGUI_LOG_BUFFER_H
