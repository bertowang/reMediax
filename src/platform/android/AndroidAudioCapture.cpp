#ifdef __ANDROID__

#include "platform/android/AndroidAudioCapture.h"

#include "core/Clock.h"
#include "core/Logger.h"
#include "platform/android/AndroidJni.h"

namespace TGCapture::platform
{

AndroidAudioCapture::AndroidAudioCapture(AudioCapSourceType type, bool loopback)
    : capture::AudioSourceBase(type), m_loopback(loopback)
{
}

AndroidAudioCapture::~AndroidAudioCapture()
{
    Stop();
    StopJava();
}

int AndroidAudioCapture::Open(int sampleRate, int channels)
{
    m_sampleRate = sampleRate > 0 ? sampleRate : 44100;
    m_channels = channels > 0 ? channels : 2;
    if (m_loopback && !m_projection) {
        TG_LOGE("AndroidAudioCapture: 系统内录需要 MediaProjection（Android 10+ 授权后传入）");
        return -1;
    }
    if (!StartJava()) {
        return -2;
    }
    return 0;
}

bool AndroidAudioCapture::StartJava()
{
    JNIEnv* env = AndroidJni::Env();
    if (!env) {
        TG_LOGE("AndroidAudioCapture: 拿不到 JNIEnv（是否忘了传 android.JavaVM？）");
        return false;
    }
    jclass clazz = AndroidJni::HelperClass(env);
    if (!clazz) {
        TG_LOGE("AndroidAudioCapture: 找不到 TGCaptureSystemCapture（是否漏打包 bindings/android 的 Java 类？）");
        return false;
    }
    jmethodID start = env->GetStaticMethodID(clazz, "startAudio",
                                             "(Landroid/media/projection/MediaProjection;ZIIJ)Z");
    if (!start) {
        TG_LOGE("AndroidAudioCapture: 找不到 startAudio");
        env->DeleteLocalRef(clazz);
        return false;
    }
    jboolean ok = env->CallStaticBooleanMethod(clazz, start, m_projection,
                                               m_loopback ? JNI_TRUE : JNI_FALSE,
                                               m_sampleRate, m_channels,
                                               reinterpret_cast<jlong>(this));
    env->DeleteLocalRef(clazz);
    if (!ok) {
        TG_LOGE("AndroidAudioCapture: startAudio 失败（loopback=%d）", m_loopback ? 1 : 0);
        return false;
    }
    m_started = true;
    TG_LOGI("AndroidAudioCapture: 就绪 %s %dHz %dch",
            m_loopback ? "系统内录(AudioPlaybackCapture)" : "麦克风(AudioRecord)", m_sampleRate, m_channels);
    return true;
}

void AndroidAudioCapture::Close()
{
    StopJava();
}

void AndroidAudioCapture::StopJava()
{
    if (!m_started) {
        return;
    }
    if (JNIEnv* env = AndroidJni::Env()) {
        jclass clazz = AndroidJni::HelperClass(env);
        if (clazz) {
            // 按 nativePtr 停自己那一路：麦克风与系统内录可以同时开，各自一个 AudioRecord
            jmethodID stop = env->GetStaticMethodID(clazz, "stopAudio", "(J)V");
            if (stop) {
                env->CallStaticVoidMethod(clazz, stop, reinterpret_cast<jlong>(this));
            }
            env->DeleteLocalRef(clazz);
        }
    }
    m_started = false;
}

void AndroidAudioCapture::Loop(std::stop_token token)
{
    // PCM 由 Java 侧线程回调，本线程只保活
    AndroidJni::Env();
    while (!token.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    AndroidJni::DetachIfNeeded();
}

void AndroidAudioCapture::OnPcm(const uint8_t* data, int bytes, int64_t ptsUs)
{
    if (!data || bytes <= 0) {
        return;
    }
    const int frameSize = m_channels * static_cast<int>(sizeof(int16_t));
    if (frameSize <= 0) {
        return;
    }

    auto buffer = std::shared_ptr<uint8_t>(new uint8_t[static_cast<size_t>(bytes)],
                                           std::default_delete<uint8_t[]>());
    if (m_muted.load()) {
        memset(buffer.get(), 0, static_cast<size_t>(bytes));
    } else {
        memcpy(buffer.get(), data, static_cast<size_t>(bytes));
    }

    auto frame = std::make_shared<core::Frame>();
    frame->kind = core::MediaKind::Audio;
    frame->sampleRate = m_sampleRate;
    frame->channels = m_channels;
    frame->sampleFmt = AV_SAMPLE_FMT_S16;
    frame->planar = false;
    frame->nbSamples = bytes / frameSize;
    frame->audioData = buffer;
    frame->audioBytes = static_cast<size_t>(bytes);
    frame->captureUs = ptsUs > 0 ? ptsUs : core::MonotonicClock::NowUs();
    frame->ptsUs = frame->captureUs;
    Emit(std::move(frame));
}

}  // namespace TGCapture::platform

extern "C" JNIEXPORT void JNICALL
Java_com_tencent_tgcapture_TGCaptureSystemCapture_nativeOnPcm(JNIEnv* env, jclass, jlong ptr,
                                                              jobject buf, jint bytes, jlong ptsUs)
{
    auto* self = reinterpret_cast<TGCapture::platform::AndroidAudioCapture*>(ptr);
    if (!self) {
        return;
    }
    auto* data = static_cast<const uint8_t*>(env->GetDirectBufferAddress(buf));
    self->OnPcm(data, bytes, ptsUs);
}

#endif  // __ANDROID__
