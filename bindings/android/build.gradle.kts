// TGCapture Android SDK 模块（产出 AAR）
//   Java API      src/main/java/com/tencent/tgcapture/*
//   JNI 胶水      src/main/cpp/tgcapture_jni.cpp  -> libtgcapture_jni.so（静态链入内核 libtgcapture.a + ffmpeg 等）
//   内核          仓库根 CMakeLists.txt（由 src/main/cpp/CMakeLists.txt add_subdirectory 引入）
//
// 依赖目录 TGCapture3rd：-PTG_3RD_ROOT=<dir> > 环境变量 TG_3RD_ROOT > 仓库同级 TGCapture3rd（CMake 自动探测）
plugins {
    alias(libs.plugins.android.library)
}

val thirdRoot: String? = (findProperty("TG_3RD_ROOT") as String?)?.takeIf { it.isNotBlank() }
    ?: System.getenv("TG_3RD_ROOT")?.takeIf { it.isNotBlank() }

android {
    namespace = "com.tencent.tgcapture"
    compileSdk = 36
    buildToolsVersion = "36.0.0"
    // 与 FfmpegCompileShell 编依赖时用的 NDK 保持一致；本机版本不同可在 gradle.properties 里覆盖 tg.ndkVersion
    ndkVersion = (findProperty("tg.ndkVersion") as String?) ?: "29.0.14033849"

    defaultConfig {
        // TGCapture3rd 的 android 依赖按 api23 编译，低于 23 会链接到不存在的 libc 符号
        minSdk = 23
        consumerProguardFiles("consumer-rules.pro")
        ndk {
            // 换 / 加 ABI 只改这里：CMake 侧 find_package(TGCapture3rd) 按 ANDROID_ABI 选 TGCapture3rd/<lib>/android/<abi>
            abiFilters += listOf("arm64-v8a")
        }
        externalNativeBuild {
            cmake {
                arguments += listOf("-DANDROID_STL=c++_shared")
                // 依赖根目录即 CMake 包前缀（含 TGCapture3rdConfig.cmake），由 cmake/Deps.cmake 作为 find_package 的 HINTS
                thirdRoot?.let { arguments += "-DTG_3RD_ROOT=$it" }
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = (findProperty("tg.cmakeVersion") as String?) ?: "4.1.1"
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}
