package com.tencent.tgcapture;

/** 封装 / 推流参数（对应 C++ MuxerParams）。 */
public final class MuxerParams {
    /** flv（RTMP / RTMPS）/ mpegts（SRT / 本地 .ts） */
    public String format = "flv";
    /** rtmp:// rtmps:// srt:// 或本地文件绝对路径 */
    public String output = "";
    public int connectTimeoutMs = 10_000;
    public int ioTimeoutMs = 5_000;
}
