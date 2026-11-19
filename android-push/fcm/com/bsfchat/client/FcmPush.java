package com.bsfchat.client;

// Firebase Cloud Messaging, without the google-services Gradle plugin.
// Staged by cmake/AndroidPush.cmake only when all four BSFCHAT_FCM_* values
// were supplied at configure time; PushConfig is generated next to it from
// those values. They are Firebase's public client configuration (they ship
// inside every APK that uses Firebase), not secrets.
//
// FirebaseApp is normally initialised before any of this runs: the same
// values are also generated as the string resources FirebaseInitProvider
// reads (res/values/bsfchat_push.xml — google_app_id, gcm_defaultSenderId,
// google_api_key, project_id), which matters for a cold start caused by a
// push, where Firebase's own receiver runs before any code of ours.
// ensureInitialised() is the programmatic fallback for every entry point.
//
// Auto-init is off in the manifest (firebase_messaging_auto_init_enabled):
// nothing contacts Firebase until the app asks for a token, which it only
// does when the user is on push rather than a background connection.

import android.content.Context;

import com.google.android.gms.tasks.OnCompleteListener;
import com.google.android.gms.tasks.Task;
import com.google.firebase.FirebaseApp;
import com.google.firebase.FirebaseOptions;
import com.google.firebase.messaging.FirebaseMessaging;

final class FcmPush {
    private FcmPush() { }

    static boolean configured() { return true; }

    static synchronized void ensureInitialised(Context ctx) {
        Context app = ctx.getApplicationContext();
        if (!FirebaseApp.getApps(app).isEmpty()) return;
        FirebaseOptions options = new FirebaseOptions.Builder()
            .setProjectId(PushConfig.PROJECT_ID)
            .setApplicationId(PushConfig.APP_ID)
            .setApiKey(PushConfig.API_KEY)
            .setGcmSenderId(PushConfig.SENDER_ID)
            .build();
        FirebaseApp.initializeApp(app, options);
    }

    static void requestToken(final Context ctx) {
        try {
            ensureInitialised(ctx);
            FirebaseMessaging fm = FirebaseMessaging.getInstance();
            fm.setAutoInitEnabled(true);
            fm.getToken().addOnCompleteListener(new OnCompleteListener<String>() {
                @Override
                public void onComplete(Task<String> task) {
                    if (task.isSuccessful() && task.getResult() != null) {
                        PushBridge.onToken(ctx, task.getResult());
                    } else {
                        // No network yet, Play services updating, or none
                        // at all. PushBridge decides: retry with backoff,
                        // or unavailable (background connection) for good.
                        PushBridge.onTokenFailed(ctx, String.valueOf(task.getException()));
                    }
                }
            });
        } catch (Throwable t) {
            // Firebase classes missing or refusing to initialise: final.
            PushBridge.onUnavailable(t.toString());
        }
    }

    static void disable(final Context ctx) {
        try {
            ensureInitialised(ctx);
            FirebaseMessaging fm = FirebaseMessaging.getInstance();
            fm.setAutoInitEnabled(false);
            fm.deleteToken().addOnCompleteListener(new OnCompleteListener<Void>() {
                @Override
                public void onComplete(Task<Void> task) {
                    PushBridge.onTokenDeleted(ctx);
                }
            });
        } catch (Throwable t) {
            PushBridge.onTokenDeleted(ctx);
        }
    }
}
