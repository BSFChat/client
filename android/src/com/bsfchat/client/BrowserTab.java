package com.bsfchat.client;

// Opens a URL in a Custom Tab, without androidx.browser.
//
// Used for the BSFChat ID sign-in page and the identity provider's account
// pages (delete account). A Custom Tab runs in OUR task, over our activity,
// so the user sees it as part of the app rather than being sent off to a
// separate browser task, and the back button returns to BSFChat.
//
// androidx.browser is not among the dependencies Qt's Gradle template pulls
// in, so this speaks the Custom Tabs intent protocol directly: an ACTION_VIEW
// intent carrying the EXTRA_SESSION key (a null binder is allowed and means
// "no session") is what every Custom Tabs browser looks for. A browser that
// does not support Custom Tabs ignores the extra and opens the page normally,
// which is exactly the fallback we want.
//
// The sign-in's App Link redirect still comes back to BSFChatActivity: it is
// singleTask, so the redirect intent brings our task forward and clears the
// tab sitting on top of it.

import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.util.Log;

public final class BrowserTab {
    private static final String TAG = "BSFChatBrowserTab";

    // androidx.browser.customtabs.CustomTabsIntent.EXTRA_SESSION
    private static final String EXTRA_SESSION =
        "android.support.customtabs.extra.SESSION";
    // CustomTabsIntent.EXTRA_SHARE_STATE / SHARE_STATE_OFF: no share item on
    // a sign-in page.
    private static final String EXTRA_SHARE_STATE =
        "androidx.browser.customtabs.extra.SHARE_STATE";
    private static final int SHARE_STATE_OFF = 2;

    private BrowserTab() {}

    // Called from C++ (IdentityClient) on the Qt thread. startActivity is
    // safe from any thread. False when nothing could handle the URL.
    public static boolean open(Context context, String url) {
        if (context == null || url == null) return false;
        try {
            Intent intent = new Intent(Intent.ACTION_VIEW, Uri.parse(url));
            intent.addCategory(Intent.CATEGORY_BROWSABLE);
            Bundle extras = new Bundle();
            extras.putBinder(EXTRA_SESSION, null);
            intent.putExtras(extras);
            intent.putExtra(EXTRA_SHARE_STATE, SHARE_STATE_OFF);
            // Only a non-activity context needs a new task; from the
            // activity the tab stacks on top of it, which is the point.
            if (!(context instanceof Activity)) {
                intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            }
            context.startActivity(intent);
            return true;
        } catch (ActivityNotFoundException e) {
            Log.w(TAG, "no browser for " + Uri.parse(url).getHost());
            return false;
        } catch (Throwable t) {
            Log.w(TAG, "could not open a browser tab: " + t);
            return false;
        }
    }
}
