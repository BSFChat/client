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
import org.qtproject.qt.android.bindings.QtActivity;

public class BSFChatActivity extends QtActivity {

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
        nativeOnNewIntent();
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode,
                                     Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        // MediaProjection consent — forwarded to the screen-capture
        // helper which owns the VirtualDisplay + ImageReader plumbing.
        if (requestCode == ScreenCaptureHelper.REQUEST_CODE) {
            ScreenCaptureHelper.instance()
                .onActivityResult(this, resultCode, data);
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
        for (int i = 0; i < permissions.length; ++i) {
            nativeOnPermissionResult(
                permissions[i], grantResults[i] == 0, requestCode);
        }
    }

    // Native-side receivers — linked in via JNI from the BSFChat shared
    // library. Both are fire-and-forget; they switch onto the Qt event
    // loop inside the C++ implementation.
    private static native void nativeOnNewIntent();
    private static native void nativeOnPermissionResult(
        String permission, boolean granted, int requestCode);
}
