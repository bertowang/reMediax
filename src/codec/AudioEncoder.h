// 音频编码器：封装 ffmpeg avcodec（libfdk_aac / aac / aac_at）
#ifndef TGCAPTURE_CODEC_AUDIO_ENCODER_H
#define TGCAPTURE_CODEC_AUDIO_ENCODER_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "TGCapture/TGCapturerInterface.h"
#include "core/Bus.h"
#include "core/Clock.h"
#include "core/Frame.h"
#include "core/MediaTypes.h"
#include "core/Queue.h"
#include "core/Thread.h"

extern "C"
{
#include <libavcodec/avcodec.h>
}

namespace TGCapture
{
namespace codec
{

class AudioEncoder : public core::BusNode, public core::Worker
{
public:
    explicit AudioEncoder(size_t queueCapacity = 64);
    ~AudioEncoder() override;

    int Open(const AudioEncoderParams& params, const core::AudioMediaType& srcType);
    void Close();

    void SetSyncClock(std::shared_ptr<core::AVSync> sync) { m_sync = std::move(sync); }

    // BusNode：编码后的包经 Forward 交给链上的下一个节点（muxer）
    bool Submit(core::FramePtr frame) override;
    [[nodiscard]] size_t Pending() const override;
    bool OnCommand(const core::BusCommand& command) override;   // 支持 Flush

    [[nodiscard]] AVCodecContext* CodecContext() const noexcept { return m_ctx; }
    [[nodiscard]] bool IsOpened() const noexcept { return m_ctx != nullptr; }
    [[nodiscard]] uint64_t EncodedFrames() const noexcept { return m_encoded.load(); }
    [[nodiscard]] const std::string& CodecName() const noexcept { return m_codecName; }

    // 端到端编码延迟 EMA（微秒）：capture → encode out
    [[nodiscard]] int64_t EncodeLatencyUs() const noexcept { return m_encodeLatencyUs.load(); }

protected:
    void Loop(std::stop_token token) override;
    void OnThreadExit() override;

private:
    int EncodeOne(const core::FramePtr& frame);
    int Drain();
    void DeliverPacket(core::AVPacketPtr packet);

private:
    std::string m_codecName;
    AVCodecContext* m_ctx = nullptr;

    core::AudioMediaType m_srcType;
    int m_frameSize = 1024;
    // 累积缓冲：把不固定长度的采集数据攒成编码器要求的 frame_size
    std::vector<int16_t> m_pcmBuffer;
    int64_t m_nextPts = 0;

    std::unique_ptr<core::BoundedQueue<core::FramePtr>> m_queue;
    std::shared_ptr<core::AVSync> m_sync;

    std::atomic<uint64_t> m_encoded{0};
    std::atomic<int64_t> m_encodeLatencyUs{0};   // capture → encode out EMA（μs）
    std::atomic_bool m_opened{false};
};

}  // namespace codec
}  // namespace TGCapture

#endif  // TGCAPTURE_CODEC_AUDIO_ENCODER_H
