package com.bsfchat.client;

// Foreground "sync" service — sibling of VoiceService but for the
// general Matrix /sync loop. Exists purely to keep the process alive
// when the user backgrounds the app so inbound messages keep flowing
// and we can post notifications for them.
//
// Why not FCM: BSFChat is self-hosted. Routing every user's push
// traffic through Google infrastructure would betray the project's
// own-your-server promise. A persistent foreground service keeps
// delivery entirely within the client↔server trust boundary at the
// cost of some battery (the Matrix /sync long-poll is cheap — most
// of the time we're idle on a server-held socket).
//
// The notification type is `dataSync` (Android 14+) — chosen
// deliberately over `dataSync` because `dataSync` matches the
// platform's expectation of "periodic background work that
// downloads content". Our work IS that. If Google ever tightens
// the runtime restrictions on it we'll revisit.

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;

public class SyncService extends Service {
    private static final String CHANNEL_ID = "bsfchat_sync";
    private static final int NOTIFICATION_ID = 4202;

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

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
                type = ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC;
            } catch (NoSuchFieldError ignored) {
                // Older runtime — type=0 is fine.
            }
            if (type != 0) {
                startForeground(NOTIFICATION_ID, n, type);
            } else {
                startForeground(NOTIFICATION_ID, n);
            }
        } else {
            startForeground(NOTIFICATION_ID, n);
        }
        // NOT_STICKY: if we're killed the user will notice messages
        // stopping and can foreground the app to re-establish — better
        // than silently re-launching into a broken half-state.
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
                "Background sync",
                NotificationManager.IMPORTANCE_MIN);
            ch.setDescription(
                "Shown while BSFChat is keeping messages in sync.");
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
         .setContentText("Keeping messages in sync")
         .setOngoing(true)
         .setPriority(Notification.PRIORITY_MIN)
         .setContentIntent(tapPi)
         .setSmallIcon(android.R.drawable.stat_notify_sync);
        return b.build();
    }
}
