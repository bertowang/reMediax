// 封装推流：flv(rtmp) / mpegts(srt) / 本地文件，音视频交织写出
#ifndef TGCAPTURE_MUX_MUXER_H
#define TGCAPTURE_MUX_MUXER_H

#include <atomic>
#include <memory>
#include <string>

#include "TGCapture/TGCapturerInterface.h"
#include "core/Frame.h"
#include "core/Queue.h"
#include "core/Thread.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace TGCapture
{

class Muxer : public core::Worker
{
public:
    explicit Muxer(size_t queueCapacity = 128);
    ~Muxer() override;

    int Open(const MuxerParams& params, AVCodecContext* videoCtx, AVCodecContext* audioCtx);
    void Close();

    void SubmitVideoPacket(core::AVPacketPtr packet);
    void SubmitAudioPacket(core::AVPacketPtr packet);

    [[nodiscard]] size_t VideoPending() const;
    [[nodiscard]] size_t AudioPending() const;
    [[nodiscard]] bool IsOpened() const noexcept { return m_opened.load(); }

    // 音视频交织阈值（微秒）：领先的一方暂停写入，等落后的一方追上，0 = 关闭
    void SetInterleaveUs(int64_t us) noexcept { m_interleaveUs.store(us > 0 ? us : 0); }
    [[nodiscard]] int64_t InterleaveUs() const noexcept { return m_interleaveUs.load(); }

    [[nodiscard]] float TrafficBps() const;
    [[nodiscard]] uint64_t WrittenBytes() const noexcept { return m_bytes.load(); }
    [[nodiscard]] uint64_t WrittenPackets() const noexcept { return m_packets.load(); }
    [[nodiscard]] std::string Stats() const;

protected:
    void Loop(std::stop_token token) override;

private:
    int WriteOne(core::AVPacketPtr packet, bool isVideo);
    void UpdateBitrate(size_t bytes);

private:
    AVFormatContext* m_fmt = nullptr;
    AVStream* m_videoStream = nullptr;
    AVStream* m_audioStream = nullptr;
    std::string m_url;
    std::string m_format;

    std::unique_ptr<core::BoundedQueue<core::AVPacketPtr>> m_videoQueue;
    std::unique_ptr<core::BoundedQueue<core::AVPacketPtr>> m_audioQueue;

    std::atomic<uint64_t> m_bytes{0};
    std::atomic<uint64_t> m_packets{0};
    std::atomic<float> m_bps{0.0f};
    std::atomic_bool m_opened{false};
    std::atomic_bool m_headerWritten{false};

    // 交织：记录两路各自最后写出的 PTS（微秒）
    std::atomic<int64_t> m_lastVideoPtsUs{INT64_MIN / 4};
    std::atomic<int64_t> m_lastAudioPtsUs{INT64_MIN / 4};
    std::atomic<int64_t> m_lastVideoArrivalUs{0};
    std::atomic<int64_t> m_lastAudioArrivalUs{0};
    std::atomic<int64_t> m_interleaveUs{350000};
    std::atomic<uint64_t> m_diagStallVideo{0};
    std::atomic<uint64_t> m_diagStallAudio{0};
    int64_t m_lastStatUs = 0;
};

}  // namespace TGCapture

#endif  // TGCAPTURE_MUX_MUXER_H
