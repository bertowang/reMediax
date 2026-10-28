// Android JNI 环境封装：C++ 侧调用 Java（MediaProjection / AudioRecord）与接收 Java 回调都依赖它。
#ifndef TGCAPTURE_PLATFORM_ANDROID_JNI_H
#define TGCAPTURE_PLATFORM_ANDROID_JNI_H

#ifdef __ANDROID__

#include <jni.h>

namespace TGCapture::platform
{

class AndroidJni
{
public:
    // 由 App 在 JNI_OnLoad 或创建 capturer 后通过 SetPrivateData("android.JavaVM", vm) 传入
    static void SetJavaVM(JavaVM* vm) noexcept;
    static JavaVM* GetJavaVM() noexcept;

    // 取当前线程 JNIEnv（非 Java 线程会自动 Attach）
    static JNIEnv* Env();

    // 与 Env() 成对使用（仅当本线程是被 Attach 的才 Detach）
    static void DetachIfNeeded();

    // 采集桥接类 com.tencent.tgcapture.TGCaptureSystemCapture 的全局引用。
    // 由 JNI_OnLoad（bindings/android）在 App 的 ClassLoader 上预先解析并缓存：
    // 被 AttachCurrentThread 挂上来的 native 线程里调 FindClass 走的是系统 ClassLoader，
    // 找不到 App 自己的类。未缓存时（宿主自己加载 so、没走我们的 JNI_OnLoad）回退 FindClass。
    static void SetHelperClass(JNIEnv* env, jclass clazz);
    // 返回 local ref（调用方 DeleteLocalRef），失败返回 nullptr
    static jclass HelperClass(JNIEnv* env);
};

}  // namespace TGCapture::platform

#endif  // __ANDROID__
#endif  // TGCAPTURE_PLATFORM_ANDROID_JNI_H
