package com.bsfchat.client;

// Java half of mobile push on Android. Compiled into every build; the
// Firebase-specific code lives in FcmPush / PushMessagingService, which
// cmake/AndroidPush.cmake stages from android-push/fcm/ when the four
// BSFCHAT_FCM_* values are supplied and from android-push/stub/ (no
// Firebase classes at all) when they are not. See
// docs/push-notifications.md.
//
// Three jobs:
//
//   * Token plumbing. The FCM registration token arrives in
//     PushMessagingService.onNewToken or from FcmPush.requestToken, often
//     before (or without) Qt running. It is always written to
//     SharedPreferences, and handed to C++ (nativeOnPushToken) when the
//     native side is up; start() replays the stored one on every launch.
//   * Message handling (onMessage, called from onMessageReceived on a
//     Firebase worker thread). What Google delivered carries no message
//     text: the server sends event_id_only unless the user opted into
//     names. App in the foreground: nothing to do, the live /sync shows it.
//     Process alive with Qt running: C++ kicks its /sync, and if the event
//     comes in it raises the normal, full notification itself from local
//     data; this thread waits for that answer (up to ~8 s of Firebase's
//     ~10 s) so the process is not frozen mid-fetch. Otherwise a generic
//     notification is posted here.
//   * Preferences C++ mirrors in (notifications on/off, names tier), so a
//     push that arrives with Qt not running still honours them.

import android.app.ActivityManager;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.net.Uri;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

public final class PushBridge {
    private static final String TAG = "BSFChatPush";
    private static final String PREFS = "bsfchat_push";
    private static final String KEY_TOKEN = "token";
    private static final String KEY_ENABLED = "enabled";
    private static final String KEY_NAMES = "names";

    // Same channel AndroidNotifier.cpp posts chat notifications to, created
    // with the same name and importance so whichever side runs first wins
    // and the user sees one "Chat messages" channel.
    private static final String CHAT_CHANNEL_ID = "bsfchat_chat";
    private static final String CHAT_CHANNEL_NAME = "Chat messages";
    // Tag prefix for notifications posted from here. NotificationManager
    // .cpp cancels "push|<room>" alongside its own tag when the room opens.
    private static final String TAG_PREFIX = "push|";

    // How long onMessage waits for the native side to say whether its /sync
    // delivered the event. Firebase allows roughly ten seconds.
    private static final long NATIVE_WAIT_MS = 8000;

    // Token request retries (audit 3: one transient failure used to mark
    // push unavailable for the whole session). Backoff per attempt; the
    // last value repeats. After GIVE_UP_AFTER failures the native side is
    // told push is unavailable, but retrying carries on at the slow rate
    // and a later token clears it.
    private static final long[] RETRY_MS = { 10_000, 30_000, 120_000, 600_000, 1_800_000 };
    private static final int GIVE_UP_AFTER = 6;
    private static final Handler sMain = new Handler(Looper.getMainLooper());
    private static volatile boolean sWanted = false;
    private static int sFailures = 0;           // main thread only
    private static Runnable sRetry = null;      // main thread only

    private static volatile boolean sNativeReady = false;
    private static final Map<String, CountDownLatch> sWaiting = new ConcurrentHashMap<>();
    private static final Map<String, Boolean> sHandled = new ConcurrentHashMap<>();

    private PushBridge() { }

    // ── Called from C++ (src/core/PushBridge.cpp) ────────────────────────

    // After the native methods below are registered.
    public static void setNativeReady(boolean ready) {
        sNativeReady = ready;
    }

    public static boolean configured() {
        return FcmPush.configured();
    }

    // Every launch, and whenever "wanted" changes. wanted=false (the user
    // keeps a background connection instead, or turned notifications off)
    // stops Firebase from minting tokens and deletes the current one, so the
    // device stops talking to Google for this app.
    public static void start(Context ctx, boolean wanted) {
        if (!FcmPush.configured()) {
            notifyUnavailable("not-configured");
            return;
        }
        sWanted = wanted;
        sMain.post(new Runnable() {
            @Override public void run() { cancelRetry(); sFailures = 0; }
        });
        String stored = prefs(ctx).getString(KEY_TOKEN, null);
        if (!wanted) {
            // Only if there is a token to delete: a user who never used
            // push must not have Firebase contacted on every launch.
            if (stored != null && !stored.isEmpty()) FcmPush.disable(ctx);
            return;
        }
        if (stored != null && !stored.isEmpty()) notifyToken(stored);
        FcmPush.requestToken(ctx);
    }

    public static void setPrefs(Context ctx, boolean enabled, boolean names) {
        prefs(ctx).edit().putBoolean(KEY_ENABLED, enabled).putBoolean(KEY_NAMES, names).apply();
    }

    // The native side's answer for one pushed event: handled=true means its
    // /sync saw the event and the regular notification path took it from
    // there (including deciding not to notify, e.g. a muted room).
    public static void onNativeDone(String eventId, boolean handled) {
        if (eventId == null) return;
        CountDownLatch latch = sWaiting.get(eventId);
        if (latch == null) return;   // already gave up waiting
        sHandled.put(eventId, handled);
        latch.countDown();
    }

    // Android 13+ POST_NOTIFICATIONS denied, or notifications switched off
    // for the app in system settings. Push would still arrive and show
    // nothing, so Settings says so (audit 3).
    public static boolean notificationsBlocked(Context ctx) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.N) return false;
        NotificationManager nm =
            (NotificationManager) ctx.getSystemService(Context.NOTIFICATION_SERVICE);
        return nm != null && !nm.areNotificationsEnabled();
    }

    public static void openNotificationSettings(Context ctx) {
        Intent i;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            i = new Intent(android.provider.Settings.ACTION_APP_NOTIFICATION_SETTINGS);
            i.putExtra(android.provider.Settings.EXTRA_APP_PACKAGE, ctx.getPackageName());
        } else {
            i = new Intent(android.provider.Settings.ACTION_APPLICATION_DETAILS_SETTINGS);
            i.setData(Uri.parse("package:" + ctx.getPackageName()));
        }
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        try {
            ctx.startActivity(i);
        } catch (RuntimeException e) {
            Log.w(TAG, "cannot open notification settings: " + e);
        }
    }

    public static String deviceName() {
        String maker = Build.MANUFACTURER == null ? "" : Build.MANUFACTURER;
        String model = Build.MODEL == null ? "" : Build.MODEL;
        if (model.toLowerCase().startsWith(maker.toLowerCase())) return model;
        return (maker + " " + model).trim();
    }

    // ── Called from FcmPush / PushMessagingService ──────────────────────

    static void onToken(Context ctx, String token) {
        if (token == null || token.isEmpty()) return;
        sMain.post(new Runnable() {
            @Override public void run() { cancelRetry(); sFailures = 0; }
        });
        prefs(ctx).edit().putString(KEY_TOKEN, token).apply();
        notifyToken(token);
    }

    static void onTokenDeleted(Context ctx) {
        prefs(ctx).edit().remove(KEY_TOKEN).apply();
        notifyToken("");
    }

    static void onUnavailable(String reason) {
        Log.w(TAG, "push unavailable: " + reason);
        notifyUnavailable(reason == null ? "unknown" : reason);
    }

    // FcmPush.requestToken's failure. Only a device that cannot do FCM at
    // all is final; anything else (no network yet, Play services busy or
    // updating, a timeout) is retried with backoff.
    static void onTokenFailed(final Context ctx, final String reason) {
        if (isPermanent(reason)) {
            onUnavailable(reason);
            return;
        }
        final Context app = ctx.getApplicationContext();
        sMain.post(new Runnable() {
            @Override public void run() {
                if (!sWanted) return;
                long delay = RETRY_MS[Math.min(sFailures, RETRY_MS.length - 1)];
                ++sFailures;
                Log.w(TAG, "token request failed (" + reason + "); retry " + sFailures
                           + " in " + (delay / 1000) + " s");
                if (sFailures >= GIVE_UP_AFTER) notifyUnavailable(reason);
                else notifyRetrying(reason);
                cancelRetry();
                sRetry = new Runnable() {
                    @Override public void run() {
                        sRetry = null;
                        if (sWanted) FcmPush.requestToken(app);
                    }
                };
                sMain.postDelayed(sRetry, delay);
            }
        });
    }

    private static boolean isPermanent(String reason) {
        if (reason == null) return false;
        // No Google Play services / no FCM on this device, or a build whose
        // Firebase configuration is wrong. Neither improves with time.
        return reason.contains("MISSING_INSTANCEID_SERVICE")
            || reason.contains("INVALID_SENDER")
            || reason.contains("NoClassDefFoundError")
            || reason.contains("API key not valid");
    }

    private static void cancelRetry() {
        if (sRetry != null) {
            sMain.removeCallbacks(sRetry);
            sRetry = null;
        }
    }

    // Runs on a Firebase worker thread, never the main thread.
    static void onMessage(Context ctx, Map<String, String> data) {
        SharedPreferences p = prefs(ctx);
        if (!p.getBoolean(KEY_ENABLED, true)) return;

        String roomId = field(data, "room_id");
        String eventId = field(data, "event_id");
        int unread = unreadCount(data);
        if (roomId == null) return;   // nothing we can show or open

        if (isForeground(ctx)) return;   // the live /sync shows it

        if (sNativeReady && eventId != null) {
            CountDownLatch latch = new CountDownLatch(1);
            sWaiting.put(eventId, latch);
            boolean accepted = false;
            try {
                accepted = nativeOnPushMessage(roomId, eventId, unread);
                if (accepted) latch.await(NATIVE_WAIT_MS, TimeUnit.MILLISECONDS);
            } catch (UnsatisfiedLinkError e) {
                sNativeReady = false;
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            } finally {
                sWaiting.remove(eventId);
            }
            Boolean handled = sHandled.remove(eventId);
            if (accepted && handled != null && handled) return;
        }

        boolean names = p.getBoolean(KEY_NAMES, false);
        postGeneric(ctx, roomId, eventId, unread,
                    names ? field(data, "sender_display_name") : null,
                    names ? field(data, "room_name") : null);
    }

    // ── Helpers ─────────────────────────────────────────────────────────

    // Sygnal's FCM v1 path sends every data value as a string, and may send
    // an absent field as "None" or "null"; treat those as absent.
    private static String field(Map<String, String> data, String key) {
        if (data == null) return null;
        String v = data.get(key);
        if (v == null) return null;
        v = v.trim();
        if (v.isEmpty() || v.equals("None") || v.equals("null")) return null;
        return v;
    }

    // Sygnal flattens counts.unread to a top-level "unread"; accept the
    // nested Matrix spelling too in case a gateway passes counts through.
    private static int unreadCount(Map<String, String> data) {
        String v = field(data, "unread");
        if (v == null) v = field(data, "unread_count");
        if (v == null) {
            String counts = field(data, "counts");
            if (counts != null) {
                try {
                    v = String.valueOf(new org.json.JSONObject(counts).optInt("unread", 0));
                } catch (org.json.JSONException ignored) { }
            }
        }
        if (v == null) return 0;
        try {
            return Math.max(0, Integer.parseInt(v));
        } catch (NumberFormatException e) {
            return 0;
        }
    }

    // An activity on screen (foreground or visible). A process kept up only
    // by a foreground service (a backgrounded voice call, or the background
    // connection) reports IMPORTANCE_FOREGROUND_SERVICE, which also sorts
    // below VISIBLE, and used to count as foreground here, so the push was
    // dropped. It is background: the message goes through the native /sync
    // path below (which answers quickly, since Qt 6 keeps the event loop
    // running in the background) or the generic notification.
    private static boolean isForeground(Context ctx) {
        ActivityManager.RunningAppProcessInfo info = new ActivityManager.RunningAppProcessInfo();
        ActivityManager.getMyMemoryState(info);
        int imp = info.importance;
        return imp <= ActivityManager.RunningAppProcessInfo.IMPORTANCE_VISIBLE
            && imp != ActivityManager.RunningAppProcessInfo.IMPORTANCE_FOREGROUND_SERVICE;
    }

    private static void ensureChannel(NotificationManager nm) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return;
        if (nm.getNotificationChannel(CHAT_CHANNEL_ID) != null) return;
        NotificationChannel ch = new NotificationChannel(
            CHAT_CHANNEL_ID, CHAT_CHANNEL_NAME, NotificationManager.IMPORTANCE_HIGH);
        ch.setDescription("Notifications for new chat messages.");
        ch.setShowBadge(true);
        nm.createNotificationChannel(ch);
    }

    private static Notification.Builder builder(Context ctx) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            return new Notification.Builder(ctx, CHAT_CHANNEL_ID);
        }
        return new Notification.Builder(ctx);
    }

    private static void postGeneric(Context ctx, String roomId, String eventId, int unread,
                                    String sender, String roomName) {
        NotificationManager nm =
            (NotificationManager) ctx.getSystemService(Context.NOTIFICATION_SERVICE);
        if (nm == null) return;
        ensureChannel(nm);

        String title = roomName != null ? roomName : (sender != null ? sender : "BSFChat");
        String text;
        if (sender != null && roomName != null) text = "New message from " + sender;
        else if (unread > 1) text = unread + " unread messages";
        else text = "New message";

        // Explicit intent to the activity, as AndroidNotifier does: the
        // singleTask activity gets it through onNewIntent (or as its launch
        // intent from cold), and UrlHandler routes bsfchat://push/… to
        // ServerManager::openMessageLink, which finds the room's server.
        Intent tap = new Intent(Intent.ACTION_VIEW);
        tap.setClass(ctx, BSFChatActivity.class);
        tap.setData(Uri.parse("bsfchat://push/" + Uri.encode(roomId)
                              + "/" + Uri.encode(eventId == null ? "" : eventId)));
        tap.setFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        int flags = PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE;
        PendingIntent pi = PendingIntent.getActivity(ctx, roomId.hashCode() & 0x7fffffff, tap, flags);

        // Lock screen: the public version says only that something arrived
        // (M2-8), same as AndroidNotifier.applyLockScreenPrivacy.
        Notification publicVersion = builder(ctx)
            .setContentTitle("BSFChat")
            .setContentText("New message")
            .setSmallIcon(android.R.drawable.sym_action_chat)
            .build();

        Notification.Builder b = builder(ctx)
            .setContentTitle(title)
            .setContentText(text)
            .setSmallIcon(android.R.drawable.sym_action_chat)
            .setAutoCancel(true)
            .setContentIntent(pi)
            .setVisibility(Notification.VISIBILITY_PRIVATE)
            .setPublicVersion(publicVersion);
        if (unread > 0) b.setNumber(unread);
        try {
            nm.notify(TAG_PREFIX + roomId, 1, b.build());
        } catch (SecurityException e) {
            // POST_NOTIFICATIONS not granted (Android 13+). Nothing to do here;
            // Settings > Notifications offers the permission again.
            Log.w(TAG, "cannot post notification: " + e);
        }
    }

    private static SharedPreferences prefs(Context ctx) {
        return ctx.getApplicationContext().getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    private static void notifyToken(String token) {
        if (!sNativeReady) return;
        try {
            nativeOnPushToken(token, deviceName());
        } catch (UnsatisfiedLinkError e) {
            sNativeReady = false;
        }
    }

    private static void notifyRetrying(String reason) {
        if (!sNativeReady) return;
        try {
            nativeOnPushRetrying(reason == null ? "unknown" : reason);
        } catch (UnsatisfiedLinkError e) {
            sNativeReady = false;
        }
    }

    private static void notifyUnavailable(String reason) {
        if (!sNativeReady) return;
        try {
            nativeOnPushUnavailable(reason);
        } catch (UnsatisfiedLinkError e) {
            sNativeReady = false;
        }
    }

    // Implemented in src/core/PushBridge.cpp. They post onto the Qt event
    // loop and return; nativeOnPushMessage returns whether C++ took the event
    // (it then answers through onNativeDone).
    private static native void nativeOnPushToken(String token, String deviceName);
    private static native void nativeOnPushUnavailable(String reason);
    private static native void nativeOnPushRetrying(String reason);
    private static native boolean nativeOnPushMessage(String roomId, String eventId, int unread);
}
