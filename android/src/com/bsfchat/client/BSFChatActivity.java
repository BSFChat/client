package com.bsfchat.client;

// Thin QtActivity subclass that plugs two Android lifecycle hooks
// the stock QtActivity doesn't forward:
//
//   1. onNewIntent  — when the app is already running (launchMode =
//      singleTop) and receives a new intent (e.g. a Share to BSFChat
//      while the activity is already on-screen), QtActivity silently
//      discards it; getIntent() keeps returning the original cold-
//      start intent. We override, call setIntent(), and emit a
//      broadcast that the C++ side picks up via a small listener.
//
//   2. onRequestPermissionsResult — stock QtActivity doesn't expose
//      the grant/deny answer to Qt. Our AndroidPermissions C++
//      helper polls checkSelfPermission every 250ms as a workaround,
//      which is functional but slow and awkward (a dismissed dialog
//      takes 2 min to time out). Overriding lets us signal the
//      result immediately.
//
// Kept tiny: the rest of Qt's activity plumbing is fine, we just
// need these two forwards. The manifest points android:name at this
// class instead of org.qtproject.qt.android.bindings.QtActivity.

import android.content.Intent;
import android.util.Log;
import org.qtproject.qt.android.bindings.QtActivity;

public class BSFChatActivity extends QtActivity {

    private static final String TAG = "BSFChatActivity";

    // Broadcast tag used by the C++ side to listen for new-intents
    // without hooking into Android's package-wide broadcast system.
    public static final String ACTION_NEW_INTENT =
        "com.bsfchat.client.NEW_INTENT";

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        // Stash it so getIntent() returns the fresh one — our C++
        // UrlHandler::checkAndroidShareIntent() reads getIntent()
        // and acts on ACTION_SEND.
        setIntent(intent);

        // Notify the native layer. We use a local broadcast for
        // simplicity — the JNI call site isn't available at class-
        // load time so we can't call directly into Qt here. Native
        // side polls on applicationState change anyway, but we
        // bump a native hook registered by AndroidPermissions.
        //
        // Guarded: the bridge is registered from C++ when
        // AndroidPermissions is constructed, which happens on the Qt
        // thread inside main(). An intent that arrives before that —
        // the window is small but it is exactly the one an OIDC
        // redirect into a just-relaunched process lands in — would
        // otherwise throw UnsatisfiedLinkError out of onNewIntent and
        // take the process down. setIntent() above has already done
        // the part that matters; main()'s own
        // checkAndroidLaunchIntent() at startup reads it back.
        try {
            nativeOnNewIntent();
        } catch (UnsatisfiedLinkError e) {
            Log.w(TAG, "native new-intent hook not registered yet; "
                       + "the startup intent scan will pick this up");
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode,
                                     Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        // MediaProjection consent — forwarded to the screen-capture
        // helper which owns the VirtualDisplay + ImageReader plumbing.
        if (requestCode == ScreenCaptureHelper.REQUEST_CODE) {
            try {
                ScreenCaptureHelper.instance()
                    .onActivityResult(this, resultCode, data);
            } catch (Throwable t) {
                // A throw here unwinds into ActivityThread and kills the
                // process. Losing a screen share is survivable; losing
                // the app in the middle of a call is not.
                Log.w(TAG, "screen-capture result handling failed: " + t);
            }
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode,
                                           String[] permissions,
                                           int[] grantResults) {
        super.onRequestPermissionsResult(
            requestCode, permissions, grantResults);
        // One JNI callback for every (perm, result) pair so the C++
        // side doesn't need to marshal an array. `result` is 0 for
        // PERMISSION_GRANTED, -1 for PERMISSION_DENIED.
        // grantResults can come back EMPTY when a request is cancelled
        // (the user swiped the dialog away, or another dialog stole it).
        // Indexing it against permissions.length would then throw.
        final int n = Math.min(permissions.length, grantResults.length);
        try {
            for (int i = 0; i < n; ++i) {
                nativeOnPermissionResult(
                    permissions[i], grantResults[i] == 0, requestCode);
            }
            // A cancelled request has to be answered too, or the QML
            // continuation that is waiting on microphoneResult /
            // cameraResult / notificationsResult never runs and the join
            // button stays dead until the app is restarted.
            if (n == 0) {
                for (int i = 0; i < permissions.length; ++i) {
                    nativeOnPermissionResult(permissions[i], false, requestCode);
                }
            }
        } catch (UnsatisfiedLinkError e) {
            Log.w(TAG, "native permission hook not registered: " + e);
        }
    }

    // Native-side receivers — linked in via JNI from the BSFChat shared
    // library. Both are fire-and-forget; they switch onto the Qt event
    // loop inside the C++ implementation.
    private static native void nativeOnNewIntent();
    private static native void nativeOnPermissionResult(
        String permission, boolean granted, int requestCode);
}
