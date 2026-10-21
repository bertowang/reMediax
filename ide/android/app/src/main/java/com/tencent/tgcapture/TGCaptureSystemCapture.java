package com.tencent.tgcapture;

import android.graphics.ImageFormat;
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
import android.os.Looper;
import android.view.Surface;

import java.nio.ByteBuffer;

/**
 * 采集桥接层：把 Android 系统采集能力（MediaProjection / AudioRecord / AudioPlaybackCapture）
 * 接到 TGCapture 内核。C++ 侧通过 JNI 调用本类的 start*/stop* 方法。
 *
 * 使用方式：
 *   1) App 侧通过 MediaProjectionManager.createScreenCaptureIntent() 申请授权，
 *      在 onActivityResult 里拿到 MediaProjection；
 *   2) capturer.SetPrivateData("android.JavaVM", vm) 与 SetPrivateData("android.MediaProjection", projection)；
 *   3) 之后 EnableMicrophoneCapture / EnableSpeakerCapture / CreateScreenCapture 即可正常工作。
 */
public final class TGCaptureSystemCapture {
    static {
        System.loadLibrary("tgcapture");
    }

    private static final String TAG = "TGCaptureSystemCapture";

    private static VirtualDisplay sVirtualDisplay;
    private static ImageReader sImageReader;
    private static AudioRecord sAudioRecord;
    private static Thread sAudioThread;
    private static volatile boolean sAudioRunning = false;
    private static Handler sHandler;

    private TGCaptureSystemCapture() {
    }

    private static synchronized Handler handler() {
        if (sHandler == null) {
            sHandler = new Handler(Looper.getMainLooper());
        }
        return sHandler;
    }

    /** 屏幕采集：encoderSurface 非空时走零拷贝（帧直投编码器 Surface），否则走 ImageReader CPU 路径。 */
    public static synchronized boolean startScreen(MediaProjection projection, Surface encoderSurface,
                                                   int width, int height, int fps, long nativePtr) {
        stopScreen();
        if (projection == null) {
            return false;
        }
        final int dpi = 1;
        if (encoderSurface != null) {
            sVirtualDisplay = projection.createVirtualDisplay("tgcapture", width, height, dpi,
                    DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR, encoderSurface, null, handler());
            return sVirtualDisplay != null;
        }

        sImageReader = ImageReader.newInstance(width, height, ImageFormat.YUV_420_888, 3);
        final ImageReader reader = sImageReader;
        reader.setOnImageAvailableListener(new ImageReader.OnImageAvailableListener() {
            @Override
            public void onImageAvailable(ImageReader r) {
                Image image = r.acquireLatestImage();
                if (image == null) {
                    return;
                }
                try {
                    Image.Plane[] planes = image.getPlanes();
                    if (planes.length < 3) {
                        return;
                    }
                    nativeOnVideoFrame(nativePtr,
                            planes[0].getBuffer(), planes[0].getRowStride(),
                            planes[1].getBuffer(), planes[1].getRowStride(),
                            planes[2].getBuffer(), planes[2].getRowStride(),
                            planes[1].getPixelStride(),
                            image.getWidth(), image.getHeight(),
                            System.nanoTime() / 1000L);
                } finally {
                    image.close();
                }
            }
        }, handler());

        sVirtualDisplay = projection.createVirtualDisplay("tgcapture", width, height, dpi,
                DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR, sImageReader.getSurface(), null, handler());
        return sVirtualDisplay != null;
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
    }

    /** loopback=false 麦克风（MIC）；true 系统内录（AudioPlaybackCapture，需 Android 10+ 且已授权）。 */
    public static synchronized boolean startAudio(MediaProjection projection, boolean loopback,
                                                  int sampleRate, int channels, long nativePtr) {
        stopAudio();
        final int channelConfig = channels >= 2 ? AudioFormat.CHANNEL_IN_STEREO : AudioFormat.CHANNEL_IN_MONO;
        final int encoding = AudioFormat.ENCODING_PCM_16BIT;
        final int minBuf = AudioRecord.getMinBufferSize(sampleRate, channelConfig, encoding);
        if (minBuf <= 0) {
            return false;
        }
        final int bufferSize = minBuf * 2;

        AudioRecord record;
        if (loopback) {
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q || projection == null) {
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
        if (record.getState() != AudioRecord.STATE_INITIALIZED) {
            return false;
        }

        sAudioRecord = record;
        sAudioRunning = true;
        record.startRecording();

        final ByteBuffer direct = ByteBuffer.allocateDirect(bufferSize);
        final byte[] tmp = new byte[bufferSize];
        sAudioThread = new Thread(new Runnable() {
            @Override
            public void run() {
                while (sAudioRunning) {
                    int n = sAudioRecord.read(tmp, 0, tmp.length);
                    if (n <= 0) {
                        continue;
                    }
                    direct.clear();
                    direct.put(tmp, 0, n);
                    nativeOnPcm(nativePtr, direct, n, System.nanoTime() / 1000L);
                }
            }
        }, "tgcapture-audio");
        sAudioThread.start();
        return true;
    }

    public static synchronized void stopAudio() {
        sAudioRunning = false;
        if (sAudioThread != null) {
            try {
                sAudioThread.join(200);
            } catch (InterruptedException ignored) {
            }
            sAudioThread = null;
        }
        if (sAudioRecord != null) {
            try {
                sAudioRecord.stop();
            } catch (IllegalStateException ignored) {
            }
            sAudioRecord.release();
            sAudioRecord = null;
        }
    }

    // ---- native（由 libtgcapture.so 实现）----
    private static native void nativeOnVideoFrame(long ptr, ByteBuffer y, int yStride,
                                                  ByteBuffer u, int uStride,
                                                  ByteBuffer v, int vStride,
                                                  int uvPixelStride, int width, int height, long ptsUs);

    private static native void nativeOnPcm(long ptr, ByteBuffer pcm, int bytes, long ptsUs);
}
