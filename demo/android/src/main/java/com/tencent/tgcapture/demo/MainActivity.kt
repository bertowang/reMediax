package com.tencent.tgcapture.demo

import android.Manifest
import android.content.Intent
import android.content.pm.ApplicationInfo
import android.content.pm.PackageManager
import android.media.projection.MediaProjection
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import android.view.View
import android.view.WindowManager
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import com.tencent.tgcapture.TGCapturer
import com.tencent.tgcapture.TGProjectionService
import com.tencent.tgcapture.demo.databinding.ActivityMainBinding
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * 开播测试入口：上半部分是状态 / 控制 / 日志，下半部分是参数设置页。
 *
 * 开播流程：
 *   权限（录音 / 通知）→ [需要录屏或内录] 系统录屏授权 → TGProjectionService 前台服务取 MediaProjection
 *   → LiveSession（worker 线程：create → 配置采集源 → Setup* → prepare → start）
 */
class MainActivity : AppCompatActivity() {

    private enum class State { IDLE, AUTHORIZING, STARTING, LIVE, STOPPING }

    private lateinit var binding: ActivityMainBinding
    private val main = Handler(Looper.getMainLooper())
    private var state = State.IDLE
    private var session: LiveSession? = null
    private var pendingConfig: DemoConfig? = null
    private val logLines = ArrayDeque<String>()

    private val permissionLauncher =
        registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { onPermissionsResult() }

    private val projectionLauncher =
        registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
            val cfg = pendingConfig ?: return@registerForActivityResult
            TGProjectionService.start(this, result.resultCode, result.data, object : TGProjectionService.Callback {
                override fun onProjectionReady(projection: MediaProjection) = startSession(cfg, projection)

                override fun onProjectionFailed(reason: String) {
                    appendLog("录屏授权失败：$reason")
                    setState(State.IDLE)
                }
            })
        }

    private val statsTicker = object : Runnable {
        override fun run() {
            session?.queryStats { bps, info ->
                binding.textBitrate.text = getString(R.string.bitrate_fmt, bps / 1000f)
                binding.textStats.text = info
            }
            main.postDelayed(this, 1000)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        binding.textVersion.text = getString(R.string.version_fmt, TGCapturer.version())
        binding.btnStart.setOnClickListener { if (state == State.IDLE) onStartClicked() else stopLive() }
        binding.switchMuteAudio.setOnCheckedChangeListener { _, checked -> session?.muteAudio(checked) }
        binding.switchMuteVideo.setOnCheckedChangeListener { _, checked -> session?.muteVideo(checked) }
        binding.btnClearLog.setOnClickListener {
            logLines.clear()
            binding.textLog.text = ""
        }
        if (savedInstanceState == null) {
            supportFragmentManager.beginTransaction()
                .replace(R.id.settings_container, SettingsFragment())
                .commit()
        }
        setState(State.IDLE)
        probeEncoders()
        handleAutomation(intent)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        handleAutomation(intent)
    }

    /**
     * 回归脚本入口（scripts/regress_android_demo.sh），只在 debuggable 包里生效：
     *   adb shell am start -n com.tencent.tgcapture.demo/.MainActivity --es tg_cmd start|stop
     * 状态变化与日志同步打到 logcat（tag TGDemo），脚本据此等待 LIVE / IDLE。
     */
    private fun handleAutomation(intent: Intent?) {
        val cmd = intent?.getStringExtra(EXTRA_CMD) ?: return
        intent.removeExtra(EXTRA_CMD)
        if (applicationInfo.flags and ApplicationInfo.FLAG_DEBUGGABLE == 0) return
        Log.i(LOG_TAG, "automation cmd=$cmd state=$state")
        when (cmd) {
            "start" -> if (state == State.IDLE) onStartClicked()
            "stop" -> if (state == State.LIVE) stopLive()
            else -> Log.w(LOG_TAG, "unknown automation cmd: $cmd")
        }
    }

    // 硬编探测会真的建一次编码会话（几十 ~ 几百 ms），放后台线程；结果进程内缓存，转屏重建不会重复探测
    private fun probeEncoders() = Thread({
        val video = TGCapturer.getAvailableEncoders(true).joinToString()
        val audio = TGCapturer.getAvailableEncoders(false).joinToString()
        main.post {
            if (!isDestroyed) {
                appendLog("本机可用编码器\n  视频: ${video.ifEmpty { "无" }}\n  音频: ${audio.ifEmpty { "无" }}")
            }
        }
    }, "demo-probe").start()

    override fun onDestroy() {
        main.removeCallbacksAndMessages(null)
        session?.shutdown()
        session = null
        TGProjectionService.stop(this)
        super.onDestroy()
    }

    // ------------------------------------------------------------------ 开播
    private fun onStartClicked() {
        val cfg = DemoConfig.load(this)
        if (cfg.outputMode == DemoConfig.OUTPUT_PUBLISH && cfg.publishUrl.isBlank()) {
            appendLog("请先在下方设置推流地址")
            return
        }
        pendingConfig = cfg
        appendLog("开播配置：\n${cfg.summary()}")

        val wanted = buildList {
            if (cfg.mic || cfg.speaker) add(Manifest.permission.RECORD_AUDIO)
            // 前台服务通知（Android 13+ 需要运行时授权；拒绝也不影响录屏，只是通知栏看不到）
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) add(Manifest.permission.POST_NOTIFICATIONS)
        }.filterNot(::granted)
        setState(State.AUTHORIZING)
        if (wanted.isEmpty()) onPermissionsResult() else permissionLauncher.launch(wanted.toTypedArray())
    }

    private fun onPermissionsResult() {
        var cfg = pendingConfig ?: return
        if ((cfg.mic || cfg.speaker) && !granted(Manifest.permission.RECORD_AUDIO)) {
            appendLog("未授予录音权限：麦克风 / 系统内录已关闭")
            cfg = cfg.copy(mic = false, speaker = false)
            pendingConfig = cfg
        }
        if (cfg.needsProjection) {
            projectionLauncher.launch(TGProjectionService.createScreenCaptureIntent(this))
        } else {
            startSession(cfg, null)
        }
    }

    private fun startSession(cfg: DemoConfig, projection: MediaProjection?) {
        pendingConfig = null
        setState(State.STARTING)
        session = LiveSession(this, cfg, projection, object : LiveSession.Events {
            override fun onLog(line: String) = appendLog(line)

            override fun onLive() {
                setState(State.LIVE)
                binding.chronometer.base = SystemClock.elapsedRealtime()
                binding.chronometer.start()
                main.post(statsTicker)
            }

            override fun onFinished(err: Int) {
                appendLog(if (err == 0) "已停播" else "开播失败（详见上方日志）")
                finishSession()
            }
        }).also { it.start() }
    }

    private fun stopLive() {
        val s = session ?: return
        setState(State.STOPPING)
        s.stop()
    }

    private fun finishSession() {
        main.removeCallbacks(statsTicker)
        binding.chronometer.stop()
        session?.shutdown()
        session = null
        TGProjectionService.stop(this)
        binding.switchMuteAudio.isChecked = false
        binding.switchMuteVideo.isChecked = false
        setState(State.IDLE)
    }

    // ------------------------------------------------------------------ UI
    private fun setState(s: State) {
        state = s
        Log.i(LOG_TAG, "state=$s")
        binding.textState.text = getString(
            when (s) {
                State.IDLE -> R.string.state_idle
                State.AUTHORIZING -> R.string.state_authorizing
                State.STARTING -> R.string.state_starting
                State.LIVE -> R.string.state_live
                State.STOPPING -> R.string.state_stopping
            }
        )
        binding.btnStart.setText(if (s == State.IDLE) R.string.action_start else R.string.action_stop)
        binding.btnStart.isEnabled = s == State.IDLE || s == State.LIVE
        val live = s == State.LIVE
        binding.switchMuteAudio.isEnabled = live
        binding.switchMuteVideo.isEnabled = live
        binding.liveInfo.visibility = if (s == State.IDLE) View.INVISIBLE else View.VISIBLE
        (supportFragmentManager.findFragmentById(R.id.settings_container) as? SettingsFragment)
            ?.setLocked(s != State.IDLE)
        if (s == State.IDLE) {
            window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
            binding.textBitrate.text = ""
            binding.textStats.text = ""
        } else {
            window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        }
    }

    private fun appendLog(line: String) {
        Log.i(LOG_TAG, line)
        val stamp = SimpleDateFormat("HH:mm:ss", Locale.US).format(Date())
        logLines.addLast("$stamp  $line")
        while (logLines.size > MAX_LOG_LINES) logLines.removeFirst()
        binding.textLog.text = logLines.joinToString("\n")
        binding.scrollLog.post { binding.scrollLog.fullScroll(View.FOCUS_DOWN) }
    }

    private fun granted(permission: String) =
        ContextCompat.checkSelfPermission(this, permission) == PackageManager.PERMISSION_GRANTED

    private companion object {
        const val MAX_LOG_LINES = 200
        const val LOG_TAG = "TGDemo"
        const val EXTRA_CMD = "tg_cmd"
    }
}
