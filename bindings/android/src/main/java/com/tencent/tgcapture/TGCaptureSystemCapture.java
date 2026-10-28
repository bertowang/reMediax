package com.tencent.tgcapture;

import android.graphics.PixelFormat;
import android.hardware.display.DisplayManager;
import android.hardware.display.VirtualDisplay;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioPlaybackCaptureConfiguration;
import android.media.AudioRecord;
import android.media.Image;
import android.media.ImageReader;
import android.media.MediaRecorder;
import android.media.projection.MediaProjection;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;
import android.view.Surface;

import java.nio.ByteBuffer;
import java.util.HashMap;
import java.util.Map;

/**
 * 采集桥接层（内部类，业务方不要直接调用）：把 Android 系统采集能力
 * （MediaProjection / AudioRecord / AudioPlaybackCapture）接到 TGCapture 内核。
 * C++ 侧（src/platform/android/*）通过 JNI 调用本类的 start* / stop* 静态方法，
 * 采集到的数据再经 native* 方法回灌给 C++。
 *
 * <p>类引用由 libtgcapture_jni.so 的 JNI_OnLoad 预先缓存：内核在自己 attach 的线程上
 * 调用 FindClass 会走系统 ClassLoader，找不到 App 里的类。
 */
public final class TGCaptureSystemCapture {
    private static final String TAG = "TGCaptureSystemCapture";

    private static VirtualDisplay sVirtualDisplay;
    private static ImageReader sImageReader;
    private static HandlerThread sVideoThread;

    /** 每个 native 音频源一路 AudioRecord（麦克风与系统内录可同时开）。 */
    private static final Map<Long, AudioSession> sAudio = new HashMap<>();

    private TGCaptureSystemCapture() {
    }

    // ------------------------------------------------------------------ 屏幕
    /**
     * 屏幕采集。encoderSurface 非空时帧直接投到编码器 Surface（零拷贝）；
     * 否则走 ImageReader(RGBA_8888) → nativeOnRgbaFrame（C++ 侧转 NV12）。
     *
     * <p>VirtualDisplay 的生产者只输出 RGBA，ImageReader 必须用 RGBA_8888：
     * 配 YUV_420_888 时格式协商失败，一帧都收不到。
     */
    public static synchronized boolean startScreen(MediaProjection projection, Surface encoderSurface,
                                                   int width, int height, int fps, final long nativePtr) {
        stopScreen();
        if (projection == null) {
            Log.e(TAG, "startScreen: MediaProjection 为空");
            return false;
        }
        final int dpi = 1;
        sVideoThread = new HandlerThread("tgcapture-screen");
        sVideoThread.start();
        final Handler handler = new Handler(sVideoThread.getLooper());
        try {
            if (encoderSurface != null) {
                sVirtualDisplay = projection.createVirtualDisplay("tgcapture", width, height, dpi,
                        DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR, encoderSurface, null, handler);
                return sVirtualDisplay != null;
            }

            sImageReader = ImageReader.newInstance(width, height, PixelFormat.RGBA_8888, 3);
            sImageReader.setOnImageAvailableListener(new ImageReader.OnImageAvailableListener() {
                private boolean mFirst = true;

                @Override
                public void onImageAvailable(ImageReader r) {
                    Image image = null;
                    try {
                        image = r.acquireLatestImage();
                        if (image == null) {
                            return;
                        }
                        Image.Plane plane = image.getPlanes()[0];
                        if (mFirst) {
                            mFirst = false;
                            Log.i(TAG, "screen 首帧 " + image.getWidth() + "x" + image.getHeight()
                                    + " rowStride=" + plane.getRowStride() + " pixelStride=" + plane.getPixelStride());
                        }
                        nativeOnRgbaFrame(nativePtr, plane.getBuffer(), plane.getRowStride(),
                                image.getWidth(), image.getHeight(), System.nanoTime() / 1000L);
                    } catch (IllegalStateException e) {
                        Log.w(TAG, "acquireLatestImage: " + e.getMessage());
                    } finally {
                        if (image != null) {
                            image.close();
                        }
                    }
                }
            }, handler);

            sVirtualDisplay = projection.createVirtualDisplay("tgcapture", width, height, dpi,
                    DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR, sImageReader.getSurface(), null, handler);
            return sVirtualDisplay != null;
        } catch (RuntimeException e) {
            // Android 14+：同一个 MediaProjection token 只能 createVirtualDisplay 一次，
            // 重复使用会抛 SecurityException —— 需要重新弹授权框拿新的 projection
            Log.e(TAG, "createVirtualDisplay 失败: " + e);
            stopScreen();
            return false;
        }
    }

    public static synchronized void stopScreen() {
        if (sVirtualDisplay != null) {
            sVirtualDisplay.release();
            sVirtualDisplay = null;
        }
        if (sImageReader != null) {
            sImageReader.setOnImageAvailableListener(null, null);
            sImageReader.close();
            sImageReader = null;
        }
        if (sVideoThread != null) {
            sVideoThread.quitSafely();
            sVideoThread = null;
        }
    }

    // ------------------------------------------------------------------ 音频
    /** loopback=false 麦克风（MIC）；true 系统内录（AudioPlaybackCapture，需 Android 10+ 且已授权）。 */
    public static synchronized boolean startAudio(MediaProjection projection, boolean loopback,
                                                  int sampleRate, int channels, long nativePtr) {
        stopAudio(nativePtr);
        final int channelConfig = channels >= 2 ? AudioFormat.CHANNEL_IN_STEREO : AudioFormat.CHANNEL_IN_MONO;
        final int encoding = AudioFormat.ENCODING_PCM_16BIT;
        final int minBuf = AudioRecord.getMinBufferSize(sampleRate, channelConfig, encoding);
        if (minBuf <= 0) {
            Log.e(TAG, "startAudio: 不支持的参数 " + sampleRate + "Hz " + channels + "ch");
            return false;
        }
        final int bufferSize = minBuf * 2;

        AudioRecord record;
        try {
            if (loopback) {
                if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q || projection == null) {
                    Log.e(TAG, "startAudio: 系统内录需要 Android 10+ 且持有 MediaProjection");
                    return false;
                }
                AudioPlaybackCaptureConfiguration cfg =
                        new AudioPlaybackCaptureConfiguration.Builder(projection)
                                .addMatchingUsage(AudioAttributes.USAGE_MEDIA)
                                .addMatchingUsage(AudioAttributes.USAGE_GAME)
                                .addMatchingUsage(AudioAttributes.USAGE_UNKNOWN)
                                .build();
                record = new AudioRecord.Builder()
                        .setAudioPlaybackCaptureConfig(cfg)
                        .setAudioFormat(new AudioFormat.Builder()
                                .setEncoding(encoding)
                                .setSampleRate(sampleRate)
                                .setChannelMask(channelConfig)
                                .build())
                        .setBufferSizeInBytes(bufferSize)
                        .build();
            } else {
                record = new AudioRecord(MediaRecorder.AudioSource.MIC, sampleRate, channelConfig,
                        encoding, bufferSize);
            }
        } catch (SecurityException | UnsupportedOperationException | IllegalArgumentException e) {
            // 未授予 RECORD_AUDIO 时这里抛 SecurityException
            Log.e(TAG, "startAudio: 创建 AudioRecord 失败（是否已授予 RECORD_AUDIO？）: " + e);
            return false;
        }
        if (record.getState() != AudioRecord.STATE_INITIALIZED) {
            record.release();
            Log.e(TAG, "startAudio: AudioRecord 初始化失败 loopback=" + loopback);
            return false;
        }
        AudioSession session = new AudioSession(record, bufferSize, nativePtr);
        sAudio.put(nativePtr, session);
        session.start();
        return true;
    }

    public static synchronized void stopAudio(long nativePtr) {
        AudioSession session = sAudio.remove(nativePtr);
        if (session != null) {
            session.stop();
        }
    }

    private static final class AudioSession {
        private final AudioRecord mRecord;
        private final int mBufferSize;
        private final long mNativePtr;
        private volatile boolean mRunning;
        private Thread mThread;

        AudioSession(AudioRecord record, int bufferSize, long nativePtr) {
            mRecord = record;
            mBufferSize = bufferSize;
            mNativePtr = nativePtr;
        }

        void start() {
            mRunning = true;
            mRecord.startRecording();
            mThread = new Thread(new Runnable() {
                @Override
                public void run() {
                    // 直接读进 DirectByteBuffer：C++ 侧 GetDirectBufferAddress 零拷贝取数据
                    final ByteBuffer direct = ByteBuffer.allocateDirect(mBufferSize);
                    while (mRunning) {
                        direct.clear();
                        int n = mRecord.read(direct, mBufferSize);
                        if (n <= 0) {
                            continue;
                        }
                        // 采集时刻 ≈ 读到时刻 - 本块时长；内核会做漂移测量，这里给到达时刻即可
                        nativeOnPcm(mNativePtr, direct, n, System.nanoTime() / 1000L);
                    }
                }
            }, "tgcapture-audio");
            mThread.start();
        }

        void stop() {
            mRunning = false;
            try {
                mRecord.stop();   // 先 stop 让阻塞中的 read 返回，再 join
            } catch (IllegalStateException ignored) {
            }
            if (mThread != null) {
                try {
                    mThread.join(500);
                } catch (InterruptedException ignored) {
                    Thread.currentThread().interrupt();
                }
            }
            mRecord.release();
        }
    }

    // ---- native（由 libtgcapture_jni.so 实现，见 src/platform/android/*.cpp）----
    private static native void nativeOnRgbaFrame(long ptr, ByteBuffer rgba, int rowStride,
                                                 int width, int height, long ptsUs);

    private static native void nativeOnVideoFrame(long ptr, ByteBuffer y, int yStride,
                                                  ByteBuffer u, int uStride,
                                                  ByteBuffer v, int vStride,
                                                  int uvPixelStride, int width, int height, long ptsUs);

    private static native void nativeOnPcm(long ptr, ByteBuffer pcm, int bytes, long ptsUs);
}
