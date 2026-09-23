package com.bsfchat.client;

// Minimal foreground service that keeps the BSFChat process alive while
// the user is in a voice channel. Android aggressively kills backgrounded
// apps — including ones holding an open microphone via AudioRecord — so
// a voice call must be anchored by a Service in the foreground state,
// which in turn requires a persistent notification.
//
// ── Foreground-service types, and why they are computed at runtime ──
//
// The manifest declares foregroundServiceType="microphone|camera" so a
// single FGS can carry both streams during a call. What is passed to
// startForeground() is NOT that whole set: from Android 14 the platform
// requires the matching RUNTIME permission to be held for every type in
// the call, and throws SecurityException otherwise. A user joins voice
// having granted RECORD_AUDIO and (almost always) nothing else, so a
// fixed `MICROPHONE|CAMERA` would throw on every plain voice join on
// Android 14+ — the common path, not an edge case.
//
// So each type is included only when its permission is actually held,
// and the set is recomputed on every onStartCommand. Turning the camera
// on mid-call re-starts the service (see refreshForegroundType and
// bsfchat::audio_routing::refreshVoiceService), which re-runs this and
// adds the camera bit at the point it becomes both needed and legal.
//
// Note also that FOREGROUND_SERVICE_TYPE_MICROPHONE and
// FOREGROUND_SERVICE_TYPE_CAMERA are API 30 constants, not API 29 —
// minSdk is 28, so 28/29 devices exist in range and get the untyped
// startForeground() overload.
//
// The service itself has no other logic — it exists purely as a
// lifetime anchor. C++ calls startService()/stopService() around
// VoiceEngine start/stop (see AndroidAudioRouting).
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
import android.content.pm.PackageManager;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.PowerManager;
import android.util.Log;

public class VoiceService extends Service {
    private static final String TAG = "BSFChatVoice";
    private static final String CHANNEL_ID = "bsfchat_voice";
    private static final int NOTIFICATION_ID = 4201;

    // Partial wake lock held for the lifetime of the service so the
    // CPU stays awake while voice is running. Foreground-service
    // state alone doesn't prevent dozing — Android may throttle our
    // network and audio threads when the screen is off otherwise.
    // We hold a PARTIAL lock (no screen, no keyboard) so battery
    // impact is the minimum needed to keep voice flowing.
    private PowerManager.WakeLock wakeLock;

    // acquire() takes a bound so a leaked lock cannot flatten the
    // battery forever, and the bound has to be renewed or a call
    // longer than it silently loses the lock — which is exactly the
    // "voice got choppy after an hour with the screen off" shape. The
    // old comment here said "renewed below" and nothing renewed it.
    private static final long WAKE_LOCK_BOUND_MS = 60 * 60 * 1000L;
    private static final long WAKE_LOCK_RENEW_MS = 30 * 60 * 1000L;
    private final Handler mMain = new Handler(Looper.getMainLooper());
    private final Runnable mRenewWakeLock = new Runnable() {
        @Override
        public void run() {
            if (wakeLock == null) return;
            try {
                wakeLock.acquire(WAKE_LOCK_BOUND_MS);
            } catch (Throwable t) {
                Log.w(TAG, "wake lock renew failed: " + t);
            }
            mMain.postDelayed(this, WAKE_LOCK_RENEW_MS);
        }
    };

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
            wakeLock.acquire(WAKE_LOCK_BOUND_MS);
            mMain.postDelayed(mRenewWakeLock, WAKE_LOCK_RENEW_MS);
        }
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        Notification n = buildNotification();
        try {
            int type = foregroundTypeFor(this);
            if (type != 0) {
                startForeground(NOTIFICATION_ID, n, type);
            } else {
                startForeground(NOTIFICATION_ID, n);
            }
        } catch (Throwable t) {
            // ForegroundServiceStartNotAllowedException (12+) or a
            // type/permission SecurityException (14+). Uncaught, this
            // unwinds into ActivityThread and kills the process
            // mid-call. Losing the anchor means the call will not
            // survive backgrounding, which is far better than losing
            // the app.
            Log.w(TAG, "startForeground refused; the call will not "
                       + "survive backgrounding: " + t);
            stopSelf(startId);
            return START_NOT_STICKY;
        }
        // START_STICKY means if the system kills us under memory pressure
        // it'll try to restart us — but VoiceEngine's network state won't
        // have survived, so the user will land in a broken room. We
        // accept that: a stale "in voice" banner is less confusing than
        // being silently dropped mid-call.
        return START_STICKY;
    }

    // Which foreground-service types we may legally claim right now.
    // Returns 0 to mean "use the untyped overload".
    private static int foregroundTypeFor(Context ctx) {
        // The typed constants used here (MICROPHONE=128, CAMERA=64) are
        // API 30. On 28/29 the untyped overload is the correct call.
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return 0;
        int type = 0;
        if (granted(ctx, android.Manifest.permission.RECORD_AUDIO))
            type |= ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE;
        if (granted(ctx, android.Manifest.permission.CAMERA))
            type |= ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA;
        return type;
    }

    private static boolean granted(Context ctx, String permission) {
        return ctx.checkSelfPermission(permission)
            == PackageManager.PERMISSION_GRANTED;
    }

    // Re-deliver a start command so onStartCommand recomputes the type
    // set. Called when the camera is turned on part-way through a call,
    // at which point CAMERA has just been granted and the camera bit
    // becomes both necessary and legal to claim.
    public static void refreshForegroundType(Context ctx) {
        if (ctx == null) return;
        try {
            ctx.startForegroundService(
                new Intent(ctx, VoiceService.class));
        } catch (Throwable t) {
            Log.w(TAG, "foreground-type refresh refused: " + t);
        }
    }

    @Override
    public void onDestroy() {
        mMain.removeCallbacks(mRenewWakeLock);
        if (wakeLock != null && wakeLock.isHeld()) {
            wakeLock.release();
        }
        wakeLock = null;
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
