package com.bsfchat.client;

// Status and navigation bar appearance from the app theme. Called from C++
// (src/core/SystemBars.cpp) at startup, on theme changes and on resume.
//
// The app chooses dark or light itself, independent of the system setting,
// so the bars' icons are set explicitly: light icons on the dark theme, dark
// icons on the light one. Below API 35 the platform also paints the bars, so
// their colours are set to the theme's header and window colours; from 35 the
// app is edge-to-edge (targetSdk 36) and only the icon appearance applies.

import android.app.Activity;
import android.os.Build;
import android.util.Log;
import android.view.View;
import android.view.Window;
import android.view.WindowInsetsController;

public final class SystemBars {
    private static final String TAG = "BSFChatSystemBars";

    private SystemBars() { }

    public static void apply(final Activity activity, final boolean dark,
                             final int statusColor, final int navColor) {
        if (activity == null) return;
        activity.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                try {
                    applyOnUiThread(activity, dark, statusColor, navColor);
                } catch (RuntimeException e) {
                    // Cosmetic; never worth the process.
                    Log.w(TAG, "could not set the system bar appearance: " + e);
                }
            }
        });
    }

    @SuppressWarnings("deprecation")
    private static void applyOnUiThread(Activity activity, boolean dark,
                                        int statusColor, int navColor) {
        Window window = activity.getWindow();
        if (window == null) return;

        if (Build.VERSION.SDK_INT < 35) {
            // Deprecated (and ignored) from 35, where the app draws behind
            // the bars; before that they are what the user sees.
            window.setStatusBarColor(statusColor);
            window.setNavigationBarColor(navColor);
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            WindowInsetsController c = window.getInsetsController();
            if (c == null) return;
            int mask = WindowInsetsController.APPEARANCE_LIGHT_STATUS_BARS
                     | WindowInsetsController.APPEARANCE_LIGHT_NAVIGATION_BARS;
            c.setSystemBarsAppearance(dark ? 0 : mask, mask);
            return;
        }

        View decor = window.getDecorView();
        int flags = decor.getSystemUiVisibility();
        int light = View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            light |= View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR;
        }
        flags = dark ? (flags & ~light) : (flags | light);
        decor.setSystemUiVisibility(flags);
    }
}
