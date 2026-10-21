#ifdef __ANDROID__

#include "platform/android/AndroidJni.h"

#include <atomic>

namespace TGCapture::platform
{

namespace
{
std::atomic<JavaVM*> g_vm{nullptr};
thread_local bool tls_attached = false;
}  // namespace

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
