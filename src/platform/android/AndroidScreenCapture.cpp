#ifdef __ANDROID__

#include "platform/android/AndroidScreenCapture.h"

#include "core/Clock.h"
#include "core/Logger.h"
#include "platform/android/AndroidJni.h"

extern "C"
{
#include <libyuv.h>
}

namespace TGCapture::platform
{

AndroidScreenCapture::~AndroidScreenCapture()
{
    Stop();
    StopJava();
}

void AndroidScreenCapture::SetProjection(jobject projection) noexcept
{
    m_projection = projection;
}

void AndroidScreenCapture::SetEncoderSurface(jobject surface) noexcept
{
    m_encoderSurface = surface;
}

int AndroidScreenCapture::Open(int width, int height, float fps)
{
    m_width = width > 0 ? (width & ~1) : 1280;
    m_height = height > 0 ? (height & ~1) : 720;
    m_fps = fps > 0 ? fps : 30.0f;

    if (!m_projection) {
        TG_LOGE("AndroidScreenCapture: 未设置 MediaProjection（先 SetPrivateData(\"android.MediaProjection\")）");
        return -1;
    }
    if (!StartJava()) {
        return -2;
    }
    return 0;
}

bool AndroidScreenCapture::StartJava()
{
    JNIEnv* env = AndroidJni::Env();
    if (!env) {
        TG_LOGE("AndroidScreenCapture: 拿不到 JNIEnv（是否忘了传 android.JavaVM？）");
        return false;
    }
    jclass clazz = AndroidJni::HelperClass(env);
    if (!clazz) {
        TG_LOGE("AndroidScreenCapture: 找不到 TGCaptureSystemCapture（是否漏打包 bindings/android 的 Java 类？）");
        return false;
    }
    jmethodID start = env->GetStaticMethodID(clazz, "startScreen",
                                             "(Landroid/media/projection/MediaProjection;"
                                             "Landroid/view/Surface;IIIJ)Z");
    if (!start) {
        TG_LOGE("AndroidScreenCapture: 找不到 startScreen");
        env->DeleteLocalRef(clazz);
        return false;
    }
    jboolean ok = env->CallStaticBooleanMethod(clazz, start, m_projection, m_encoderSurface,
                                               m_width, m_height, static_cast<int>(m_fps),
                                               reinterpret_cast<jlong>(this));
    env->DeleteLocalRef(clazz);
    if (!ok) {
        TG_LOGE("AndroidScreenCapture: startScreen 失败");
        return false;
    }
    m_started = true;
    TG_LOGI("AndroidScreenCapture: 就绪 %dx%d fps=%.1f surface=%d", m_width, m_height, m_fps,
            m_encoderSurface != nullptr ? 1 : 0);
    return true;
}

void AndroidScreenCapture::Close()
{
    StopJava();
}

void AndroidScreenCapture::StopJava()
{
    if (!m_started) {
        return;
    }
    if (JNIEnv* env = AndroidJni::Env()) {
        jclass clazz = AndroidJni::HelperClass(env);
        if (clazz) {
            jmethodID stop = env->GetStaticMethodID(clazz, "stopScreen", "()V");
            if (stop) {
                env->CallStaticVoidMethod(clazz, stop);
            }
            env->DeleteLocalRef(clazz);
        }
    }
    m_started = false;
}

void AndroidScreenCapture::Loop(std::stop_token token)
{
    // 帧由 Java 侧（ImageReader 回调）驱动，本线程只负责保活与停止响应
    AndroidJni::Env();
    while (!token.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    AndroidJni::DetachIfNeeded();
}

void AndroidScreenCapture::OnFrame(const uint8_t* y, int yStride,
                                   const uint8_t* u, int uStride,
                                   const uint8_t* v, int vStride,
                                   int uvPixelStride, int width, int height, int64_t ptsUs)
{
    if (!y || width <= 0 || height <= 0) {
        return;
    }
    const int w = width & ~1;
    const int h = height & ~1;

    // I420(IYUV) 或 NV12 统一转成 NV12：半平面更省内存，且与 openh264/NVENC 输入一致
    const size_t ySize = static_cast<size_t>(w) * h;
    const size_t uvSize = static_cast<size_t>(w) * h / 2;
    auto buffer = std::shared_ptr<uint8_t>(new uint8_t[ySize + uvSize], std::default_delete<uint8_t[]>());
    uint8_t* dstY = buffer.get();
    uint8_t* dstUV = buffer.get() + ySize;

    const bool interleaved = uvPixelStride == 1 && u && v && (v == u + 1 || u == v + 1);
    if (interleaved) {
        // 已是半平面（NV12/NV21）：逐行拷 Y 与交错 UV
        const uint8_t* srcUV = (v == u + 1) ? u : v;
        libyuv::CopyPlane(y, yStride, dstY, w, w, h);
        libyuv::CopyPlane(srcUV, uStride, dstUV, w, w, h / 2);
    } else if (u && v) {
        // 三平面 I420
        libyuv::I420ToNV12(y, yStride, u, uStride, v, vStride, dstY, w, dstUV, w, w, h);
    } else {
        libyuv::CopyPlane(y, yStride, dstY, w, w, h);
        memset(dstUV, 128, uvSize);   // 无色度信息时给灰，至少保证亮度可用
    }

    auto frame = std::make_shared<core::Frame>();
    frame->kind = core::MediaKind::Video;
    frame->width = w;
    frame->height = h;
    frame->pixFmt = AV_PIX_FMT_NV12;
    frame->ptsUs = ptsUs > 0 ? ptsUs : core::MonotonicClock::NowUs();
    frame->captureUs = core::MonotonicClock::NowUs();
    frame->planes[0] = buffer;
    frame->strides[0] = w;
    frame->planes[1] = std::shared_ptr<uint8_t>(buffer, dstUV);
    frame->strides[1] = w;
    Emit(std::move(frame));
}

void AndroidScreenCapture::OnRgbaFrame(const uint8_t* rgba, int rowStride, int width, int height, int64_t ptsUs)
{
    if (!rgba || width <= 0 || height <= 0 || rowStride < width * 4) {
        return;
    }
    const int w = width & ~1;
    const int h = height & ~1;
    const size_t ySize = static_cast<size_t>(w) * h;
    const size_t uvSize = static_cast<size_t>(w) * h / 2;
    auto buffer = std::shared_ptr<uint8_t>(new uint8_t[ySize + uvSize], std::default_delete<uint8_t[]>());
    uint8_t* dstY = buffer.get();
    uint8_t* dstUV = buffer.get() + ySize;

    // Android RGBA_8888 的内存字节序是 R,G,B,A —— 对应 libyuv 的 "ABGR"（libyuv 按小端 32 位字命名）。
    // rowStride 可能大于 width*4（行尾对齐填充），必须按 rowStride 取行
    if (libyuv::ABGRToNV12(rgba, rowStride, dstY, w, dstUV, w, w, h) != 0) {
        TG_LOGW_THROTTLED(3000000, "AndroidScreenCapture: RGBA->NV12 转换失败 %dx%d stride=%d", w, h, rowStride);
        return;
    }

    auto frame = std::make_shared<core::Frame>();
    frame->kind = core::MediaKind::Video;
    frame->width = w;
    frame->height = h;
    frame->pixFmt = AV_PIX_FMT_NV12;
    frame->ptsUs = ptsUs > 0 ? ptsUs : core::MonotonicClock::NowUs();
    frame->captureUs = core::MonotonicClock::NowUs();
    frame->planes[0] = buffer;
    frame->strides[0] = w;
    frame->planes[1] = std::shared_ptr<uint8_t>(buffer, dstUV);
    frame->strides[1] = w;
    Emit(std::move(frame));
}

}  // namespace TGCapture::platform

extern "C" JNIEXPORT void JNICALL
Java_com_tencent_tgcapture_TGCaptureSystemCapture_nativeOnRgbaFrame(
    JNIEnv* env, jclass, jlong ptr, jobject buf, jint rowStride, jint width, jint height, jlong ptsUs)
{
    auto* self = reinterpret_cast<TGCapture::platform::AndroidScreenCapture*>(ptr);
    if (!self || !buf) {
        return;
    }
    auto* rgba = static_cast<const uint8_t*>(env->GetDirectBufferAddress(buf));
    self->OnRgbaFrame(rgba, rowStride, width, height, ptsUs);
}

extern "C" JNIEXPORT void JNICALL
Java_com_tencent_tgcapture_TGCaptureSystemCapture_nativeOnVideoFrame(
    JNIEnv* env, jclass, jlong ptr, jobject yBuf, jint yStride, jobject uBuf, jint uStride,
    jobject vBuf, jint vStride, jint uvPixelStride, jint width, jint height, jlong ptsUs)
{
    auto* self = reinterpret_cast<TGCapture::platform::AndroidScreenCapture*>(ptr);
    if (!self) {
        return;
    }
    auto* y = static_cast<const uint8_t*>(env->GetDirectBufferAddress(yBuf));
    auto* u = uBuf ? static_cast<const uint8_t*>(env->GetDirectBufferAddress(uBuf)) : nullptr;
    auto* v = vBuf ? static_cast<const uint8_t*>(env->GetDirectBufferAddress(vBuf)) : nullptr;
    self->OnFrame(y, yStride, u, uStride, v, vStride, uvPixelStride, width, height, ptsUs);
}

#endif  // __ANDROID__
