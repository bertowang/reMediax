// TGCapture Android 根工程（Android Studio 打开 ide/android 这一层）
//
// 只做聚合，不放源码：
//   :tgcapture  -> bindings/android   SDK 模块（AAR）：Java API + JNI 胶水 + 内核 libtgcapture_jni.so
//   :demo       -> demo/android       开播测试 App（依赖 :tgcapture）
// 内核 C++ 的构建逻辑只在仓库根 CMakeLists.txt 一处，由 :tgcapture 的 externalNativeBuild 驱动。
pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "TGCapture"

val repoRoot = settingsDir.resolve("../..").canonicalFile

include(":tgcapture")
project(":tgcapture").projectDir = repoRoot.resolve("bindings/android")

include(":demo")
project(":demo").projectDir = repoRoot.resolve("demo/android")
