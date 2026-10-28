// 插件版本统一在 gradle/libs.versions.toml 里管理，这里只声明、不应用
plugins {
    alias(libs.plugins.android.application) apply false
    alias(libs.plugins.android.library) apply false
    alias(libs.plugins.kotlin.android) apply false
}
