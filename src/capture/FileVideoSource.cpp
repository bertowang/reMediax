#include "capture/FileVideoSource.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include "core/AvError.h"
#include "core/Clock.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

namespace TGCapture::capture
{

namespace
{
constexpr int64_t kResyncUs = 200000;   // 落后墙钟超过 200ms（线程被挂起 / 解码跟不上）就重新对齐，不追赶
constexpr int64_t kMaxSleepUs = 5000;
constexpr AVRational kUs{1, 1000000};

int64_t NominalFrameDurUs(const AVStream* st, float fallbackFps)
{
    AVRational r = st->avg_frame_rate.num > 0 && st->avg_frame_rate.den > 0 ? st->avg_frame_rate : st->r_frame_rate;
    if (r.num > 0 && r.den > 0) {
        const int64_t d = av_rescale(1000000, r.den, r.num);
        if (d >= 1000 && d <= 1000000) {
            return d;
        }
    }
    return static_cast<int64_t>(1000000.0 / (fallbackFps > 0 ? fallbackFps : 30.0f));
}

// yuvj* 是"全幅 yuv*"的旧写法：sws 会在内部把它改写成 yuv* + 全幅标志，导致 sws_getCachedContext
// 每帧都判定参数不同而重建上下文（并每帧打一次 deprecated 告警）。这里先规整成 yuv* + full 标志
AVPixelFormat NormalizeJpegFormat(AVPixelFormat fmt, bool& full)
{
    switch (fmt) {
        case AV_PIX_FMT_YUVJ420P: full = true; return AV_PIX_FMT_YUV420P;
        case AV_PIX_FMT_YUVJ422P: full = true; return AV_PIX_FMT_YUV422P;
        case AV_PIX_FMT_YUVJ444P: full = true; return AV_PIX_FMT_YUV444P;
        case AV_PIX_FMT_YUVJ440P: full = true; return AV_PIX_FMT_YUV440P;
        case AV_PIX_FMT_YUVJ411P: full = true; return AV_PIX_FMT_YUV411P;
        default: return fmt;
    }
}
}  // namespace

FileVideoSource::FileVideoSource(std::string path, bool loop)
    : VideoSourceBase(TypeOfLocalFile, VideoScaleStrategy::SCALE_FIT_V0), m_path(std::move(path)), m_loop(loop)
{
}

FileVideoSource::~FileVideoSource()
{
    StopCapture();
    CloseFile();
}

int FileVideoSource::Probe(const std::string& path, Info* info, std::string* error)
{
    auto fail = [&](int code, const std::string& why) {
        if (error) {
            *error = why;
        }
        return code;
    };
    AVFormatContext* fmt = nullptr;
    int ret = avformat_open_input(&fmt, path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        return fail(TGErr_InvalidArgument, std::string("打不开文件：") + core::AvErr(ret));
    }
    int code = TGErr_OK;
    std::string why;
    if ((ret = avformat_find_stream_info(fmt, nullptr)) < 0) {
        code = TGErr_InvalidArgument;
        why = std::string("读取流信息失败：") + core::AvErr(ret);
    } else {
        const AVCodec* codec = nullptr;
        const int idx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
        if (idx == AVERROR_STREAM_NOT_FOUND) {
            code = TGErr_InvalidArgument;
            why = "文件里没有视频轨";
        } else if (idx < 0 || !codec) {
            code = TGErr_NotSupported;
            why = "视频轨的解码器未编进（当前只支持 H.264）";
        } else {
            const AVStream* st = fmt->streams[idx];
            if (st->codecpar->width < 2 || st->codecpar->height < 2) {
                code = TGErr_InvalidArgument;
                why = "视频轨尺寸无效";
            } else if (info) {
                info->width = st->codecpar->width & ~1;
                info->height = st->codecpar->height & ~1;
                info->durationUs = st->duration > 0 ? av_rescale_q(st->duration, st->time_base, kUs)
                                                    : (fmt->duration > 0 ? fmt->duration : 0);
                const AVRational r = st->avg_frame_rate;
                info->fps = r.num > 0 && r.den > 0 ? av_q2d(r) : 0.0;
            }
        }
    }
    avformat_close_input(&fmt);
    return code == TGErr_OK ? TGErr_OK : fail(code, why);
}

int FileVideoSource::Open(int /*width*/, int /*height*/, float fps)
{
    m_fps = fps > 0 ? fps : 30.0f;
    CloseFile();   // Close 之后再 Open、或重复 Open：都从头开始
    m_ended.store(false);
    m_loops.store(0);
    return OpenFile();
}

void FileVideoSource::Close()
{
    CloseFile();
}

int FileVideoSource::OpenFile()
{
    int ret = avformat_open_input(&m_fmt, m_path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        TG_LOGE("FileVideoSource: 打不开 %s：%s", m_path.c_str(), core::AvErr(ret));
        m_fmt = nullptr;
        return TGErr_InvalidArgument;
    }
    if ((ret = avformat_find_stream_info(m_fmt, nullptr)) < 0) {
        TG_LOGE("FileVideoSource: 读取流信息失败 %s：%s", m_path.c_str(), core::AvErr(ret));
        CloseFile();
        return TGErr_InvalidArgument;
    }
    const AVCodec* codec = nullptr;
    m_stream = av_find_best_stream(m_fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (m_stream < 0 || !codec) {
        TG_LOGE("FileVideoSource: %s 没有可解码的视频轨（ret=%d）", m_path.c_str(), m_stream);
        CloseFile();
        return m_stream == AVERROR_STREAM_NOT_FOUND ? TGErr_InvalidArgument : TGErr_NotSupported;
    }
    AVStream* st = m_fmt->streams[m_stream];
    // 只读视频轨：其它流的包直接在解封装层丢弃
    for (unsigned i = 0; i < m_fmt->nb_streams; ++i) {
        m_fmt->streams[i]->discard = static_cast<int>(i) == m_stream ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
    }
    m_dec = avcodec_alloc_context3(codec);
    if (!m_dec || avcodec_parameters_to_context(m_dec, st->codecpar) < 0) {
        CloseFile();
        return TGErr_Internal;
    }
    m_dec->thread_count = 0;   // 自动：帧级多线程只增加几帧解码延迟，出帧时刻由时间戳决定，不受影响
    if ((ret = avcodec_open2(m_dec, codec, nullptr)) < 0) {
        TG_LOGE("FileVideoSource: 解码器 %s 打开失败：%s", codec->name, core::AvErr(ret));
        CloseFile();
        return TGErr_NotSupported;
    }
    m_pkt = av_packet_alloc();
    m_frame = av_frame_alloc();
    m_last = av_frame_alloc();
    if (!m_pkt || !m_frame || !m_last) {
        CloseFile();
        return TGErr_Internal;
    }
    m_decoderDrained = false;
    m_width = st->codecpar->width & ~1;
    m_height = st->codecpar->height & ~1;
    m_frameDurUs = NominalFrameDurUs(st, m_fps);
    m_durationUs = st->duration > 0 ? av_rescale_q(st->duration, st->time_base, kUs)
                                    : (m_fmt->duration > 0 ? m_fmt->duration : 0);
    TG_LOGI("FileVideoSource: 打开 %s（%s %dx%d 帧间隔 %.1fms 时长 %.2fs）loop=%d", m_path.c_str(), codec->name,
            m_width, m_height, m_frameDurUs / 1000.0, m_durationUs / 1e6, m_loop ? 1 : 0);
    return TGErr_OK;
}

void FileVideoSource::CloseFile()
{
    sws_freeContext(m_sws);
    m_sws = nullptr;
    av_frame_free(&m_last);
    av_frame_free(&m_frame);
    av_packet_free(&m_pkt);
    avcodec_free_context(&m_dec);
    if (m_fmt) {
        avformat_close_input(&m_fmt);
    }
    m_stream = -1;
}

bool FileVideoSource::Rewind()
{
    const AVStream* st = m_fmt->streams[m_stream];
    const int64_t start = st->start_time != AV_NOPTS_VALUE ? st->start_time : 0;
    if (av_seek_frame(m_fmt, m_stream, start, AVSEEK_FLAG_BACKWARD) >= 0) {
        avcodec_flush_buffers(m_dec);
        m_decoderDrained = false;
        return true;
    }
    // 不支持 seek 的封装（没有索引的 flv 等）：重新打开
    TG_LOGW("FileVideoSource: %s seek 回开头失败，重新打开", m_path.c_str());
    AVFrame* keep = av_frame_clone(m_last);
    CloseFile();
    const bool ok = OpenFile() == TGErr_OK;
    if (ok && keep) {
        av_frame_unref(m_last);
        av_frame_move_ref(m_last, keep);
    }
    av_frame_free(&keep);
    return ok;
}

bool FileVideoSource::DecodeNext()
{
    while (true) {
        int ret = avcodec_receive_frame(m_dec, m_frame);
        if (ret == 0) {
            return true;
        }
        if (ret == AVERROR_EOF) {
            return false;   // 解码器已吐完
        }
        if (ret != AVERROR(EAGAIN)) {
            TG_LOGW("FileVideoSource: 解码失败：%s", core::AvErr(ret));
            return false;
        }
        if (m_decoderDrained) {
            return false;
        }
        ret = av_read_frame(m_fmt, m_pkt);
        if (ret < 0) {
            // 文件读完：冲刷解码器里剩下的帧（B 帧 / 多线程解码的积压）
            avcodec_send_packet(m_dec, nullptr);
            m_decoderDrained = true;
            continue;
        }
        if (m_pkt->stream_index == m_stream) {
            if ((ret = avcodec_send_packet(m_dec, m_pkt)) < 0 && ret != AVERROR(EAGAIN)) {
                TG_LOGW_THROTTLED(3000000, "FileVideoSource: 送包失败：%s（跳过该包）", core::AvErr(ret));
            }
        }
        av_packet_unref(m_pkt);
    }
}

core::FramePtr FileVideoSource::Convert(const AVFrame* src)
{
    const int w = src->width & ~1;
    const int h = src->height & ~1;
    if (w < 2 || h < 2) {
        return nullptr;
    }
    bool full = src->color_range == AVCOL_RANGE_JPEG;
    const AVPixelFormat fmt = NormalizeJpegFormat(static_cast<AVPixelFormat>(src->format), full);
    if (!m_sws || m_swsW != src->width || m_swsH != src->height || m_swsFmt != fmt || m_swsFull != full) {
        sws_freeContext(m_sws);
        m_sws = sws_getContext(src->width, src->height, fmt, w, h, AV_PIX_FMT_NV12, SWS_BILINEAR, nullptr, nullptr,
                               nullptr);
        if (m_sws) {
            // 输出一律限幅（编码 / 合成都按 BT.601 限幅处理）；全幅源在这里压到 16~235
            const int* coeffs = sws_getCoefficients(SWS_CS_DEFAULT);
            sws_setColorspaceDetails(m_sws, coeffs, full ? 1 : 0, coeffs, 0, 0, 1 << 16, 1 << 16);
        }
        m_swsW = src->width;
        m_swsH = src->height;
        m_swsFmt = fmt;
        m_swsFull = full;
    }
    if (!m_sws) {
        TG_LOGW_THROTTLED(3000000, "FileVideoSource: 像素格式 %d 无法转换", src->format);
        return nullptr;
    }
    const size_t ySize = static_cast<size_t>(w) * h;
    auto buffer = std::shared_ptr<uint8_t>(new uint8_t[ySize + ySize / 2], std::default_delete<uint8_t[]>());
    uint8_t* dst[4] = {buffer.get(), buffer.get() + ySize, nullptr, nullptr};
    int lines[4] = {w, w, 0, 0};
    sws_scale(m_sws, src->data, src->linesize, 0, src->height, dst, lines);

    auto frame = std::make_shared<core::Frame>();
    frame->kind = core::MediaKind::Video;
    frame->width = w;
    frame->height = h;
    frame->pixFmt = AV_PIX_FMT_NV12;
    frame->planes[0] = buffer;
    frame->strides[0] = w;
    frame->planes[1] = std::shared_ptr<uint8_t>(buffer, buffer.get() + ySize);
    frame->strides[1] = w;
    return frame;
}

bool FileVideoSource::SleepUntil(int64_t due, const std::stop_token& st)
{
    while (!st.stop_requested()) {
        const int64_t now = core::MonotonicClock::NowUs();
        if (now >= due) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(std::min(due - now, kMaxSleepUs)));
    }
    return false;
}

void FileVideoSource::Loop(std::stop_token st)
{
    // Close 之后重新开始采集（preview 组关了又开）：在这里重新打开，从头播放
    if (!m_fmt) {
        m_ended.store(false);
        m_loops.store(0);
        if (OpenFile() != TGErr_OK) {
            while (!st.stop_requested()) {
                SleepFor(std::chrono::milliseconds(10));
            }
            return;
        }
    }
    // 出帧时刻 = t0 + 本遍内的相对时间戳 + 之前各遍的总时长
    int64_t t0 = core::MonotonicClock::NowUs();
    int64_t base = 0;                       // 之前各遍累计的时长
    int64_t passStart = AV_NOPTS_VALUE;     // 本遍第一帧的时间戳（us）
    int64_t lastPts = AV_NOPTS_VALUE;       // 本遍上一帧的时间戳（us）
    int64_t lastDue = t0;
    const AVRational tb = m_fmt->streams[m_stream]->time_base;

    while (!st.stop_requested()) {
        if (m_ended.load()) {
            // 播完：按帧率重复最后一帧（不能停：主画面断帧会触发断流看护，预览也会卡住）
            const int64_t due = lastDue + static_cast<int64_t>(1000000.0 / m_fps);
            if (!SleepUntil(due, st)) {
                break;
            }
            lastDue = due;
            if (auto frame = m_last && m_last->width > 0 ? Convert(m_last) : nullptr) {
                frame->captureUs = due;
                frame->ptsUs = due;
                Emit(std::move(frame));
            }
            continue;
        }
        if (!DecodeNext()) {
            bool more = false;
            if (m_loop && lastPts != AV_NOPTS_VALUE) {
                // 一遍的时长 = 最后一帧时间戳 - 第一帧时间戳 + 一帧
                base += lastPts - passStart + m_frameDurUs;
                passStart = AV_NOPTS_VALUE;
                lastPts = AV_NOPTS_VALUE;
                more = Rewind() && DecodeNext();
                if (more) {
                    m_loops.fetch_add(1);
                    TG_LOGI("FileVideoSource: %s 第 %u 遍播完，从头循环", m_path.c_str(), m_loops.load());
                }
            }
            if (!more) {
                m_ended.store(true);
                TG_LOGI("FileVideoSource: %s 播放结束（之后重复最后一帧）", m_path.c_str());
                if (m_onEof) {
                    m_onEof();
                }
                continue;
            }
        }
        int64_t pts = m_frame->best_effort_timestamp;
        pts = pts != AV_NOPTS_VALUE ? av_rescale_q(pts, tb, kUs)
                                    : (lastPts != AV_NOPTS_VALUE ? lastPts + m_frameDurUs : 0);
        if (passStart == AV_NOPTS_VALUE) {
            passStart = pts;
        }
        if (lastPts != AV_NOPTS_VALUE && pts <= lastPts) {
            pts = lastPts + 1;   // 时间戳回退 / 重复：保持单调
        }
        lastPts = pts;
        const int64_t rel = base + (pts - passStart);
        int64_t due = t0 + rel;
        const int64_t now = core::MonotonicClock::NowUs();
        if (now - due > kResyncUs) {
            TG_LOGW("FileVideoSource: 落后 %lldms，重新对齐墙钟", static_cast<long long>((now - due) / 1000));
            t0 = now - rel;
            due = now;
        }
        av_frame_unref(m_last);
        av_frame_ref(m_last, m_frame);
        auto frame = Convert(m_frame);
        av_frame_unref(m_frame);
        if (!SleepUntil(due, st)) {
            break;
        }
        lastDue = due;
        if (frame) {
            frame->captureUs = due;
            frame->ptsUs = due;
            Emit(std::move(frame));
        }
    }
}

}  // namespace TGCapture::capture
