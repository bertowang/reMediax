package com.tencent.tgcapture;

import android.media.projection.MediaProjection;
import android.os.Handler;
import android.os.Looper;
import android.view.Surface;

import java.nio.ByteBuffer;

/**
 * TGCapture 开播内核的 Java 门面（与 C++ {@code TGCapture::ITGCapturer} 一一对应）。
 *
 * <pre>
 * TGCapturer.init(TGCapturer.LOG_INFO, null);          // 进程内一次
 * TGCapturer c = TGCapturer.create();
 * c.setListener(listener);
 * c.setMediaProjection(projection);                    // 录屏 / 系统内录前必须
 * c.createScreenCapture();  c.enableMicrophoneCapture();
 * c.setupVideoEncoder(v); c.setupAudioEncoder(a); c.setupMuxer(m);
 * c.setPreviewSurface(surfaceView.getHolder().getSurface());   // 可选：开播前预览
 * c.startPreview();                                    // 可选：Idle → Previewing（需已 setupVideoEncoder）
 * c.prepare(); c.start();                              // 会连推流地址，必须在工作线程调用
 * ...
 * c.stop();                                            // 预览开着则回到预览，否则回到 Idle
 * c.stopPreview(); c.release();
 * </pre>
 *
 * <p>返回 int 的接口：0 = 成功，负数 = TGCaptureError。
 * prepare/start/stop 会阻塞（连接、收尾），不要在主线程调用。
 */
public final class TGCapturer {
    static {
        System.loadLibrary("tgcapture_jni");
    }

    public static final int LOG_DEBUG = 0;
    public static final int LOG_INFO = 1;
    public static final int LOG_WARN = 2;
    public static final int LOG_ERROR = 3;

    // CaptureFeature
    public static final int FEATURE_SCREEN = 0;
    public static final int FEATURE_MICROPHONE = 2;
    public static final int FEATURE_SPEAKER = 3;

    // StatsInfoType
    public static final int STATS_META = 1;
    public static final int STATS_BITRATE = 2;
    public static final int STATS_PACKET = 3;
    public static final int STATS_AUDIO = 4;
    public static final int STATS_VIDEO = 5;

    // NotifyMsgType（TGCaptureListener.onNotify 的 type，完整列表见 TGCapturerInterface.h）
    /** 运行期分辨率切换已生效：content = "宽x高"，data = 码率 bps */
    public static final int NOTIFY_VIDEO_META_CHANGED = 1;
    /** 编码器回落：content = "video:h264_mediacodec->libopenh264"，data = 0 视频 / 1 音频 */
    public static final int NOTIFY_ENCODER_FALLBACK = 40;
    /** 运行期音频源增删：content = 音频源 id，data = 混流通道号 */
    public static final int NOTIFY_AUDIO_SOURCE_ADDED = 41;
    public static final int NOTIFY_AUDIO_SOURCE_REMOVED = 42;

    // AudioSourceKind（addAudioSource）
    public static final int AUDIO_SOURCE_MICROPHONE = 0;
    public static final int AUDIO_SOURCE_SPEAKER = 1;
    public static final int AUDIO_SOURCE_EXTERNAL = 2;
    public static final int AUDIO_SOURCE_FILE = 3;

    // AudioCapSourceType（外部注入音频用）
    public static final int AUDIO_GAME = 0;
    public static final int AUDIO_GAME_MIC = 1;

    private long mHandle;

    private TGCapturer(long handle) {
        mHandle = handle;
    }

    // ------------------------------------------------------------------ 全局
    /** 进程内调用一次。logDir 为空则不落盘；内核日志统一输出到 logcat（tag = TGCapture）。 */
    public static int init(int logLevel, String logDir) {
        return nativeInit(logLevel, logDir == null ? "" : logDir);
    }

    public static void uninit() {
        nativeUnInit();
    }

    public static String version() {
        return nativeVersion();
    }

    /**
     * 本机能否真正打开该编码器（硬编取决于设备 / 系统）。首次探测硬编会创建一次编码会话，
     * 耗时几十 ~ 几百 ms，结果进程内缓存；不要在主线程上批量调用。
     * 不探测也能开播：请求的编码器打不开时内核自动回落（视频 → libopenh264，音频 → aac），
     * 并通过 onNotify(NOTIFY_ENCODER_FALLBACK) 告知。
     */
    public static boolean isEncoderAvailable(String name) {
        return name != null && nativeIsEncoderAvailable(name);
    }

    /** 本机可用的编码器，按推荐顺序（视频：系统硬编在前），例如 ["h264_mediacodec", "libopenh264"]。 */
    public static String[] getAvailableEncoders(boolean video) {
        String list = nativeGetAvailableEncoders(video);
        return list == null || list.isEmpty() ? new String[0] : list.split(",");
    }

    /** 内置分辨率档位名，按分辨率从低到高，例如 ["360p", "480p", ..., "1080p60"]。 */
    public static String[] getResolutionPresets() {
        String list = nativeGetResolutionPresets();
        return list == null || list.isEmpty() ? new String[0] : list.split(",");
    }

    /**
     * 按档位填写 width / height / fps / bitrate（其余字段不动）。portrait = true 时宽高互换。
     * 档位名不认识返回 false，params 不变。
     */
    public static boolean applyResolutionPreset(String name, VideoEncoderParams params, boolean portrait) {
        if (name == null || params == null) {
            return false;
        }
        int[] v = nativeGetResolutionPreset(name, portrait);
        if (v == null || v.length != 4) {
            return false;
        }
        params.width = v[0];
        params.height = v[1];
        params.fps = v[2] / 1000f;
        params.bitrate = v[3];
        return true;
    }

    public static TGCapturer create() {
        long handle = nativeCreate();
        if (handle == 0) {
            throw new IllegalStateException("TGCapturer 创建失败");
        }
        return new TGCapturer(handle);
    }

    // ------------------------------------------------------------------ 配置
    public void setListener(TGCaptureListener listener) {
        nativeSetListener(handle(), listener);
    }

    /**
     * 录屏与系统内录共用的 MediaProjection（由 {@link TGProjectionService} 取得）。
     * 必须在 {@link #createScreenCapture()} / {@link #enableSpeakerCapture()} 之前调用。
     *
     * <p>Android 14+ 要求在 createVirtualDisplay 之前给 projection 注册回调，这里统一注册：
     * 用户从系统通知栏停止投屏时 onStop 会被触发，届时采集画面停止（内核会上报视频断流）。
     */
    public void setMediaProjection(MediaProjection projection) {
        if (projection == null) {
            return;
        }
        projection.registerCallback(new MediaProjection.Callback() {
            @Override
            public void onStop() {
                // 无需处理：VirtualDisplay 随 projection 失效，断流由内核看护上报
            }
        }, new Handler(Looper.getMainLooper()));
        nativeSetMediaProjection(handle(), projection);
    }

    public boolean isSupported(int feature) {
        return nativeIsSupported(handle(), feature);
    }

    public int enableMicrophoneCapture() {
        return nativeEnableMicrophone(handle());
    }

    /** 系统内录（AudioPlaybackCapture）：Android 10+，需先 setMediaProjection。 */
    public int enableSpeakerCapture() {
        return nativeEnableSpeaker(handle());
    }

    /** 创建屏幕采集（MediaProjection）并设为视频源（= C++ CreateScreenCapture + AddVideoSource），需先 setMediaProjection。 */
    public boolean createScreenCapture() {
        return nativeCreateScreenCapture(handle());
    }

    public int setupVideoEncoder(VideoEncoderParams p) {
        return nativeSetupVideoEncoder(handle(), p.width, p.height, p.fps, p.bitrate, p.bitrateMaxFactor,
                p.gop, p.codec, p.perfMode, p.threads);
    }

    public int setupAudioEncoder(AudioEncoderParams p) {
        return nativeSetupAudioEncoder(handle(), p.sampleRate, p.channels, p.bitrate, p.codec, p.enabled);
    }

    public int setupMuxer(MuxerParams p) {
        return nativeSetupMuxer(handle(), p.format, p.output, p.connectTimeoutMs, p.ioTimeoutMs);
    }

    // ------------------------------------------------------------------ 生命周期
    public int prepare() {
        return nativePrepare(handle());
    }

    public int start() {
        return nativeStart(handle());
    }

    public void stop() {
        nativeStop(handle());
    }

    // ------------------------------------------------------------------ 预览
    /**
     * 预览画面输出到该 Surface（RGBA，缓冲区尺寸 = 采集尺寸，由系统缩放到 View）。传 null 解除。
     * Surface 销毁前（surfaceDestroyed）必须先传 null。
     */
    public void setPreviewSurface(Surface surface) {
        nativeSetPreviewSurface(handle(), surface);
    }

    /**
     * 开播前预览：打开屏幕采集（需先 setMediaProjection + createScreenCapture + setupVideoEncoder）。
     * 预览期间 prepare/start 不会重开采集；stop 后回到预览，不需要重新申请录屏授权。
     */
    public int startPreview() {
        return nativeStartPreview(handle());
    }

    /** 关闭预览。只有预览时同时停止采集；推流中只关预览，推流不受影响。 */
    public void stopPreview() {
        nativeStopPreview(handle());
    }

    public boolean isPreviewing() {
        return nativeIsPreviewing(handle());
    }

    /** 释放内核对象（内部会先 stop）。之后本对象不可再用。 */
    public synchronized void release() {
        if (mHandle != 0) {
            nativeDestroy(mHandle);
            mHandle = 0;
        }
    }

    // ------------------------------------------------------------------ 运行期
    public boolean muteAudio(boolean mute) {
        return nativeMuteAudio(handle(), mute);
    }

    public boolean muteVideo(boolean mute) {
        return nativeMuteVideo(handle(), mute);
    }

    public int setAVTimeout(long audioTimeoutMs, long videoTimeoutMs) {
        return nativeSetAVTimeout(handle(), audioTimeoutMs, videoTimeoutMs);
    }

    /**
     * 切换编码分辨率，推流不断（任意状态可调，需先 setupVideoEncoder）。
     * 推流中：编码器按新尺寸重开、首帧 IDR，屏幕采集同步改 VirtualDisplay 尺寸（不重新授权）；
     * 生效后 onNotify(NOTIFY_VIDEO_META_CHANGED)。未开播：下次 prepare 生效。
     * bitrate = 0 保持当前码率；帧率不变。
     */
    public int setVideoResolution(int width, int height, int bitrate) {
        return nativeSetVideoResolution(handle(), width, height, bitrate);
    }

    /** 按档位切换（分辨率与码率取档位值，帧率不变）。portrait = true 时宽高互换。 */
    public int setResolutionPreset(String name, boolean portrait) {
        return nativeSetResolutionPreset(handle(), name == null ? "" : name, portrait);
    }

    /** 当前输出码率（bps）。 */
    public float getTrafficStats() {
        return nativeGetTrafficStats(handle());
    }

    /** JSON 统计信息，type 见 STATS_*。 */
    public String getStatsInfo(int type) {
        return nativeGetStatsInfo(handle(), type);
    }

    /** 外部注入 NV12 帧（data 必须是 DirectByteBuffer）。start 之后（或预览期间）调用。 */
    public int pushVideoFrameNv12(ByteBuffer data, int width, int height, int stride, long ptsUs) {
        return nativePushVideoFrame(handle(), data, width, height, stride, ptsUs);
    }

    /** 外部注入交错 S16 PCM（data 必须是 DirectByteBuffer）。ptsUs 传 -1 由内核取到达时刻。 */
    public int pushAudioFrame(int type, ByteBuffer data, int bytes, int sampleRate, int channels, long ptsUs) {
        return nativePushAudioFrame(handle(), type, data, bytes, sampleRate, channels, ptsUs);
    }

    // ------------------------------------------------------------------ 运行期音频源
    /**
     * 增加一路音频源（任意状态；推流中立即生效，淡入 20ms）。
     *
     * @param kind   AUDIO_SOURCE_*
     * @param id     唯一标识（字母数字 . _ -），null = 内核生成（mic / speaker / pcm / file …）
     * @param volume 线性增益 0 ~ 4
     * @return 实际 id；失败返回 null（原因见 logcat）
     */
    public String addAudioSource(int kind, String id, float volume) {
        return nativeAddAudioSource(handle(), kind, id, volume, null, true);
    }

    /** 文件音轨（mp4 / mov / flv / ts，AAC）。 */
    public String addFileAudioSource(String id, String filePath, boolean loop, float volume) {
        return nativeAddAudioSource(handle(), AUDIO_SOURCE_FILE, id, volume, filePath, loop);
    }

    /** 移除（推流中先淡出 20ms）。旧接口建的源 id：mic / speaker / ext.&lt;type&gt;。 */
    public int removeAudioSource(String id) {
        return nativeRemoveAudioSource(handle(), id);
    }

    public int setAudioSourceVolume(String id, float volume) {
        return nativeSetAudioSourceVolume(handle(), id, volume);
    }

    /** 各路电平 JSON：[{"id":"mic","peak":-12.3,"rms":-20.1}, …, {"id":"master",…}]，单位 dBFS（-100 = 静音）。 */
    public String getAudioLevels() {
        return nativeGetAudioLevels(handle());
    }

    /** External 类音频源推交错 S16 PCM（data 必须是 DirectByteBuffer）。 */
    public int pushAudioSourceFrame(String id, ByteBuffer data, int bytes, int sampleRate, int channels, long ptsUs) {
        return nativePushAudioSourceFrame(handle(), id, data, bytes, sampleRate, channels, ptsUs);
    }

    private long handle() {
        if (mHandle == 0) {
            throw new IllegalStateException("TGCapturer 已 release");
        }
        return mHandle;
    }

    // ---- native（libtgcapture_jni.so，JNI_OnLoad 中 RegisterNatives 注册）----
    private static native int nativeInit(int logLevel, String logDir);

    private static native void nativeUnInit();

    private static native String nativeVersion();

    private static native boolean nativeIsEncoderAvailable(String name);

    private static native String nativeGetAvailableEncoders(boolean video);
    private static native String nativeGetResolutionPresets();
    private static native int[] nativeGetResolutionPreset(String name, boolean portrait);

    private static native long nativeCreate();

    private static native void nativeDestroy(long handle);

    private static native void nativeSetListener(long handle, TGCaptureListener listener);

    private static native void nativeSetMediaProjection(long handle, MediaProjection projection);

    private static native void nativeSetPreviewSurface(long handle, Surface surface);

    private static native int nativeStartPreview(long handle);

    private static native String nativeAddAudioSource(long handle, int kind, String id, float volume, String filePath,
                                                      boolean loop);

    private static native int nativeRemoveAudioSource(long handle, String id);

    private static native int nativeSetAudioSourceVolume(long handle, String id, float volume);

    private static native String nativeGetAudioLevels(long handle);

    private static native int nativePushAudioSourceFrame(long handle, String id, ByteBuffer data, int bytes,
                                                         int sampleRate, int channels, long ptsUs);

    private static native void nativeStopPreview(long handle);

    private static native boolean nativeIsPreviewing(long handle);

    private static native boolean nativeIsSupported(long handle, int feature);

    private static native int nativeEnableMicrophone(long handle);

    private static native int nativeEnableSpeaker(long handle);

    private static native boolean nativeCreateScreenCapture(long handle);

    private static native int nativeSetupVideoEncoder(long handle, int width, int height, float fps, int bitrate,
                                                      float bitrateMaxFactor, int gop, String codec, int perfMode,
                                                      int threads);

    private static native int nativeSetupAudioEncoder(long handle, int sampleRate, int channels, int bitrate,
                                                      String codec, boolean enabled);

    private static native int nativeSetupMuxer(long handle, String format, String output, int connectTimeoutMs,
                                               int ioTimeoutMs);

    private static native int nativePrepare(long handle);

    private static native int nativeStart(long handle);

    private static native void nativeStop(long handle);

    private static native boolean nativeMuteAudio(long handle, boolean mute);

    private static native boolean nativeMuteVideo(long handle, boolean mute);

    private static native int nativeSetAVTimeout(long handle, long audioMs, long videoMs);
    private static native int nativeSetVideoResolution(long handle, int width, int height, int bitrate);
    private static native int nativeSetResolutionPreset(long handle, String name, boolean portrait);

    private static native float nativeGetTrafficStats(long handle);

    private static native String nativeGetStatsInfo(long handle, int type);

    private static native int nativePushVideoFrame(long handle, ByteBuffer data, int width, int height, int stride,
                                                   long ptsUs);

    private static native int nativePushAudioFrame(long handle, int type, ByteBuffer data, int bytes, int sampleRate,
                                                   int channels, long ptsUs);
}
