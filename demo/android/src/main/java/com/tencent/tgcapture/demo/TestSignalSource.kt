package com.tencent.tgcapture.demo

import com.tencent.tgcapture.TGCapturer
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.math.PI
import kotlin.math.sin

/**
 * 测试信号源：不依赖录屏 / 麦克风，经外部注入接口推帧，用于模拟器或排查采集问题。
 *   视频：NV12 彩条 + 一个水平移动的白块（肉眼可判断卡顿 / 掉帧）
 *   音频：1kHz 正弦，每秒响 0.5s（便于听出断音和音画不同步）
 */
class TestSignalSource(
    private val capturer: TGCapturer,
    private val config: DemoConfig,
) {
    @Volatile
    private var running = false
    private val threads = mutableListOf<Thread>()

    fun start() {
        running = true
        if (config.testPattern) {
            threads += Thread(::videoLoop, "demo-pattern").apply { start() }
        }
        if (config.testTone) {
            threads += Thread(::audioLoop, "demo-tone").apply { start() }
        }
    }

    fun stop() {
        running = false
        threads.forEach { it.join(1000) }
        threads.clear()
    }

    private fun videoLoop() {
        val w = config.width
        val h = config.height
        val frameSize = w * h * 3 / 2
        val base = buildColorBars(w, h)
        val frame = ByteBuffer.allocateDirect(frameSize)
        val intervalNs = 1_000_000_000L / config.fps
        val box = (minOf(w, h) / 6) and 1.inv()
        // 步长取偶数：NV12 的 UV 按 2 像素一组，白块左边界必须落在偶数列
        val step = maxOf(2, w / (config.fps * 4)) and 1.inv()
        var next = System.nanoTime()
        var x = 0
        while (running) {
            frame.clear()
            frame.put(base)
            base.rewind()
            // 白块：Y=235，UV=128（中性色）
            val top = ((h - box) / 2) and 1.inv()
            for (row in top until top + box) {
                val off = row * w + x
                for (col in 0 until box) frame.put(off + col, 235.toByte())
            }
            for (row in top / 2 until (top + box) / 2) {
                val off = w * h + row * w + x
                for (col in 0 until box) frame.put(off + col, 128.toByte())
            }
            x += step
            if (x + box > w) x = 0

            capturer.pushVideoFrameNv12(frame, w, h, w, System.nanoTime() / 1000)
            next += intervalNs
            val sleepNs = next - System.nanoTime()
            if (sleepNs > 0) Thread.sleep(sleepNs / 1_000_000, (sleepNs % 1_000_000).toInt())
            else next = System.nanoTime()
        }
    }

    private fun audioLoop() {
        val rate = config.sampleRate
        val ch = config.channels
        val samplesPer10ms = rate / 100
        val bytes = samplesPer10ms * ch * 2
        val buf = ByteBuffer.allocateDirect(bytes).order(ByteOrder.nativeOrder())
        var n = 0L
        val start = System.nanoTime()
        var chunk = 0L
        while (running) {
            buf.clear()
            for (i in 0 until samplesPer10ms) {
                val inBeep = (n % rate) < rate / 2
                val v = if (inBeep) (sin(2 * PI * 1000.0 * n / rate) * 8000).toInt().toShort() else 0
                repeat(ch) { buf.putShort(v) }
                n++
            }
            // 采集时刻按墙钟推进（内核用它测漂移，不能传"按样本数累加"的时间）
            capturer.pushAudioFrame(TGCapturer.AUDIO_GAME, buf, bytes, rate, ch, System.nanoTime() / 1000)
            chunk++
            val sleepNs = start + chunk * 10_000_000L - System.nanoTime()
            if (sleepNs > 0) Thread.sleep(sleepNs / 1_000_000, (sleepNs % 1_000_000).toInt())
        }
    }

    /** 8 条 75% 彩条（BT.601 limited range）。 */
    private fun buildColorBars(w: Int, h: Int): ByteBuffer {
        val yuv = arrayOf(
            intArrayOf(180, 128, 128), intArrayOf(162, 44, 142), intArrayOf(131, 156, 44),
            intArrayOf(112, 72, 58), intArrayOf(84, 184, 198), intArrayOf(65, 100, 212),
            intArrayOf(35, 212, 114), intArrayOf(16, 128, 128),
        )
        val out = ByteBuffer.allocateDirect(w * h * 3 / 2)
        val bar = (w + 7) / 8
        for (row in 0 until h) for (col in 0 until w) out.put(yuv[col / bar][0].toByte())
        for (row in 0 until h / 2) {
            var col = 0
            while (col < w) {
                val c = yuv[col / bar]
                out.put(c[1].toByte()); out.put(c[2].toByte())
                col += 2
            }
        }
        out.flip()
        return out
    }
}
