// 文件音频源（AudioSourceKind::File）：本地媒体文件（mp4 / mov / m4a / flv，AAC）的音轨按实时节奏送进混音器。
//   - 解封装 + 解码 + 重采样到 Open 给定的格式（S16 交错），每 10ms 一块，按墙钟节拍 Emit；
//   - loop = true 播完从头再来（无缝：重新 seek 到开头继续解码，不插静音）；
//     loop = false 播完通知一次 EOF（EsCap_Notify_LocalFile_EOF），之后按节拍输出静音（时间轴不断，
//     不触发断流看护），宿主可在收到通知后 RemoveAudioSource；
//   - 文件在 Open 时打开并校验（不存在 / 无音轨 / 解码器缺失 → Open 失败，AddAudioSource 据此返回错误）。
// 跨平台纯 ffmpeg 实现，三端共用；ts 需要 ffmpeg 编进 mpegts 解封装器（当前开播 profile 未编）。
#ifndef TGCAPTURE_CAPTURE_FILE_AUDIO_SOURCE_H
#define TGCAPTURE_CAPTURE_FILE_AUDIO_SOURCE_H

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "capture/Source.h"

struct AVFormatContext;
struct AVCodecContext;
struct SwrContext;
struct AVPacket;
struct AVFrame;

namespace TGCapture::capture
{

class FileAudioSource : public AudioSourceBase
{
public:
    FileAudioSource(std::string path, bool loop);
    ~FileAudioSource() override;

    // 只检查文件能否打开、有没有可解码的音轨（不保留句柄）。返回 0 或 TGErr_*
    static int Probe(const std::string& path, std::string* error = nullptr);

    int Open(int sampleRate, int channels) override;
    void Close() override;

    // 播放到结尾（loop = false）时回调一次，在源线程上执行
    void SetEofCallback(std::function<void()> cb) { m_onEof = std::move(cb); }

    [[nodiscard]] const std::string& Path() const noexcept { return m_path; }
    [[nodiscard]] bool Loop() const noexcept { return m_loop; }
    [[nodiscard]] int64_t DurationUs() const noexcept { return m_durationUs; }
    [[nodiscard]] uint32_t Loops() const noexcept { return m_loops.load(); }
    [[nodiscard]] bool Ended() const noexcept { return m_ended.load(); }

protected:
    void Loop(std::stop_token st) override;

private:
    int OpenFile();
    void CloseFile();
    // 从文件解码出至少 samples 个输出样本（每声道）追加到 m_pcm；到结尾返回 false
    bool Fill(size_t samples);
    bool Rewind();

    std::string m_path;
    bool m_loop = true;
    std::function<void()> m_onEof;

    AVFormatContext* m_fmt = nullptr;
    AVCodecContext* m_dec = nullptr;
    SwrContext* m_swr = nullptr;
    AVPacket* m_pkt = nullptr;
    AVFrame* m_frame = nullptr;
    int m_stream = -1;
    bool m_decoderDrained = false;
    int64_t m_durationUs = 0;

    std::vector<int16_t> m_pcm;   // 已解码、待送出的交错样本
    std::atomic<uint32_t> m_loops{0};
    std::atomic_bool m_ended{false};
};

}  // namespace TGCapture::capture

#endif  // TGCAPTURE_CAPTURE_FILE_AUDIO_SOURCE_H
