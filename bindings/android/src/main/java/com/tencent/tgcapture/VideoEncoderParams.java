package com.tencent.tgcapture;

/** 视频编码参数（对应 C++ VideoEncoderParams 的常用子集）。 */
public final class VideoEncoderParams {
    public static final int PERF_CPU = 0;
    public static final int PERF_GPU = 1;

    public int width = 1280;
    public int height = 720;
    public float fps = 30f;
    public int bitrate = 3_000_000;
    public float bitrateMaxFactor = 1.5f;
    public int gop = 60;
    /** libopenh264（软编）/ h264_mediacodec（硬编） */
    public String codec = "h264_mediacodec";
    /** PERF_CPU = 系统内存帧；PERF_GPU = 硬件帧直通（Android 当前等价 CPU） */
    public int perfMode = PERF_CPU;
    public int threads = 4;
}
