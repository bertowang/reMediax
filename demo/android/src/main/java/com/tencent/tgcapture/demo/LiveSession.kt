package com.tencent.tgcapture.demo

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.media.projection.MediaProjection
import android.os.Handler
import android.os.Looper
import android.view.Surface
import com.tencent.tgcapture.OverlayDesc
import com.tencent.tgcapture.TGCaptureListener
import com.tencent.tgcapture.TGCapturer
import java.io.File
import java.util.concurrent.Executors
import java.util.concurrent.RejectedExecutionException

/**
 * 一次开播会话：TGCapturer 的完整生命周期。
 *
 * prepare / start / stop 会阻塞（连推流地址、写文件尾），全部放在单线程 worker 上串行执行；
 * 事件统一切回主线程交给 [Events]。
 *
 * 叠加层演示（staged / commit）：Logo（运行时生成的半透明 png）与画中画（本地视频文件）。
 * 增删 / 移动只改暂存场景 —— 预览立即可见，推流画面要等「提交」才原子切换；「撤销」回到已提交场景。
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
        /** 叠加层状态变化：暂存 / 已提交两份列表的可读摘要 */
        fun onOverlays(summary: String) {}
    }

    private val main = Handler(Looper.getMainLooper())
    private val worker = Executors.newSingleThreadExecutor { Thread(it, "demo-session") }
    private var capturer: TGCapturer? = null
    private var signals: TestSignalSource? = null
    @Volatile
    private var finished = false

    // 预览 Surface 由主线程（SurfaceHolder 回调）直接设置：surfaceDestroyed 必须同步解绑，不能排队等 worker
    private val previewLock = Any()
    private var previewSurface: Surface? = null
    private var pipCorner = 0

    fun start() = submit {
        val c = try {
            TGCapturer.create()
        } catch (e: IllegalStateException) {
            log("TGCapturer.create 失败: ${e.message}")
            teardown(TGERR_FAILED)
            return@submit
        }
        synchronized(previewLock) {
            capturer = c
            previewSurface?.let { c.setPreviewSurface(it) }
        }
        c.setListener(listener)
        if (!configure(c)) {
            teardown(TGERR_FAILED)
            return@submit
        }
        // 预览先开：Prepare 前就能看到画面（叠加层的暂存效果也在预览上看）
        if (config.preview) check("startPreview", c.startPreview())
        if (!check("prepare", c.prepare()) || !check("start", c.start())) {
            teardown(TGERR_FAILED)
            return@submit
        }
        signals = TestSignalSource(c, config).also { it.start() }
        post { events.onLive() }
        publishOverlays(c)
    }

    /** 主线程调用：Surface 可用时传入，surfaceDestroyed 时传 null（同步解绑）。 */
    fun setPreviewSurface(surface: Surface?) {
        synchronized(previewLock) {
            previewSurface = surface
            capturer?.setPreviewSurface(surface)
        }
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

    // ------------------------------------------------------------------ 叠加层（只改暂存，提交才上推流）
    /** Logo：右上角半透明图片；已暂存则删除。 */
    fun toggleLogo() = submit {
        val c = capturer ?: return@submit
        if (c.getOverlays(true).any { it.id == LOGO_ID }) {
            log("stageRemoveOverlay logo rc=${rcText(c.stageRemoveOverlay(LOGO_ID))}")
        } else {
            val png = logoFile() ?: return@submit
            val d = OverlayDesc(LOGO_ID, OverlayDesc.KIND_IMAGE, png.absolutePath).apply {
                x = 0.72f; y = 0.05f; width = 0.24f; zOrder = 2
            }
            log("stageOverlay $d rc=${rcText(c.stageOverlay(d))}")
        }
        publishOverlays(c)
    }

    /** 画中画：本地视频文件（设置页「画中画视频」）；已暂存则删除。 */
    fun togglePip() = submit {
        val c = capturer ?: return@submit
        if (c.getOverlays(true).any { it.id == PIP_ID }) {
            log("stageRemoveOverlay pip rc=${rcText(c.stageRemoveOverlay(PIP_ID))}")
        } else {
            if (!File(config.overlayVideo).isFile) {
                log("画中画视频不存在：${config.overlayVideo}（adb push 一个 mp4 过去，或在设置页改路径）")
                return@submit
            }
            pipCorner = 0
            val d = OverlayDesc(PIP_ID, OverlayDesc.KIND_VIDEO_FILE, config.overlayVideo).apply {
                width = PIP_WIDTH; zOrder = 1
                x = PIP_CORNERS[0].first; y = PIP_CORNERS[0].second
            }
            log("stageOverlay $d rc=${rcText(c.stageOverlay(d))}")
        }
        publishOverlays(c)
    }

    /** 画中画换到下一个角（只改几何：内容复用，视频不会从头播）。 */
    fun movePip() = submit {
        val c = capturer ?: return@submit
        val d = c.getOverlays(true).firstOrNull { it.id == PIP_ID } ?: run {
            log("暂存区没有画中画")
            return@submit
        }
        pipCorner = (pipCorner + 1) % PIP_CORNERS.size
        d.x = PIP_CORNERS[pipCorner].first
        d.y = PIP_CORNERS[pipCorner].second
        log("stageOverlay(move) $d rc=${rcText(c.stageOverlay(d))}")
        publishOverlays(c)
    }

    fun commitOverlays() = submit {
        val c = capturer ?: return@submit
        log("commitOverlays rc=${rcText(c.commitOverlays())}")
        publishOverlays(c)
    }

    fun discardOverlays() = submit {
        val c = capturer ?: return@submit
        log("discardOverlayChanges rc=${rcText(c.discardOverlayChanges())}")
        publishOverlays(c)
    }

    private fun publishOverlays(c: TGCapturer) {
        val staged = c.getOverlays(true).joinToString(" ") { it.id }
        val program = c.getOverlays(false).joinToString(" ") { it.id }
        val stagedFull = c.getOverlays(true).joinToString("|") { it.toString() }
        val programFull = c.getOverlays(false).joinToString("|") { it.toString() }
        val dirty = stagedFull != programFull
        val summary = "推流: ${program.ifEmpty { "无" }}   预览: ${staged.ifEmpty { "无" }}" +
            if (dirty) "   （有未提交改动）" else ""
        post { events.onOverlays(summary) }
    }

    /** 运行时生成 Logo：半透明绿底圆角矩形 + 白字（带 alpha 通道，演示按透明度混合）。 */
    private fun logoFile(): File? {
        val f = File(context.cacheDir, "tg_logo.png")
        if (f.isFile) return f
        return try {
            val bmp = Bitmap.createBitmap(320, 120, Bitmap.Config.ARGB_8888)
            val canvas = Canvas(bmp)
            val bg = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.argb(200, 0, 200, 80) }
            canvas.drawRoundRect(RectF(0f, 0f, 320f, 120f), 24f, 24f, bg)
            val text = Paint(Paint.ANTI_ALIAS_FLAG).apply {
                color = Color.WHITE; textSize = 64f; textAlign = Paint.Align.CENTER; isFakeBoldText = true
            }
            canvas.drawText("TG LIVE", 160f, 82f, text)
            f.outputStream().use { bmp.compress(Bitmap.CompressFormat.PNG, 100, it) }
            bmp.recycle()
            f
        } catch (e: Exception) {
            log("生成 logo 失败: ${e.message}")
            null
        }
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
        if (config.fileSource) {
            if (!c.createLocalFileCapture(config.videoFile, config.videoFileLoop)) {
                log("createLocalFileCapture 失败：${config.videoFile}（文件不存在 / 无 H.264 视频轨，详见 logcat）")
                return false
            }
            // 文件里的音轨不会自动加入，需要声音另加文件音频源（两者各自按墙钟播放）
            if (config.videoFileAudio) {
                val id = c.addFileAudioSource("file", config.videoFile, config.videoFileLoop, 1.0f)
                log("addFileAudioSource(主画面音轨) id=$id")
            }
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
        val c = synchronized(previewLock) {
            capturer.also { capturer = null }
        }
        c?.let {
            it.stop()
            it.release()
        }
        post { events.onFinished(err) }
    }

    private fun rcText(rc: Int) = if (rc == 0) "0" else "$rc(${ErrorText.of(rc)})"

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
        const val LOGO_ID = "logo"
        const val PIP_ID = "pip"
        const val PIP_WIDTH = 0.35f
        /** 画中画的四个角（左上角坐标，画布比例；PIP_WIDTH = 0.35 时 16:9 素材高约 0.35） */
        val PIP_CORNERS = listOf(0.62f to 0.60f, 0.03f to 0.60f, 0.03f to 0.05f, 0.62f to 0.05f)
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
        41 -> "AudioSourceAdded"
        42 -> "AudioSourceRemoved"
        43 -> "OverlayCommitted"
        else -> "type=$type"
    }
}
