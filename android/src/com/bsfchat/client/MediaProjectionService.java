package com.bsfchat.client;

// Foreground service anchor required by Android 10+ whenever you
// hold a MediaProjection token — the platform refuses to start
// projection otherwise. Type `mediaProjection` on Android 14+ so
// the FOREGROUND_SERVICE_MEDIA_PROJECTION permission check passes.
//
// It is also where the projection is OPENED. From Android 14
// getMediaProjection() throws SecurityException unless a
// `mediaProjection` foreground service is already in the foreground
// state, and Context.startForegroundService() only enqueues
// onStartCommand — it does not make the service foreground
// synchronously. So the caller (ScreenCaptureHelper) hands us the
// consent result as extras and we call back into it from inside
// onStartCommand, after startForeground() has returned. Any other
// ordering is the reverse of what 14+ requires.
//
// Actual frame capture still happens in ScreenCaptureHelper, which
// runs against this service's lifetime.

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;
import android.util.Log;

public class MediaProjectionService extends Service {
    private static final String TAG = "BSFChatScreenCap";
    private static final String CHANNEL_ID = "bsfchat_screen_share";
    private static final int NOTIFICATION_ID = 4203;

    public static final String EXTRA_RESULT_CODE =
        "com.bsfchat.client.extra.RESULT_CODE";
    public static final String EXTRA_RESULT_DATA =
        "com.bsfchat.client.extra.RESULT_DATA";

    @Override
    public IBinder onBind(Intent intent) { return null; }

    @Override
    public void onCreate() {
        super.onCreate();
        ensureChannel();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        Notification n = buildNotification();
        try {
            // minSdk is 28, so the SDK_INT >= Q branch is the only one
            // that can matter for the typed overload;
            // FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION itself is API 29.
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                startForeground(NOTIFICATION_ID, n,
                    ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION);
            } else {
                startForeground(NOTIFICATION_ID, n);
            }
        } catch (Throwable t) {
            // ForegroundServiceStartNotAllowedException (12+) or a
            // missing-type SecurityException (14+). Without this the
            // throw unwinds into ActivityThread and kills the process.
            Log.w(TAG, "startForeground refused: " + t);
            ScreenCaptureHelper.instance().onServiceFailed(
                "Screen sharing could not start");
            stopSelf(startId);
            return START_NOT_STICKY;
        }

        // Now — and only now — is getMediaProjection() legal on 14+.
        if (intent != null && intent.hasExtra(EXTRA_RESULT_DATA)) {
            int resultCode = intent.getIntExtra(EXTRA_RESULT_CODE, 0);
            Intent data = extractResultData(intent);
            if (data != null) {
                ScreenCaptureHelper.instance()
                    .onServiceForegrounded(this, resultCode, data);
            }
        }
        return START_NOT_STICKY;
    }

    private static Intent extractResultData(Intent intent) {
        if (Build.VERSION.SDK_INT >= 33) {
            return intent.getParcelableExtra(EXTRA_RESULT_DATA, Intent.class);
        }
        @SuppressWarnings("deprecation")
        Intent legacy = (Intent) intent.getParcelableExtra(EXTRA_RESULT_DATA);
        return legacy;
    }

    @Override
    public void onDestroy() {
        stopForeground(true);
        super.onDestroy();
    }

    private void ensureChannel() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return;
        NotificationManager nm = getSystemService(NotificationManager.class);
        if (nm == null) return;
        NotificationChannel ch = nm.getNotificationChannel(CHANNEL_ID);
        if (ch == null) {
            ch = new NotificationChannel(
                CHANNEL_ID,
                "Screen sharing",
                NotificationManager.IMPORTANCE_LOW);
            ch.setDescription(
                "Shown while BSFChat is sharing your screen.");
            ch.setShowBadge(false);
            nm.createNotificationChannel(ch);
        }
    }

    private Notification buildNotification() {
        Intent launch = getPackageManager()
            .getLaunchIntentForPackage(getPackageName());
        int flags = PendingIntent.FLAG_UPDATE_CURRENT;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            flags |= PendingIntent.FLAG_IMMUTABLE;
        }
        PendingIntent tapPi = PendingIntent.getActivity(
            this, 0, launch, flags);

        Notification.Builder b;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            b = new Notification.Builder(this, CHANNEL_ID);
        } else {
            b = new Notification.Builder(this);
        }
        b.setContentTitle("BSFChat")
         .setContentText("Sharing your screen")
         .setOngoing(true)
         .setContentIntent(tapPi)
         .setSmallIcon(android.R.drawable.stat_sys_upload);
        return b.build();
    }
}
