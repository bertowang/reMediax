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
};

}  // namespace TGCapture::platform

#endif  // __ANDROID__
#endif  // TGCAPTURE_PLATFORM_ANDROID_JNI_H
