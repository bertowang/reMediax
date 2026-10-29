// 文件视频源：本地视频文件（mp4 / mov / flv，H.264）按文件时间戳以实时节奏出 NV12 帧。
//   - 两种用法：CreateLocalFileCapture 的主画面（vsrc.main），或视频叠加层（vsrc.ovl.*，接合成器的叠加输入口）；
//   - 按原生尺寸出帧（偶数对齐），缩放交给编码器 / 合成器。每帧都是新缓冲（合成器会就地改写主画面像素）；
//   - loop = true 播完无缝从头再来（时间轴按上一遍的时长接续，不跳不停）；
//     loop = false 播完回调一次 EOF，之后按 Open 给的帧率重复最后一帧（时间轴不断，不触发断流看护）；
//   - Close 之后再 StartCapture 会在采集线程里重新打开文件、从头播放（preview 组关闭再打开时用到）。
// 跨平台纯 ffmpeg 实现；ts 需要 ffmpeg 编进 mpegts 解封装器（当前开播 profile 未编）。
#ifndef TGCAPTURE_CAPTURE_FILE_VIDEO_SOURCE_H
#define TGCAPTURE_CAPTURE_FILE_VIDEO_SOURCE_H

#include <atomic>
#include <functional>
#include <string>

#include "capture/Source.h"

struct AVFormatContext;
struct AVCodecContext;
struct SwsContext;
struct AVPacket;
struct AVFrame;

namespace TGCapture::capture
{

class FileVideoSource : public VideoSourceBase
{
public:
    struct Info
    {
        int width = 0;          // 偶数对齐后的出帧尺寸
        int height = 0;
        int64_t durationUs = 0;
        double fps = 0.0;       // 文件标称帧率（未知为 0）
    };

    FileVideoSource(std::string path, bool loop);
    ~FileVideoSource() override;

    // 只检查文件能否打开、有没有可解码的视频轨（不保留句柄）。返回 0 或 TGErr_*
    static int Probe(const std::string& path, Info* info = nullptr, std::string* error = nullptr);

    // width / height 忽略（按原生尺寸出帧）；fps 只用于播完后重复最后一帧的节拍
    int Open(int width, int height, float fps) override;
    void Close() override;

    // 播放到结尾（loop = false）时回调一次，在源线程上执行
    void SetEofCallback(std::function<void()> cb) { m_onEof = std::move(cb); }

    [[nodiscard]] const std::string& Path() const noexcept { return m_path; }
    [[nodiscard]] bool Looping() const noexcept { return m_loop; }
    [[nodiscard]] int64_t DurationUs() const noexcept { return m_durationUs; }
    [[nodiscard]] uint32_t Loops() const noexcept { return m_loops.load(); }
    [[nodiscard]] bool Ended() const noexcept { return m_ended.load(); }

protected:
    void Loop(std::stop_token st) override;

private:
    int OpenFile();
    void CloseFile();
    // 解出下一帧（显示顺序）到 m_frame；到文件末尾（解码器已吐完）返回 false
    bool DecodeNext();
    bool Rewind();
    // 转成 NV12 新缓冲
    core::FramePtr Convert(const AVFrame* src);
    // 按计划时刻睡到 due；期间要求停止返回 false
    bool SleepUntil(int64_t due, const std::stop_token& st);

    std::string m_path;
    bool m_loop = false;
    std::function<void()> m_onEof;

    AVFormatContext* m_fmt = nullptr;
    AVCodecContext* m_dec = nullptr;
    SwsContext* m_sws = nullptr;
    int m_swsW = 0, m_swsH = 0, m_swsFmt = -1;   // m_sws 对应的源参数（格式已规整，含全/限幅标志）
    bool m_swsFull = false;
    AVPacket* m_pkt = nullptr;
    AVFrame* m_frame = nullptr;
    AVFrame* m_last = nullptr;   // 最近一帧的引用：播完后据此重复出帧（不能用已发出的帧，合成器改写过它的像素）
    int m_stream = -1;
    bool m_decoderDrained = false;
    int64_t m_durationUs = 0;
    int64_t m_frameDurUs = 33333;   // 标称帧间隔：时间戳缺失 / 一遍的时长按它补

    std::atomic<uint32_t> m_loops{0};
    std::atomic_bool m_ended{false};
};

}  // namespace TGCapture::capture

#endif  // TGCAPTURE_CAPTURE_FILE_VIDEO_SOURCE_H
