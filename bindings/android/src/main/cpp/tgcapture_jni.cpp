// TGCapture Android JNI 胶水：com.tencent.tgcapture.TGCapturer <-> TGCapture::ITGCapturer
//
// 设计要点
//   1. 所有 native 方法在 JNI_OnLoad 里 RegisterNatives 显式注册（不靠 Java_xxx 符号导出），
//      因此 .so 可以 --exclude-libs,ALL 隐藏 ffmpeg 等静态库符号，避免与宿主自带的库冲突。
//   2. 内核在自己的线程上回调（事件分发线程、采集线程）：这些线程不是 Java 线程，
//      FindClass 会走系统 ClassLoader 找不到 App 的类 —— 用到的类与方法 ID 全部在 JNI_OnLoad 缓存。
//   3. 被 attach 上来的线程用 pthread key 的析构自动 Detach：线程退出时仍处于 attach 状态
//      会被 ART 直接 abort（"thread exited while still attached"）。
#include <android/log.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <pthread.h>

#include <cstdio>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

#include "TGCapture/ITGCapturer.h"
#include "core/Logger.h"
#include "platform/android/AndroidJni.h"

#include <libyuv.h>

extern "C"
{
#include <libavcodec/jni.h>

// 内核里 Java 采集回调的实现（src/platform/android/*.cpp），这里只取地址做 RegisterNatives
JNIEXPORT void JNICALL Java_com_tencent_tgcapture_TGCaptureSystemCapture_nativeOnRgbaFrame(
    JNIEnv*, jclass, jlong, jobject, jint, jint, jint, jlong);
JNIEXPORT void JNICALL Java_com_tencent_tgcapture_TGCaptureSystemCapture_nativeOnVideoFrame(
    JNIEnv*, jclass, jlong, jobject, jint, jobject, jint, jobject, jint, jint, jint, jint, jlong);
JNIEXPORT void JNICALL Java_com_tencent_tgcapture_TGCaptureSystemCapture_nativeOnPcm(
    JNIEnv*, jclass, jlong, jobject, jint, jlong);
}

using namespace TGCapture;

namespace
{

constexpr const char* kTag = "TGCapture";
constexpr const char* kCapturerClass = "com/tencent/tgcapture/TGCapturer";
constexpr const char* kSystemCaptureClass = "com/tencent/tgcapture/TGCaptureSystemCapture";
constexpr const char* kListenerClass = "com/tencent/tgcapture/TGCaptureListener";

JavaVM* g_vm = nullptr;
pthread_key_t g_detachKey;

struct ListenerMethods
{
    jmethodID onPrepared = nullptr;
    jmethodID onStarted = nullptr;
    jmethodID onStopped = nullptr;
    jmethodID onError = nullptr;
    jmethodID onNotify = nullptr;
} g_listener;

void DetachOnThreadExit(void*)
{
    if (g_vm) {
        g_vm->DetachCurrentThread();
    }
}

// 取当前线程的 JNIEnv；非 Java 线程自动 attach，线程退出时由 pthread key 析构自动 detach
JNIEnv* CurrentEnv()
{
    if (!g_vm) {
        return nullptr;
    }
    JNIEnv* env = nullptr;
    if (g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) {
        return env;
    }
    JavaVMAttachArgs args{JNI_VERSION_1_6, "tgcapture-callback", nullptr};
    if (g_vm->AttachCurrentThread(&env, &args) != JNI_OK) {
        return nullptr;
    }
    pthread_setspecific(g_detachKey, env);
    return env;
}

std::string ToStdString(JNIEnv* env, jstring s)
{
    if (!s) {
        return {};
    }
    const char* chars = env->GetStringUTFChars(s, nullptr);
    std::string out = chars ? chars : "";
    if (chars) {
        env->ReleaseStringUTFChars(s, chars);
    }
    return out;
}

// 内核日志的字节串是 UTF-8；NewStringUTF 要的是 Modified UTF-8（4 字节序列会触发 CheckJNI abort），
// 经 byte[] + new String(bytes, "UTF-8") 构造最稳妥
jstring ToJString(JNIEnv* env, const char* utf8)
{
    if (!utf8) {
        utf8 = "";
    }
    const auto len = static_cast<jsize>(strlen(utf8));
    jbyteArray bytes = env->NewByteArray(len);
    if (!bytes) {
        return nullptr;
    }
    env->SetByteArrayRegion(bytes, 0, len, reinterpret_cast<const jbyte*>(utf8));
    jclass stringClass = env->FindClass("java/lang/String");
    jmethodID ctor = env->GetMethodID(stringClass, "<init>", "([BLjava/lang/String;)V");
    jstring charset = env->NewStringUTF("UTF-8");
    auto result = static_cast<jstring>(env->NewObject(stringClass, ctor, bytes, charset));
    env->DeleteLocalRef(charset);
    env->DeleteLocalRef(stringClass);
    env->DeleteLocalRef(bytes);
    return result;
}

void ClearException(JNIEnv* env, const char* where)
{
    if (env->ExceptionCheck()) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "Java 回调 %s 抛出异常（已吞掉，避免带进内核线程）", where);
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
}

// 一个 Java TGCapturer 对应一个 Session
struct Session
{
    ITGCapturer* capturer = nullptr;
    std::mutex mutex;
    jobject listener = nullptr;     // GlobalRef
    // MediaProjection 的 GlobalRef。内核的采集源只保存裸指针，所以换新 projection 时旧的也不能
    // 立即释放（已创建的源可能还在用），全部随 Session 销毁一起释放
    std::vector<jobject> projections;

    // 预览画面输出目标（宿主的 SurfaceView / TextureView）。预览回调在内核的预览投递线程上执行，
    // 与 setPreviewSurface 用 previewMutex 互斥
    std::mutex previewMutex;
    ANativeWindow* previewWindow = nullptr;
    int previewWidth = 0;
    int previewHeight = 0;

    // 预览帧（NV12）→ RGBA 写入 Surface。尺寸变化时重设缓冲区几何，由系统按 View 尺寸缩放显示
    void RenderPreview(const PreviewFrame& f)
    {
        if (f.format != PreviewPixelFormat::NV12 || !f.y || !f.uv) {
            return;
        }
        std::lock_guard<std::mutex> lock(previewMutex);
        if (!previewWindow) {
            return;
        }
        if (f.width != previewWidth || f.height != previewHeight) {
            if (ANativeWindow_setBuffersGeometry(previewWindow, f.width, f.height, WINDOW_FORMAT_RGBA_8888) != 0) {
                return;
            }
            previewWidth = f.width;
            previewHeight = f.height;
        }
        ANativeWindow_Buffer buf{};
        if (ANativeWindow_lock(previewWindow, &buf, nullptr) != 0) {
            return;
        }
        if (buf.width >= f.width && buf.height >= f.height) {
            // libyuv 的 ABGR = 内存字节序 R G B A，即 WINDOW_FORMAT_RGBA_8888
            libyuv::NV12ToABGR(f.y, f.yStride, f.uv, f.uvStride, static_cast<uint8_t*>(buf.bits), buf.stride * 4,
                               f.width, f.height);
        }
        ANativeWindow_unlockAndPost(previewWindow);
    }

    void ReleasePreviewWindow()
    {
        std::lock_guard<std::mutex> lock(previewMutex);
        if (previewWindow) {
            ANativeWindow_release(previewWindow);
            previewWindow = nullptr;
        }
        previewWidth = previewHeight = 0;
    }

    // 取 listener 的 local ref（锁内拷出，锁外回调：Java 回调里允许再调本对象的接口）
    jobject AcquireListener(JNIEnv* env)
    {
        std::lock_guard<std::mutex> lock(mutex);
        return listener ? env->NewLocalRef(listener) : nullptr;
    }
};

template <typename Fn>
void WithListener(Session* s, const char* where, Fn&& fn)
{
    JNIEnv* env = CurrentEnv();
    if (!env) {
        return;
    }
    jobject l = s->AcquireListener(env);
    if (!l) {
        return;
    }
    fn(env, l);
    ClearException(env, where);
    env->DeleteLocalRef(l);
}

Session* FromHandle(jlong handle)
{
    return reinterpret_cast<Session*>(handle);
}

// ------------------------------------------------------------------ TGCapturer (static natives)
jint NativeInit(JNIEnv* env, jclass, jint logLevel, jstring logDir)
{
    const int ret = ITGCapturerMgr::Init(logLevel, ToStdString(env, logDir), false);
    // 内核日志转到 logcat（Init 之后再挂：UnInit/Init 会复位 sink）
    core::Logger::Instance().SetSink([](core::LogLevel level, const char* msg) {
        int prio = ANDROID_LOG_INFO;
        switch (level) {
            case core::LogLevel::Debug: prio = ANDROID_LOG_DEBUG; break;
            case core::LogLevel::Warn: prio = ANDROID_LOG_WARN; break;
            case core::LogLevel::Error: prio = ANDROID_LOG_ERROR; break;
            default: break;
        }
        __android_log_write(prio, kTag, msg);
    });
    return ret;
}

void NativeUnInit(JNIEnv*, jclass)
{
    ITGCapturerMgr::UnInit();
}

jstring NativeVersion(JNIEnv* env, jclass)
{
    return ToJString(env, ITGCapturerMgr::GetVersion());
}

jboolean NativeIsEncoderAvailable(JNIEnv* env, jclass, jstring name)
{
    return ITGCapturerMgr::IsEncoderAvailable(ToStdString(env, name)) ? JNI_TRUE : JNI_FALSE;
}

jstring NativeGetAvailableEncoders(JNIEnv* env, jclass, jboolean video)
{
    return ToJString(env, ITGCapturerMgr::GetAvailableEncoders(video == JNI_TRUE).c_str());
}

jlong NativeCreate(JNIEnv*, jclass)
{
    auto* s = new Session();
    s->capturer = ITGCapturer::Create();
    if (!s->capturer) {
        delete s;
        return 0;
    }
    // JavaVM 交给内核：屏幕 / 音频采集要经 JNI 调 Java 的 TGCaptureSystemCapture
    s->capturer->SetPrivateData("android.JavaVM", g_vm);

    s->capturer->SetOnPreparedCallback([s](ITGCapturer*) {
        WithListener(s, "onPrepared", [](JNIEnv* env, jobject l) { env->CallVoidMethod(l, g_listener.onPrepared); });
    });
    s->capturer->SetOnStartCallback([s]() {
        WithListener(s, "onStarted", [](JNIEnv* env, jobject l) { env->CallVoidMethod(l, g_listener.onStarted); });
    });
    s->capturer->SetOnStopCallback([s](int32_t err) {
        WithListener(s, "onStopped", [err](JNIEnv* env, jobject l) {
            env->CallVoidMethod(l, g_listener.onStopped, static_cast<jint>(err));
        });
    });
    s->capturer->SetOnErrorCallback([s](int32_t what, int32_t extra, const char* msg) {
        WithListener(s, "onError", [=](JNIEnv* env, jobject l) {
            jstring jmsg = ToJString(env, msg);
            env->CallVoidMethod(l, g_listener.onError, static_cast<jint>(what), static_cast<jint>(extra), jmsg);
            env->DeleteLocalRef(jmsg);
        });
    });
    s->capturer->SetOnNotifyCallback([s](NotifyMsgType type, const std::string& content, int64_t data) {
        WithListener(s, "onNotify", [&](JNIEnv* env, jobject l) {
            jstring jcontent = ToJString(env, content.c_str());
            env->CallVoidMethod(l, g_listener.onNotify, static_cast<jint>(type), jcontent,
                                static_cast<jlong>(data));
            env->DeleteLocalRef(jcontent);
        });
    });
    s->capturer->SetOnPreviewFrameCallback([s](const PreviewFrame& f) { s->RenderPreview(f); });
    return reinterpret_cast<jlong>(s);
}

void NativeDestroy(JNIEnv* env, jclass, jlong handle)
{
    Session* s = FromHandle(handle);
    if (!s) {
        return;
    }
    // 先销毁内核（会 Stop 并 join 所有线程，之后不会再有回调），再释放 Java 引用
    ITGCapturer::Destroy(s->capturer);
    s->capturer = nullptr;
    s->ReleasePreviewWindow();
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        if (s->listener) {
            env->DeleteGlobalRef(s->listener);
            s->listener = nullptr;
        }
        for (jobject p : s->projections) {
            env->DeleteGlobalRef(p);
        }
        s->projections.clear();
    }
    delete s;
}

void NativeSetListener(JNIEnv* env, jclass, jlong handle, jobject listener)
{
    Session* s = FromHandle(handle);
    if (!s) {
        return;
    }
    std::lock_guard<std::mutex> lock(s->mutex);
    if (s->listener) {
        env->DeleteGlobalRef(s->listener);
    }
    s->listener = listener ? env->NewGlobalRef(listener) : nullptr;
}

void NativeSetMediaProjection(JNIEnv* env, jclass, jlong handle, jobject projection)
{
    Session* s = FromHandle(handle);
    if (!s || !projection) {
        return;
    }
    jobject global = env->NewGlobalRef(projection);
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        s->projections.push_back(global);
    }
    s->capturer->SetPrivateData("android.MediaProjection", global);
}

void NativeSetPreviewSurface(JNIEnv* env, jclass, jlong handle, jobject surface)
{
    Session* s = FromHandle(handle);
    if (!s) {
        return;
    }
    s->ReleasePreviewWindow();
    if (surface) {
        ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
        std::lock_guard<std::mutex> lock(s->previewMutex);
        s->previewWindow = window;
    }
}

jint NativeStartPreview(JNIEnv*, jclass, jlong handle)
{
    Session* s = FromHandle(handle);
    return s ? s->capturer->StartPreview() : TGErr_InvalidState;
}

void NativeStopPreview(JNIEnv*, jclass, jlong handle)
{
    Session* s = FromHandle(handle);
    if (s) {
        s->capturer->StopPreview();
    }
}

jboolean NativeIsPreviewing(JNIEnv*, jclass, jlong handle)
{
    Session* s = FromHandle(handle);
    return (s && s->capturer->IsPreviewing()) ? JNI_TRUE : JNI_FALSE;
}

jboolean NativeIsSupported(JNIEnv*, jclass, jlong handle, jint feature)
{
    Session* s = FromHandle(handle);
    return (s && s->capturer->IsSupported(static_cast<CaptureFeature>(feature))) ? JNI_TRUE : JNI_FALSE;
}

jint NativeEnableMicrophone(JNIEnv*, jclass, jlong handle)
{
    Session* s = FromHandle(handle);
    return s ? s->capturer->EnableMicrophoneCapture(false) : TGErr_InvalidState;
}

jint NativeEnableSpeaker(JNIEnv*, jclass, jlong handle)
{
    Session* s = FromHandle(handle);
    return s ? s->capturer->EnableSpeakerCapture() : TGErr_InvalidState;
}

jboolean NativeCreateScreenCapture(JNIEnv*, jclass, jlong handle)
{
    Session* s = FromHandle(handle);
    if (!s) {
        return JNI_FALSE;
    }
    // 内核约定 Create*Capture 只创建，AddVideoSource 才接上帧回调（漏了则一帧都进不了编码器）。
    // Java 层合成一步：Android 只有屏幕这一种原生视频源，没有"创建了先不用"的场景
    IVideoSource* source = s->capturer->CreateScreenCapture(0, AndroidSurface);
    return (source && s->capturer->AddVideoSource(source) == TGErr_OK) ? JNI_TRUE : JNI_FALSE;
}

jint NativeSetupVideoEncoder(JNIEnv* env, jclass, jlong handle, jint width, jint height, jfloat fps,
                             jint bitrate, jfloat maxFactor, jint gop, jstring codec, jint perfMode,
                             jint threads)
{
    Session* s = FromHandle(handle);
    if (!s) {
        return TGErr_InvalidState;
    }
    VideoEncoderParams p;
    p.width = width;
    p.height = height;
    p.fps = fps;
    p.vBitrate = bitrate;
    p.vBitrateMaxFactor = maxFactor;
    p.gop = gop;
    p.vCodec = ToStdString(env, codec);
    p.perfMode = perfMode == 1 ? PerformanceMode::Gpu : PerformanceMode::Cpu;
    p.threads = threads;
    return s->capturer->SetupVideoEncoder(p);
}

jint NativeSetupAudioEncoder(JNIEnv* env, jclass, jlong handle, jint sampleRate, jint channels,
                             jint bitrate, jstring codec, jboolean enabled)
{
    Session* s = FromHandle(handle);
    if (!s) {
        return TGErr_InvalidState;
    }
    AudioEncoderParams p;
    p.sampleRate = sampleRate;
    p.channels = channels;
    p.aBitrate = bitrate;
    p.aCodec = ToStdString(env, codec);
    p.enabled = enabled == JNI_TRUE;
    return s->capturer->SetupAudioEncoder(p);
}

jint NativeSetupMuxer(JNIEnv* env, jclass, jlong handle, jstring format, jstring output,
                      jint connectTimeoutMs, jint ioTimeoutMs)
{
    Session* s = FromHandle(handle);
    if (!s) {
        return TGErr_InvalidState;
    }
    MuxerParams p;
    p.muxer = ToStdString(env, format);
    p.muxerOutput = ToStdString(env, output);
    p.connectTimeoutMs = connectTimeoutMs;
    p.ioTimeoutMs = ioTimeoutMs;
    return s->capturer->SetupMuxer(p);
}

jint NativePrepare(JNIEnv*, jclass, jlong handle)
{
    Session* s = FromHandle(handle);
    return s ? s->capturer->Prepare() : TGErr_InvalidState;
}

jint NativeStart(JNIEnv*, jclass, jlong handle)
{
    Session* s = FromHandle(handle);
    return s ? s->capturer->Start() : TGErr_InvalidState;
}

void NativeStop(JNIEnv*, jclass, jlong handle)
{
    if (Session* s = FromHandle(handle)) {
        s->capturer->Stop();
    }
}

jboolean NativeMuteAudio(JNIEnv*, jclass, jlong handle, jboolean mute)
{
    Session* s = FromHandle(handle);
    return (s && s->capturer->MuteAudio(mute == JNI_TRUE)) ? JNI_TRUE : JNI_FALSE;
}

jboolean NativeMuteVideo(JNIEnv*, jclass, jlong handle, jboolean mute)
{
    Session* s = FromHandle(handle);
    return (s && s->capturer->MuteVideo(mute == JNI_TRUE)) ? JNI_TRUE : JNI_FALSE;
}

jint NativeSetAVTimeout(JNIEnv*, jclass, jlong handle, jlong audioMs, jlong videoMs)
{
    Session* s = FromHandle(handle);
    return s ? s->capturer->SetAVTimeout(audioMs, videoMs) : TGErr_InvalidState;
}

jfloat NativeGetTrafficStats(JNIEnv*, jclass, jlong handle)
{
    Session* s = FromHandle(handle);
    return s ? s->capturer->GetTrafficStats() : 0.0f;
}

jstring NativeGetStatsInfo(JNIEnv* env, jclass, jlong handle, jint type)
{
    Session* s = FromHandle(handle);
    if (!s) {
        return nullptr;
    }
    return ToJString(env, s->capturer->GetStatsInfo(static_cast<StatsInfoType>(type)).c_str());
}

jint NativePushVideoFrame(JNIEnv* env, jclass, jlong handle, jobject nv12, jint width, jint height,
                          jint stride, jlong ptsUs)
{
    Session* s = FromHandle(handle);
    auto* data = nv12 ? static_cast<const uint8_t*>(env->GetDirectBufferAddress(nv12)) : nullptr;
    if (!s || !data) {
        return TGErr_InvalidArgument;
    }
    return s->capturer->PushExternalVideoFrame(data, width, height, stride, ptsUs, nullptr);
}

jint NativePushAudioFrame(JNIEnv* env, jclass, jlong handle, jint type, jobject pcm, jint bytes,
                          jint sampleRate, jint channels, jlong ptsUs)
{
    Session* s = FromHandle(handle);
    auto* data = pcm ? static_cast<const uint8_t*>(env->GetDirectBufferAddress(pcm)) : nullptr;
    if (!s || !data) {
        return TGErr_InvalidArgument;
    }
    return s->capturer->PushExternalAudioFrame(static_cast<AudioCapSourceType>(type), data, bytes,
                                               sampleRate, channels, ptsUs);
}

// ------------------------------------------------------------------ 运行期音频源
// 成功返回实际 id，失败返回 null（错误码见 logcat）
jstring NativeAddAudioSource(JNIEnv* env, jclass, jlong handle, jint kind, jstring id, jfloat volume,
                             jstring filePath, jboolean loop)
{
    Session* s = FromHandle(handle);
    if (!s) {
        return nullptr;
    }
    AudioSourceDesc desc;
    desc.kind = static_cast<AudioSourceKind>(kind);
    desc.id = ToStdString(env, id);
    desc.volume = volume;
    desc.filePath = ToStdString(env, filePath);
    desc.loop = loop == JNI_TRUE;
    std::string actual;
    const int ret = s->capturer->AddAudioSource(desc, &actual);
    if (ret != TGErr_OK) {
        __android_log_print(ANDROID_LOG_WARN, kTag, "addAudioSource kind=%d id=%s 失败：%d", kind, desc.id.c_str(),
                            ret);
        return nullptr;
    }
    return ToJString(env, actual.c_str());
}

jint NativeRemoveAudioSource(JNIEnv* env, jclass, jlong handle, jstring id)
{
    Session* s = FromHandle(handle);
    return s ? s->capturer->RemoveAudioSource(ToStdString(env, id)) : TGErr_InvalidState;
}

jint NativeSetAudioSourceVolume(JNIEnv* env, jclass, jlong handle, jstring id, jfloat volume)
{
    Session* s = FromHandle(handle);
    return s ? s->capturer->SetAudioSourceVolume(ToStdString(env, id), volume) : TGErr_InvalidState;
}

// JSON：[{"id":"mic","peak":-12.3,"rms":-20.1}, ...]，最后一项 id = "master"
jstring NativeGetAudioLevels(JNIEnv* env, jclass, jlong handle)
{
    Session* s = FromHandle(handle);
    if (!s) {
        return nullptr;
    }
    std::vector<AudioSourceLevel> levels;
    s->capturer->GetAudioLevels(levels);
    std::string json = "[";
    char buf[192];
    for (size_t i = 0; i < levels.size(); ++i) {
        std::snprintf(buf, sizeof(buf), "%s{\"id\":\"%s\",\"peak\":%.1f,\"rms\":%.1f}", i ? "," : "",
                      levels[i].id.c_str(), levels[i].peakDb, levels[i].rmsDb);
        json += buf;
    }
    json += "]";
    return ToJString(env, json.c_str());
}

jint NativePushAudioSourceFrame(JNIEnv* env, jclass, jlong handle, jstring id, jobject pcm, jint bytes,
                                jint sampleRate, jint channels, jlong ptsUs)
{
    Session* s = FromHandle(handle);
    auto* data = pcm ? static_cast<const uint8_t*>(env->GetDirectBufferAddress(pcm)) : nullptr;
    if (!s || !data) {
        return TGErr_InvalidArgument;
    }
    return s->capturer->PushAudioSourceFrame(ToStdString(env, id), data, bytes, sampleRate, channels, ptsUs);
}

#define TG_NATIVE(name, sig, fn) JNINativeMethod{const_cast<char*>(name), const_cast<char*>(sig), reinterpret_cast<void*>(fn)}

bool RegisterAll(JNIEnv* env)
{
    const JNINativeMethod capturerMethods[] = {
        TG_NATIVE("nativeInit", "(ILjava/lang/String;)I", NativeInit),
        TG_NATIVE("nativeUnInit", "()V", NativeUnInit),
        TG_NATIVE("nativeVersion", "()Ljava/lang/String;", NativeVersion),
        TG_NATIVE("nativeIsEncoderAvailable", "(Ljava/lang/String;)Z", NativeIsEncoderAvailable),
        TG_NATIVE("nativeGetAvailableEncoders", "(Z)Ljava/lang/String;", NativeGetAvailableEncoders),
        TG_NATIVE("nativeCreate", "()J", NativeCreate),
        TG_NATIVE("nativeDestroy", "(J)V", NativeDestroy),
        TG_NATIVE("nativeSetListener", "(JLcom/tencent/tgcapture/TGCaptureListener;)V", NativeSetListener),
        TG_NATIVE("nativeSetMediaProjection", "(JLandroid/media/projection/MediaProjection;)V", NativeSetMediaProjection),
        TG_NATIVE("nativeIsSupported", "(JI)Z", NativeIsSupported),
        TG_NATIVE("nativeEnableMicrophone", "(J)I", NativeEnableMicrophone),
        TG_NATIVE("nativeEnableSpeaker", "(J)I", NativeEnableSpeaker),
        TG_NATIVE("nativeCreateScreenCapture", "(J)Z", NativeCreateScreenCapture),
        TG_NATIVE("nativeSetupVideoEncoder", "(JIIFIFILjava/lang/String;II)I", NativeSetupVideoEncoder),
        TG_NATIVE("nativeSetupAudioEncoder", "(JIIILjava/lang/String;Z)I", NativeSetupAudioEncoder),
        TG_NATIVE("nativeSetupMuxer", "(JLjava/lang/String;Ljava/lang/String;II)I", NativeSetupMuxer),
        TG_NATIVE("nativePrepare", "(J)I", NativePrepare),
        TG_NATIVE("nativeStart", "(J)I", NativeStart),
        TG_NATIVE("nativeStop", "(J)V", NativeStop),
        TG_NATIVE("nativeMuteAudio", "(JZ)Z", NativeMuteAudio),
        TG_NATIVE("nativeMuteVideo", "(JZ)Z", NativeMuteVideo),
        TG_NATIVE("nativeSetPreviewSurface", "(JLandroid/view/Surface;)V", NativeSetPreviewSurface),
        TG_NATIVE("nativeStartPreview", "(J)I", NativeStartPreview),
        TG_NATIVE("nativeAddAudioSource", "(JILjava/lang/String;FLjava/lang/String;Z)Ljava/lang/String;",
                  NativeAddAudioSource),
        TG_NATIVE("nativeRemoveAudioSource", "(JLjava/lang/String;)I", NativeRemoveAudioSource),
        TG_NATIVE("nativeSetAudioSourceVolume", "(JLjava/lang/String;F)I", NativeSetAudioSourceVolume),
        TG_NATIVE("nativeGetAudioLevels", "(J)Ljava/lang/String;", NativeGetAudioLevels),
        TG_NATIVE("nativePushAudioSourceFrame", "(JLjava/lang/String;Ljava/nio/ByteBuffer;IIIJ)I",
                  NativePushAudioSourceFrame),
        TG_NATIVE("nativeStopPreview", "(J)V", NativeStopPreview),
        TG_NATIVE("nativeIsPreviewing", "(J)Z", NativeIsPreviewing),
        TG_NATIVE("nativeSetAVTimeout", "(JJJ)I", NativeSetAVTimeout),
        TG_NATIVE("nativeGetTrafficStats", "(J)F", NativeGetTrafficStats),
        TG_NATIVE("nativeGetStatsInfo", "(JI)Ljava/lang/String;", NativeGetStatsInfo),
        TG_NATIVE("nativePushVideoFrame", "(JLjava/nio/ByteBuffer;IIIJ)I", NativePushVideoFrame),
        TG_NATIVE("nativePushAudioFrame", "(JILjava/nio/ByteBuffer;IIIJ)I", NativePushAudioFrame),
    };
    const JNINativeMethod systemCaptureMethods[] = {
        TG_NATIVE("nativeOnRgbaFrame", "(JLjava/nio/ByteBuffer;IIIJ)V",
                  Java_com_tencent_tgcapture_TGCaptureSystemCapture_nativeOnRgbaFrame),
        TG_NATIVE("nativeOnVideoFrame",
                  "(JLjava/nio/ByteBuffer;ILjava/nio/ByteBuffer;ILjava/nio/ByteBuffer;IIIIJ)V",
                  Java_com_tencent_tgcapture_TGCaptureSystemCapture_nativeOnVideoFrame),
        TG_NATIVE("nativeOnPcm", "(JLjava/nio/ByteBuffer;IJ)V",
                  Java_com_tencent_tgcapture_TGCaptureSystemCapture_nativeOnPcm),
    };

    struct Binding
    {
        const char* cls;
        const JNINativeMethod* methods;
        jint count;
    };
    const Binding bindings[] = {
        {kCapturerClass, capturerMethods, static_cast<jint>(std::size(capturerMethods))},
        {kSystemCaptureClass, systemCaptureMethods, static_cast<jint>(std::size(systemCaptureMethods))},
    };
    for (const auto& b : bindings) {
        jclass clazz = env->FindClass(b.cls);
        if (!clazz) {
            __android_log_print(ANDROID_LOG_ERROR, kTag, "JNI_OnLoad: 找不到 %s", b.cls);
            env->ExceptionClear();
            return false;
        }
        const jint rc = env->RegisterNatives(clazz, b.methods, b.count);
        if (b.cls == kSystemCaptureClass) {
            // 采集桥接类缓存给内核：内核在 attach 线程上 FindClass 会找不到 App 的类
            platform::AndroidJni::SetHelperClass(env, clazz);
        }
        env->DeleteLocalRef(clazz);
        if (rc != JNI_OK) {
            __android_log_print(ANDROID_LOG_ERROR, kTag, "JNI_OnLoad: RegisterNatives(%s) 失败", b.cls);
            env->ExceptionClear();
            return false;
        }
    }

    jclass listener = env->FindClass(kListenerClass);
    if (!listener) {
        env->ExceptionClear();
        return false;
    }
    g_listener.onPrepared = env->GetMethodID(listener, "onPrepared", "()V");
    g_listener.onStarted = env->GetMethodID(listener, "onStarted", "()V");
    g_listener.onStopped = env->GetMethodID(listener, "onStopped", "(I)V");
    g_listener.onError = env->GetMethodID(listener, "onError", "(IILjava/lang/String;)V");
    g_listener.onNotify = env->GetMethodID(listener, "onNotify", "(ILjava/lang/String;J)V");
    env->DeleteLocalRef(listener);
    return g_listener.onPrepared && g_listener.onStarted && g_listener.onStopped && g_listener.onError &&
           g_listener.onNotify;
}

}  // namespace

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*)
{
    g_vm = vm;
    pthread_key_create(&g_detachKey, DetachOnThreadExit);
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK || !env) {
        return JNI_ERR;
    }
    platform::AndroidJni::SetJavaVM(vm);
    // h264_mediacodec 经 JNI 调 android.media.MediaCodec，必须先把 JavaVM 交给 ffmpeg
    av_jni_set_java_vm(vm, nullptr);
    if (!RegisterAll(env)) {
        return JNI_ERR;
    }
    return JNI_VERSION_1_6;
}
