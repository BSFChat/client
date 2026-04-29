package com.bsfchat.client;

// Minimal foreground service that keeps the BSFChat process alive while
// the user is in a voice channel. Android aggressively kills backgrounded
// apps — including ones holding an open microphone via AudioRecord — so
// a voice call must be anchored by a Service in the foreground state,
// which in turn requires a persistent notification.
//
// On Android 14+ a foreground service that uses the mic also needs the
// `microphone` type and the matching runtime permission; we declare
// both in AndroidManifest.xml. On older releases the type is a no-op.
//
// The service itself has no logic — it exists purely as a lifetime
// anchor. C++ calls startService()/stopService() around VoiceEngine
// start/stop (see AndroidAudioRouting).
//
// Kept in com.bsfchat.client rather than org.qtproject.*, so the class
// survives Qt bumps.

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
import android.os.PowerManager;

public class VoiceService extends Service {
    private static final String CHANNEL_ID = "bsfchat_voice";
    private static final int NOTIFICATION_ID = 4201;

    // Partial wake lock held for the lifetime of the service so the
    // CPU stays awake while voice is running. Foreground-service
    // state alone doesn't prevent dozing — Android may throttle our
    // network and audio threads when the screen is off otherwise.
    // We hold a PARTIAL lock (no screen, no keyboard) so battery
    // impact is the minimum needed to keep voice flowing.
    private PowerManager.WakeLock wakeLock;

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public void onCreate() {
        super.onCreate();
        ensureChannel();

        PowerManager pm = (PowerManager) getSystemService(POWER_SERVICE);
        if (pm != null) {
            wakeLock = pm.newWakeLock(
                PowerManager.PARTIAL_WAKE_LOCK,
                "bsfchat:voice");
            wakeLock.setReferenceCounted(false);
            wakeLock.acquire(60 * 60 * 1000L); // 1h max; renewed below
        }
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        Notification n = buildNotification();
        // On API 30+ we must pass the type to startForeground() so the
        // platform knows which foreground-service-type permission to
        // check. `microphone` is declared in the manifest.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            int type = 0;
            try {
                type = ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE;
                // OR in camera so a single FGS can legitimately
                // carry both mic and camera streams during a call.
                // Constant added in API 30 (Q+CAMERA is API 29 but
                // the FGS type is API 30); wrap separately so
                // older runtimes still get at least the mic type.
                try {
                    type |= ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA;
                } catch (NoSuchFieldError ignored) { /* API 29 */ }
            } catch (NoSuchFieldError ignored) {
                // Older runtime without the constant — leave type=0.
            }
            if (type != 0) {
                startForeground(NOTIFICATION_ID, n, type);
            } else {
                startForeground(NOTIFICATION_ID, n);
            }
        } else {
            startForeground(NOTIFICATION_ID, n);
        }
        // START_STICKY means if the system kills us under memory pressure
        // it'll try to restart us — but VoiceEngine's network state won't
        // have survived, so the user will land in a broken room. We
        // accept that: a stale "in voice" banner is less confusing than
        // being silently dropped mid-call.
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        if (wakeLock != null && wakeLock.isHeld()) {
            wakeLock.release();
            wakeLock = null;
        }
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
                "Voice calls",
                NotificationManager.IMPORTANCE_LOW);
            ch.setDescription(
                "Shown while BSFChat is connected to a voice channel.");
            ch.setShowBadge(false);
            nm.createNotificationChannel(ch);
        }
    }

    private Notification buildNotification() {
        // Tap → back into the app.
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
         .setContentText("Connected to voice")
         .setOngoing(true)
         .setContentIntent(tapPi)
         .setSmallIcon(android.R.drawable.presence_audio_online);
        return b.build();
    }
}
