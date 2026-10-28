// TGCapture 开播测试 App（Android）
// 依赖同工程的 :tgcapture 模块（bindings/android），改内核 C++ 后直接 Run 即可增量重编 .so。
import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
}

android {
    namespace = "com.tencent.tgcapture.demo"
    compileSdk = 36
    buildToolsVersion = "36.0.0"
    ndkVersion = (findProperty("tg.ndkVersion") as String?) ?: "29.0.14033849"

    defaultConfig {
        applicationId = "com.tencent.tgcapture.demo"
        minSdk = 23
        targetSdk = 36
        versionCode = 1
        versionName = "1.0"
        ndk {
            abiFilters += listOf("arm64-v8a")
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            // 测试 App：release 也用 debug 签名，方便直接安装
            signingConfig = signingConfigs.getByName("debug")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    buildFeatures {
        viewBinding = true
    }
}

kotlin {
    compilerOptions {
        jvmTarget.set(JvmTarget.JVM_17)
    }
}

dependencies {
    implementation(project(":tgcapture"))
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.appcompat)
    implementation(libs.androidx.activity.ktx)
    implementation(libs.androidx.preference.ktx)
    implementation(libs.material)
}
