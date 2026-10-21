// FFmpeg 错误码 → 可读字符串
//
// 直接用 av_err2str 宏在 MSVC 下编译不过（它是 compound literal，C++ 不支持），
// 这里手写等价实现：thread_local 缓冲 + av_make_error_string。
//
// 注意：返回的是同一个线程的静态缓冲，**一条日志里只能用一次 AvErr()**，
// 连续用两次后者会覆盖前者。
#ifndef TGCAPTURE_CORE_AV_ERROR_H
#define TGCAPTURE_CORE_AV_ERROR_H

extern "C"
{
#include <libavutil/error.h>
}

namespace TGCapture::core
{

inline const char* AvErr(int errnum) noexcept
{
    thread_local char buffer[AV_ERROR_MAX_STRING_SIZE];
    buffer[0] = '\0';
    return av_make_error_string(buffer, AV_ERROR_MAX_STRING_SIZE, errnum);
}

}  // namespace TGCapture::core

#endif  // TGCAPTURE_CORE_AV_ERROR_H
