package com.tencent.tgcapture;

/**
 * 内核事件回调。
 *
 * <p>线程：onPrepared / onStarted / onStopped（以及 prepare/start 失败时的 onError）在调用
 * prepare/start/stop 的线程上同步触发；运行期的 onError / onNotify 在内核事件分发线程上触发。
 * 回调里不要做重活，需要更新 UI 请自行切回主线程。
 */
public interface TGCaptureListener {
    void onPrepared();

    void onStarted();

    /** err = 本轮运行期最后一次上报的错误码（0 = 全程无错误）。 */
    void onStopped(int err);

    /** what = TGCaptureError（见 include/TGCapture/TGCapturerInterface.h），extra = 底层原始码，仅排障用。 */
    void onError(int what, int extra, String message);

    /** type = NotifyMsgType（如 0 = 首帧编码成功，30/31 = 视频/音频断流恢复）。 */
    void onNotify(int type, String content, long data);
}
