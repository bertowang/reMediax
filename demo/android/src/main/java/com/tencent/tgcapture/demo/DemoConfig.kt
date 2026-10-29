package com.tencent.tgcapture.demo

import android.content.Context
import android.content.SharedPreferences
import android.content.res.Configuration
import android.os.Build
import androidx.preference.PreferenceManager
import com.tencent.tgcapture.AudioEncoderParams
import com.tencent.tgcapture.MuxerParams
import com.tencent.tgcapture.VideoEncoderParams
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/** 设置页（res/xml/preferences.xml）的取值快照：开播时读一次，直播期间不再变化。 */
data class DemoConfig(
    val outputMode: String,
    val publishUrl: String,
    val muxerFormat: String,
    val videoSource: String,
    val width: Int,
    val height: Int,
    val fps: Int,
    val videoBitrateKbps: Int,
    val videoCodec: String,
    val gopSeconds: Int,
    val perfMode: Int,
    val mic: Boolean,
    val speaker: Boolean,
    val testTone: Boolean,
    val audioCodec: String,
    val audioBitrateKbps: Int,
    val sampleRate: Int,
    val channels: Int,
    val avTimeoutMs: Long,
    /** 视频源 = 本地文件时的路径与选项 */
    val videoFile: String,
    val videoFileLoop: Boolean,
    val videoFileAudio: Boolean,
    /** 开播同时开预览（画面下方的 SurfaceView，显示暂存场景） */
    val preview: Boolean,
    /** 画中画叠加层用的视频文件 */
    val overlayVideo: String,
) {
    val screenCapture get() = videoSource == VIDEO_SCREEN
    val testPattern get() = videoSource == VIDEO_PATTERN
    val fileSource get() = videoSource == VIDEO_FILE
    val audioEnabled get() = mic || speaker || testTone || (fileSource && videoFileAudio)

    /** 录屏或系统内录都要先拿到 MediaProjection。 */
    val needsProjection get() = screenCapture || speaker

    fun videoParams() = VideoEncoderParams().also {
        it.width = width
        it.height = height
        it.fps = fps.toFloat()
        it.bitrate = videoBitrateKbps * 1000
        it.gop = fps * gopSeconds
        it.codec = videoCodec
        it.perfMode = perfMode
    }

    fun audioParams() = AudioEncoderParams().also {
        it.sampleRate = sampleRate
        it.channels = channels
        it.bitrate = audioBitrateKbps * 1000
        it.codec = audioCodec
        it.enabled = audioEnabled
    }

    fun muxerParams(context: Context) = MuxerParams().also {
        it.format = muxerFormat
        it.output = if (outputMode == OUTPUT_FILE) localFile(context).absolutePath else publishUrl.trim()
    }

    /** 录制到本地：/sdcard/Android/data/<pkg>/files/records/tgcapture_<时间>.<flv|ts> */
    private fun localFile(context: Context): File {
        val dir = File(context.getExternalFilesDir(null), "records").apply { mkdirs() }
        val stamp = SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(Date())
        val ext = if (muxerFormat == "mpegts") "ts" else "flv"
        return File(dir, "tgcapture_$stamp.$ext")
    }

    fun summary() = buildString {
        append(if (outputMode == OUTPUT_FILE) "本地文件" else publishUrl).append(" [").append(muxerFormat).append("]\n")
        append("视频 ").append(
            when {
                screenCapture -> "录屏"
                fileSource -> "文件 $videoFile" + if (videoFileLoop) "（循环）" else ""
                else -> "测试图案"
            }
        ).append(' ')
        append(width).append('x').append(height).append('@').append(fps).append(' ')
        append(videoBitrateKbps).append("kbps ").append(videoCodec)
        append(" gop=").append(fps * gopSeconds).append(if (perfMode == 1) " GPU" else " CPU")
        append(if (preview) " 预览" else "").append('\n')
        if (audioEnabled) {
            val src = listOfNotNull(
                "麦克风".takeIf { mic }, "内录".takeIf { speaker }, "测试音".takeIf { testTone },
                "文件音轨".takeIf { fileSource && videoFileAudio },
            )
            append("音频 ").append(src.joinToString("+")).append(' ')
            append(sampleRate).append("Hz ").append(channels).append("ch ")
            append(audioBitrateKbps).append("kbps ").append(audioCodec)
        } else {
            append("音频 关闭（纯视频）")
        }
    }

    companion object {
        const val OUTPUT_PUBLISH = "publish"
        const val OUTPUT_FILE = "file"
        const val VIDEO_SCREEN = "screen"
        const val VIDEO_PATTERN = "pattern"
        const val VIDEO_FILE = "file"

        /** 默认素材目录：/sdcard/Android/data/<pkg>/files（adb push 即可，App 无需存储权限） */
        fun mediaDir(context: Context): File = context.getExternalFilesDir(null) ?: context.filesDir

        fun load(context: Context): DemoConfig {
            val p = PreferenceManager.getDefaultSharedPreferences(context)
            val media = mediaDir(context)
            val (long, short) = when (p.str("pref_resolution", "720")) {
                "540" -> 960 to 540
                "1080" -> 1920 to 1080
                else -> 1280 to 720
            }
            val portrait = when (p.str("pref_orientation", "auto")) {
                "portrait" -> true
                "landscape" -> false
                else -> context.resources.configuration.orientation == Configuration.ORIENTATION_PORTRAIT
            }
            return DemoConfig(
                outputMode = p.str("pref_output_mode", OUTPUT_PUBLISH),
                publishUrl = p.str("pref_publish_url", ""),
                muxerFormat = p.str("pref_muxer_format", "flv"),
                videoSource = p.str("pref_video_source", VIDEO_SCREEN),
                width = if (portrait) short else long,
                height = if (portrait) long else short,
                fps = p.int("pref_fps", 30),
                videoBitrateKbps = p.int("pref_video_bitrate", 3000),
                videoCodec = p.str("pref_video_codec", "h264_mediacodec"),
                gopSeconds = p.int("pref_gop_seconds", 2),
                perfMode = p.int("pref_perf_mode", VideoEncoderParams.PERF_CPU),
                mic = p.getBoolean("pref_audio_mic", true),
                // AudioPlaybackCapture 需要 Android 10+
                speaker = p.getBoolean("pref_audio_speaker", false) && Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q,
                testTone = p.getBoolean("pref_audio_tone", false),
                audioCodec = p.str("pref_audio_codec", "libfdk_aac"),
                audioBitrateKbps = p.int("pref_audio_bitrate", 128),
                sampleRate = p.int("pref_audio_sample_rate", 44100),
                channels = p.int("pref_audio_channels", 2),
                avTimeoutMs = p.int("pref_av_timeout", 3000).toLong(),
                videoFile = p.str("pref_video_file", File(media, "main.mp4").absolutePath),
                videoFileLoop = p.getBoolean("pref_video_file_loop", true),
                videoFileAudio = p.getBoolean("pref_video_file_audio", true),
                preview = p.getBoolean("pref_preview", true),
                overlayVideo = p.str("pref_overlay_video", File(media, "pip.mp4").absolutePath),
            )
        }

        private fun SharedPreferences.str(key: String, def: String) = getString(key, null)?.takeIf { it.isNotBlank() } ?: def
        private fun SharedPreferences.int(key: String, def: Int) = getString(key, null)?.toIntOrNull() ?: def
    }
}
