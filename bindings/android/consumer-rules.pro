# JNI 反射调用的类与方法不能被混淆 / 裁剪（宿主开 R8 时由本文件自动生效）
-keep class com.tencent.tgcapture.TGCaptureSystemCapture { *; }
-keep class com.tencent.tgcapture.TGCapturer { *; }
-keep interface com.tencent.tgcapture.TGCaptureListener { *; }
# getOverlays 在 JNI 里按字段名构造 OverlayDesc
-keep class com.tencent.tgcapture.OverlayDesc { *; }
-keepclasseswithmembernames class com.tencent.tgcapture.** {
    native <methods>;
}
