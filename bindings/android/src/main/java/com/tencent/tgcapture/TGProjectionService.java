package com.tencent.tgcapture;

import android.Manifest;
import android.app.Activity;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.ServiceInfo;
import android.media.projection.MediaProjection;
import android.media.projection.MediaProjectionManager;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.util.Log;

/**
 * 持有 MediaProjection 的前台服务。
 *
 * <p>系统要求：Android 10+ 录屏必须在 mediaProjection 类型的前台服务里拿 projection，
 * Android 14 起 {@code getMediaProjection} 必须在 {@code startForeground} 之后调用，否则抛
 * SecurityException。本服务把这套时序封装好，宿主只需要：
 *
 * <pre>
 * // 1. 弹系统授权框
 * startActivityForResult(TGProjectionService.createScreenCaptureIntent(ctx), REQ);
 * // 2. onActivityResult 里
 * TGProjectionService.start(ctx, resultCode, data, callback);   // callback 在主线程回调
 * // 3. 结束开播后
 * TGProjectionService.stop(ctx);
 * </pre>
 *
 * <p>注意：Android 14+ 一个 projection 只能 createVirtualDisplay 一次，每次开播都要重新授权。
 */
public final class TGProjectionService extends Service {
    private static final String TAG = "TGProjectionService";
    private static final String CHANNEL_ID = "tgcapture_projection";
    private static final int NOTIFICATION_ID = 0x7467;
    private static final String EXTRA_RESULT_CODE = "tg.resultCode";
    private static final String EXTRA_RESULT_DATA = "tg.resultData";

    /** 授权结果回调（主线程）。 */
    public interface Callback {
        void onProjectionReady(MediaProjection projection);

        void onProjectionFailed(String reason);
    }

    private static final Handler sMain = new Handler(Looper.getMainLooper());
    private static Callback sPending;
    private static MediaProjection sProjection;

    /** 系统录屏授权 Intent，交给 startActivityForResult / ActivityResultLauncher。 */
    public static Intent createScreenCaptureIntent(Context context) {
        MediaProjectionManager mpm =
                (MediaProjectionManager) context.getSystemService(Context.MEDIA_PROJECTION_SERVICE);
        return mpm.createScreenCaptureIntent();
    }

    /**
     * 用授权结果启动前台服务并取得 MediaProjection。
     * resultCode 不是 RESULT_OK（用户拒绝）时直接回调失败。
     */
    public static void start(Context context, int resultCode, Intent data, Callback callback) {
        if (resultCode != Activity.RESULT_OK || data == null) {
            callback.onProjectionFailed("用户拒绝了录屏授权");
            return;
        }
        synchronized (TGProjectionService.class) {
            sPending = callback;
        }
        Intent intent = new Intent(context, TGProjectionService.class)
                .putExtra(EXTRA_RESULT_CODE, resultCode)
                .putExtra(EXTRA_RESULT_DATA, data);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            context.startForegroundService(intent);
        } else {
            context.startService(intent);
        }
    }

    /** 停止投屏并结束前台服务。 */
    public static void stop(Context context) {
        context.stopService(new Intent(context, TGProjectionService.class));
    }

    /** 当前持有的 projection（未授权 / 已停止时为 null）。 */
    public static synchronized MediaProjection current() {
        return sProjection;
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent == null) {
            // 被系统杀掉后重建：授权数据已失效，不能恢复
            stopSelf();
            return START_NOT_STICKY;
        }
        try {
            enterForeground();
        } catch (RuntimeException e) {
            fail("startForeground 失败: " + e);
            stopSelf();
            return START_NOT_STICKY;
        }

        final int resultCode = intent.getIntExtra(EXTRA_RESULT_CODE, Activity.RESULT_CANCELED);
        final Intent data = intent.getParcelableExtra(EXTRA_RESULT_DATA);
        MediaProjection projection = null;
        try {
            MediaProjectionManager mpm =
                    (MediaProjectionManager) getSystemService(Context.MEDIA_PROJECTION_SERVICE);
            projection = (data != null) ? mpm.getMediaProjection(resultCode, data) : null;
        } catch (RuntimeException e) {
            Log.e(TAG, "getMediaProjection 失败: " + e);
        }
        if (projection == null) {
            fail("getMediaProjection 返回空（授权已被使用过或已失效，需要重新授权）");
            stopSelf();
            return START_NOT_STICKY;
        }

        final MediaProjection ready = projection;
        final Callback cb;
        synchronized (TGProjectionService.class) {
            releaseProjection();
            sProjection = ready;
            cb = sPending;
            sPending = null;
        }
        if (cb != null) {
            sMain.post(() -> cb.onProjectionReady(ready));
        }
        return START_NOT_STICKY;
    }

    @Override
    public void onDestroy() {
        synchronized (TGProjectionService.class) {
            releaseProjection();
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
            stopForeground(STOP_FOREGROUND_REMOVE);
        } else {
            stopForeground(true);
        }
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    private static void releaseProjection() {
        if (sProjection != null) {
            sProjection.stop();
            sProjection = null;
        }
    }

    private static void fail(String reason) {
        Log.e(TAG, reason);
        final Callback cb;
        synchronized (TGProjectionService.class) {
            cb = sPending;
            sPending = null;
        }
        if (cb != null) {
            sMain.post(() -> cb.onProjectionFailed(reason));
        }
    }

    private void enterForeground() {
        Notification notification = buildNotification();
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            int type = ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION;
            // microphone 类型要求 RECORD_AUDIO 已授予，否则 Android 14+ 的 startForeground 直接抛异常
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R
                    && checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED) {
                type |= ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE;
            }
            startForeground(NOTIFICATION_ID, notification, type);
        } else {
            startForeground(NOTIFICATION_ID, notification);
        }
    }

    private Notification buildNotification() {
        Notification.Builder builder;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            NotificationManager nm = (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
            if (nm.getNotificationChannel(CHANNEL_ID) == null) {
                nm.createNotificationChannel(new NotificationChannel(
                        CHANNEL_ID, "直播采集", NotificationManager.IMPORTANCE_LOW));
            }
            builder = new Notification.Builder(this, CHANNEL_ID);
        } else {
            builder = new Notification.Builder(this);
        }
        int icon = getApplicationInfo().icon;
        builder.setSmallIcon(icon != 0 ? icon : android.R.drawable.presence_video_online)
                .setContentTitle(getApplicationInfo().loadLabel(getPackageManager()))
                .setContentText("正在录制屏幕")
                .setOngoing(true);
        Intent launch = getPackageManager().getLaunchIntentForPackage(getPackageName());
        if (launch != null) {
            builder.setContentIntent(PendingIntent.getActivity(this, 0, launch, PendingIntent.FLAG_IMMUTABLE));
        }
        return builder.build();
    }
}
