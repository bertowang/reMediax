package com.tencent.tgcapture.demo

import android.content.Context
import android.media.projection.MediaProjection
import android.os.Handler
import android.os.Looper
import com.tencent.tgcapture.TGCaptureListener
import com.tencent.tgcapture.TGCapturer
import java.util.concurrent.Executors
import java.util.concurrent.RejectedExecutionException

/**
 * 一次开播会话：TGCapturer 的完整生命周期。
 *
 * prepare / start / stop 会阻塞（连推流地址、写文件尾），全部放在单线程 worker 上串行执行；
 * 事件统一切回主线程交给 [Events]。
 */
class LiveSession(
    private val context: Context,
    val config: DemoConfig,
    private val projection: MediaProjection?,
    private val events: Events,
) {
    interface Events {
        fun onLog(line: String)
        fun onLive()
        fun onFinished(err: Int)
    }

    private val main = Handler(Looper.getMainLooper())
    private val worker = Executors.newSingleThreadExecutor { Thread(it, "demo-session") }
    private var capturer: TGCapturer? = null
    private var signals: TestSignalSource? = null
    @Volatile
    private var finished = false

    fun start() = submit {
        val c = try {
            TGCapturer.create()
        } catch (e: IllegalStateException) {
            log("TGCapturer.create 失败: ${e.message}")
            teardown(TGERR_FAILED)
            return@submit
        }
        capturer = c
        c.setListener(listener)
        if (!configure(c) || !check("prepare", c.prepare()) || !check("start", c.start())) {
            teardown(TGERR_FAILED)
            return@submit
        }
        signals = TestSignalSource(c, config).also { it.start() }
        post { events.onLive() }
    }

    /** 停播（可重复调用）。 */
    fun stop() = submit { teardown(0) }

    fun muteAudio(mute: Boolean) = submit { capturer?.muteAudio(mute) }

    fun muteVideo(mute: Boolean) = submit { capturer?.muteVideo(mute) }

    /** 运行期加文件音频源（循环），id 固定为 "file"。 */
    fun addFileAudio(path: String) = submit {
        val c = capturer ?: return@submit
        val id = c.addFileAudioSource("file", path, true, 1.0f)
        log("addFileAudio $path id=$id")
    }

    /** 运行期切分辨率：spec = 档位名（720p / 360p …）或 "宽x高"。 */
    fun switchResolution(spec: String) = submit {
        val c = capturer ?: return@submit
        val m = Regex("""(\d+)x(\d+)""").matchEntire(spec)
        val rc = if (m != null) {
            c.setVideoResolution(m.groupValues[1].toInt(), m.groupValues[2].toInt(), 0)
        } else {
            c.setResolutionPreset(spec, false)
        }
        log("switchResolution $spec rc=$rc (${ErrorText.of(rc)})")
    }

    /** 统计在 worker 上取（避免与 release 并发），结果回主线程。 */
    fun queryStats(cb: (bps: Float, info: String) -> Unit) = submit {
        val c = capturer ?: return@submit
        val bps = c.trafficStats
        val info = c.getStatsInfo(TGCapturer.STATS_BITRATE).orEmpty()
        post { cb(bps, info) }
    }

    /** 停播并结束 worker；之后本对象的调用全部忽略。 */
    fun shutdown() {
        stop()
        worker.shutdown()
    }

    // 内核回调线程上也可能调到这里（onError -> stop），worker 已关闭时静默忽略
    private fun submit(task: () -> Unit) {
        if (!worker.isShutdown) {
            try {
                worker.execute(task)
            } catch (_: RejectedExecutionException) {
            }
        }
    }

    private fun configure(c: TGCapturer): Boolean {
        projection?.let { c.setMediaProjection(it) }
        if (config.screenCapture && !c.createScreenCapture()) {
            log("createScreenCapture 失败")
            return false
        }
        // 音频源失败不致命：缺哪一路就少哪一路，内核会对缺数据的情况上报断流
        if (config.mic) check("enableMicrophoneCapture", c.enableMicrophoneCapture())
        if (config.speaker) check("enableSpeakerCapture", c.enableSpeakerCapture())

        val mux = config.muxerParams(context)
        log("输出 -> ${mux.output}")
        return check("setupVideoEncoder", c.setupVideoEncoder(config.videoParams())) &&
            check("setupAudioEncoder", c.setupAudioEncoder(config.audioParams())) &&
            check("setupMuxer", c.setupMuxer(mux)) &&
            check("setAVTimeout", c.setAVTimeout(config.avTimeoutMs, config.avTimeoutMs))
    }

    private fun teardown(err: Int) {
        if (finished) return
        finished = true
        signals?.stop()
        signals = null
        capturer?.let {
            it.stop()
            it.release()
        }
        capturer = null
        post { events.onFinished(err) }
    }

    private fun check(what: String, rc: Int): Boolean {
        if (rc != 0) log("$what 失败 rc=$rc (${ErrorText.of(rc)})")
        return rc == 0
    }

    private fun log(line: String) = post { events.onLog(line) }

    private fun post(r: () -> Unit) {
        main.post { r() }
    }

    private val listener = object : TGCaptureListener {
        override fun onPrepared() = log("onPrepared")

        override fun onStarted() = log("onStarted")

        override fun onStopped(err: Int) = log("onStopped err=$err (${ErrorText.of(err)})")

        override fun onError(what: Int, extra: Int, message: String?) {
            log("onError what=$what (${ErrorText.of(what)}) extra=$extra $message")
            // 推流断开后 FFmpeg 的 I/O 错误是粘性的，内核不会自动重连：停播，由用户重新开始
            if (what == ErrorText.MUX_WRITE_FAILED) stop()
        }

        override fun onNotify(type: Int, content: String?, data: Long) =
            log("onNotify ${ErrorText.notify(type)} data=$data ${content.orEmpty()}")
    }

    private companion object {
        const val TGERR_FAILED = -1
    }
}

/** 错误码 / 通知类型的可读文本（对应 include/TGCapture/TGCapturerInterface.h）。 */
object ErrorText {
    const val MUX_WRITE_FAILED = -101

    fun of(code: Int) = when (code) {
        0 -> "OK"
        -1 -> "InvalidState"
        -2 -> "NotConfigured"
        -3 -> "OpenVideoEncoder"
        -4 -> "OpenAudioEncoder"
        -5 -> "OpenMuxer"
        -6 -> "OpenVideoSource"
        -7 -> "StartFailed"
        -20 -> "InvalidArgument"
        -21 -> "NotSupported"
        -22 -> "NotImplemented"
        -23 -> "Internal"
        -101 -> "MuxWriteFailed"
        -102 -> "VideoEncodeFailed"
        -103 -> "AudioEncodeFailed"
        -104 -> "VideoCaptureStalled"
        -105 -> "AudioCaptureStalled"
        -106 -> "MuxTrailerFailed"
        else -> "?"
    }

    fun notify(type: Int) = when (type) {
        0 -> "FirstFrameOK"
        1 -> "VideoMetaChanged"
        10 -> "AudioMetaChanged"
        20 -> "SystemFreeze"
        21 -> "LocalFileEOF"
        30 -> "VideoRecovered"
        31 -> "AudioRecovered"
        40 -> "EncoderFallback"
        else -> "type=$type"
    }
}
