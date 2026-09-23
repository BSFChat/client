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
// The foreground-service type is `dataSync`: the platform's own
// description of it is "periodic background work that downloads
// content", which is what a /sync long-poll is.
//
// ── The Android 15 budget, and what we do when it runs out ──
//
// From Android 15 (API 35) a `dataSync` foreground service may run
// for roughly six hours in any 24-hour window. When the budget is
// spent the system calls Service.onTimeout(startId, fgsType) and the
// app has a few seconds to stop itself; miss that and the platform
// throws ForegroundServiceDidNotStopInTimeException at the process.
// Starting another dataSync FGS before the window rolls over is
// refused with ForegroundServiceStartNotAllowedException.
//
// So the answer is graceful degradation, not a restart loop: we stop
// ourselves promptly, tell the native side (AndroidNotifier), and it
// leaves background sync off until the app is next brought to the
// foreground — at which point it tries exactly once more and stays
// degraded if the budget is still spent. Foreground sync is
// unaffected: the /sync loop belongs to the C++ side and keeps
// running for as long as the process does. FCM is deliberately not
// the fallback; see above.
//
// onTimeout is declared WITHOUT @Override on purpose. The local
// toolchain compiles against an older platform than API 35, where the
// two-argument form does not exist, and @Override would be a compile
// error there. Virtual dispatch is by name and descriptor at runtime,
// so the override still takes effect on a device that calls it.

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
import android.util.Log;

public class SyncService extends Service {
    private static final String TAG = "BSFChatSync";
    private static final String CHANNEL_ID = "bsfchat_sync";
    private static final int NOTIFICATION_ID = 4202;

    // Reasons handed to the native side. Kept as plain strings so the
    // C++ end can log them verbatim and map the first to "degraded".
    private static final String REASON_TIMEOUT = "dataSync-budget-exhausted";
    private static final String REASON_START_REFUSED = "start-refused";

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
        try {
            // minSdk is 28; FOREGROUND_SERVICE_TYPE_DATA_SYNC is API 29.
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                startForeground(NOTIFICATION_ID, n,
                    ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC);
            } else {
                startForeground(NOTIFICATION_ID, n);
            }
        } catch (Throwable t) {
            // ForegroundServiceStartNotAllowedException (Android 12+)
            // if the start slipped through while backgrounded, or the
            // Android 15 refusal once the dataSync budget is spent.
            // Uncaught, this unwinds into ActivityThread and takes the
            // whole process with it — which is the failure mode this
            // catch exists to prevent.
            Log.w(TAG, "startForeground refused: " + t);
            notifyNativeStopped(REASON_START_REFUSED);
            stopSelf(startId);
            return START_NOT_STICKY;
        }
        // NOT_STICKY: if we're killed the user will notice messages
        // stopping and can foreground the app to re-establish — better
        // than silently re-launching into a broken half-state.
        return START_NOT_STICKY;
    }

    // Android 15+ (API 35): the dataSync budget is spent. Stop within
    // the grace period or the platform crashes us.
    // No @Override — see the class comment.
    public void onTimeout(int startId, int fgsType) {
        Log.w(TAG, "dataSync foreground budget exhausted (type="
                   + fgsType + "); stopping background sync");
        handleTimeout(startId);
    }

    // Android 14 (API 34) shipped the one-argument form (for
    // shortService). Harmless to carry: if a platform ever routes a
    // dataSync timeout through it we behave identically.
    // No @Override — see the class comment.
    public void onTimeout(int startId) {
        Log.w(TAG, "foreground service timed out; stopping background sync");
        handleTimeout(startId);
    }

    private void handleTimeout(int startId) {
        // Tell C++ first: the call is fire-and-forget (it posts onto the
        // Qt event loop and returns), so it cannot eat the grace period.
        notifyNativeStopped(REASON_TIMEOUT);
        try {
            stopForeground(true);
        } catch (Throwable ignored) { }
        stopSelf(startId);
    }

    @Override
    public void onDestroy() {
        stopForeground(true);
        super.onDestroy();
    }

    private static void notifyNativeStopped(String reason) {
        try {
            nativeOnSyncServiceStopped(reason);
        } catch (Throwable t) {
            // UnsatisfiedLinkError if the shared library never got as
            // far as registering its bridges. Not worth dying for.
            Log.w(TAG, "native sync-stop hook unavailable: " + t);
        }
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

    // Implemented C++-side in src/core/AndroidNotifier.cpp. Tells the
    // app that background sync has stopped and why, so it can degrade
    // to foreground-only sync rather than believing it is still live.
    private static native void nativeOnSyncServiceStopped(String reason);
}
