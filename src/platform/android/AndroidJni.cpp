#ifdef __ANDROID__

#include "platform/android/AndroidJni.h"

#include <atomic>

namespace TGCapture::platform
{

namespace
{
std::atomic<JavaVM*> g_vm{nullptr};
std::atomic<jclass> g_helperClass{nullptr};
thread_local bool tls_attached = false;
constexpr const char* kHelperClass = "com/tencent/tgcapture/TGCaptureSystemCapture";
}  // namespace

void AndroidJni::SetHelperClass(JNIEnv* env, jclass clazz)
{
    if (!env || !clazz) {
        return;
    }
    auto global = static_cast<jclass>(env->NewGlobalRef(clazz));
    if (jclass old = g_helperClass.exchange(global, std::memory_order_acq_rel)) {
        env->DeleteGlobalRef(old);
    }
}

jclass AndroidJni::HelperClass(JNIEnv* env)
{
    if (!env) {
        return nullptr;
    }
    if (jclass cached = g_helperClass.load(std::memory_order_acquire)) {
        return static_cast<jclass>(env->NewLocalRef(cached));
    }
    jclass clazz = env->FindClass(kHelperClass);
    if (!clazz && env->ExceptionCheck()) {
        env->ExceptionClear();   // 不清掉的话后续任何 JNI 调用都是未定义行为
    }
    return clazz;
}

void AndroidJni::SetJavaVM(JavaVM* vm) noexcept
{
    g_vm.store(vm, std::memory_order_release);
}

JavaVM* AndroidJni::GetJavaVM() noexcept
{
    return g_vm.load(std::memory_order_acquire);
}

JNIEnv* AndroidJni::Env()
{
    JavaVM* vm = GetJavaVM();
    if (!vm) {
        return nullptr;
    }
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK && env) {
        return env;
    }
    if (vm->AttachCurrentThread(&env, nullptr) == JNI_OK && env) {
        tls_attached = true;
        return env;
    }
    return nullptr;
}

void AndroidJni::DetachIfNeeded()
{
    if (!tls_attached) {
        return;
    }
    if (JavaVM* vm = GetJavaVM()) {
        vm->DetachCurrentThread();
    }
    tls_attached = false;
}

}  // namespace TGCapture::platform

#endif  // __ANDROID__
