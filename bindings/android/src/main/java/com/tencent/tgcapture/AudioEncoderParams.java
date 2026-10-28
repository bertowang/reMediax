package com.tencent.tgcapture;

/** 音频编码参数（对应 C++ AudioEncoderParams）。 */
public final class AudioEncoderParams {
    public int sampleRate = 44100;
    public int channels = 2;
    public int bitrate = 128_000;
    /** libfdk_aac（默认）/ aac（ffmpeg 内置）；不可用时内核自动回退 aac */
    public String codec = "libfdk_aac";
    /** false = 纯视频（不建音频编码器与音频流） */
    public boolean enabled = true;
}
