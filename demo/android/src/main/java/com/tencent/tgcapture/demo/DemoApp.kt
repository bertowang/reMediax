package com.tencent.tgcapture.demo

import android.app.Application
import android.util.Log
import com.tencent.tgcapture.TGCapturer

class DemoApp : Application() {
    override fun onCreate() {
        super.onCreate()
        // 进程内一次。日志同时进 logcat（tag = TGCapture）和 files/logs/ 下的滚动文件：
        //   adb pull /sdcard/Android/data/com.tencent.tgcapture.demo/files/logs
        val logDir = getExternalFilesDir("logs")?.absolutePath.orEmpty()
        val rc = TGCapturer.init(TGCapturer.LOG_INFO, logDir)
        Log.i(TAG, "TGCapturer.init rc=$rc version=${TGCapturer.version()} logDir=$logDir")
    }

    private companion object {
        const val TAG = "TGCaptureDemo"
    }
}
