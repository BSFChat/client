package com.bsfchat.client;

// Staged by cmake/AndroidPush.cmake when the build was configured WITHOUT
// the BSFCHAT_FCM_* values: no Firebase dependency, no messaging service in
// the manifest, and PushBridge reports push as not configured so the app
// keeps its persistent background connection (SyncService) exactly as
// before push existed. The Firebase build's version is android-push/fcm/.

import android.content.Context;

final class FcmPush {
    private FcmPush() { }

    static boolean configured() { return false; }

    static void requestToken(Context ctx) { }

    static void disable(Context ctx) { }
}
