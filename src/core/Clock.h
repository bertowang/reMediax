// 单调时钟与音画同步
#ifndef TGCAPTURE_CORE_CLOCK_H
#define TGCAPTURE_CORE_CLOCK_H

#include <atomic>
#include <chrono>
#include <cstdint>

namespace TGCapture::core
{

class MonotonicClock
{
public:
    // 启动后单调递增的微秒时钟（不受系统时间调整影响）
    static int64_t NowUs() noexcept
    {
        using namespace std::chrono;
        static const steady_clock::time_point s_start = steady_clock::now();
        return duration_cast<microseconds>(steady_clock::now() - s_start).count();
    }
};

// 直播音画同步策略：以音频时钟为主时钟（master），视频向其对齐
//  - 视频落后音频超过 maxLagUs  -> 丢弃（追帧，避免累积延迟）
//  - 视频超前音频超过 maxLeadUs -> 等待（避免抢跑导致音画不同步）
class AVSync
{
public:
    enum class Decision
    {
        Send,   // 立即编码
        Wait,   // 需要等待（返回 WaitDelayUs）
        Drop,   // 落后太多，丢弃该帧
    };

    void Reset() noexcept
    {
        m_audioClockUs.store(kInvalid);
        m_started.store(false);
    }

    // 音频每输出一帧后更新（单位：微秒，基于采集时间轴）
    void UpdateAudioClock(int64_t audioPtsUs) noexcept
    {
        m_audioClockUs.store(audioPtsUs);
        m_started.store(true);
    }

    [[nodiscard]] int64_t AudioClockUs() const noexcept { return m_audioClockUs.load(); }
    [[nodiscard]] bool HasAudioClock() const noexcept { return m_started.load(); }

    Decision Judge(int64_t videoPtsUs, int64_t maxLeadUs, int64_t maxLagUs, int64_t* waitUs = nullptr) const noexcept
    {
        if (waitUs) {
            *waitUs = 0;
        }
        if (!m_started.load() || videoPtsUs < 0) {
            return Decision::Send;  // 音频尚未产生，视频先走
        }
        const int64_t audio = m_audioClockUs.load();
        const int64_t diff = videoPtsUs - audio;  // >0 视频超前

        if (diff > maxLeadUs) {
            if (waitUs) {
                *waitUs = diff - maxLeadUs;
            }
            return Decision::Wait;
        }
        if (diff < -maxLagUs) {
            return Decision::Drop;
        }
        return Decision::Send;
    }

private:
    static constexpr int64_t kInvalid = -1;
    std::atomic_int64_t m_audioClockUs{kInvalid};
    std::atomic_bool m_started{false};
};

}  // namespace TGCapture::core

#endif  // TGCAPTURE_CORE_CLOCK_H
