package com.bsfchat.client;

// Foreground service anchor required by Android 10+ whenever you
// hold a MediaProjection token — the platform refuses to start
// projection otherwise. Type `mediaProjection` on Android 14+ so
// the FOREGROUND_SERVICE_MEDIA_PROJECTION permission check passes.
//
// Contents are intentionally tiny: the service exists only to own
// the persistent "BSFChat is sharing your screen" notification
// that justifies keeping the projection alive while the user
// switches apps. Actual frame capture happens in
// ScreenCaptureHelper which runs against this service's lifetime.

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;

public class MediaProjectionService extends Service {
    private static final String CHANNEL_ID = "bsfchat_screen_share";
    private static final int NOTIFICATION_ID = 4203;

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
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            int type = 0;
            try {
                type = ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION;
            } catch (NoSuchFieldError ignored) { /* older runtime */ }
            if (type != 0) startForeground(NOTIFICATION_ID, n, type);
            else startForeground(NOTIFICATION_ID, n);
        } else {
            startForeground(NOTIFICATION_ID, n);
        }
        return START_NOT_STICKY;
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
