package com.bsfchat.client;

// Receives FCM data messages relayed by the Sygnal gateway, and token
// rotations. Declared in the manifest (exported=false, MESSAGING_EVENT) by
// cmake/AndroidPush.cmake only in Firebase builds. All logic is in
// PushBridge so the stub build compiles the same callers.

import com.google.firebase.messaging.FirebaseMessagingService;
import com.google.firebase.messaging.RemoteMessage;

public class PushMessagingService extends FirebaseMessagingService {
    @Override
    public void onCreate() {
        super.onCreate();
        FcmPush.ensureInitialised(this);
    }

    @Override
    public void onNewToken(String token) {
        PushBridge.onToken(this, token);
    }

    // On a Firebase worker thread; PushBridge.onMessage may block for a few
    // seconds while the native /sync fetches the event.
    @Override
    public void onMessageReceived(RemoteMessage message) {
        PushBridge.onMessage(this, message.getData());
    }
}
