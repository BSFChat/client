package com.bsfchat.client;

// Owns the MediaProjection lifecycle: launches the consent intent,
// converts the activity result into a MediaProjection token,
// hooks up a VirtualDisplay → ImageReader pipeline, periodically
// grabs frames, encodes them to JPEG, and hands the bytes to C++
// via a JNI callback.
//
// Frame rate is fixed at 15 fps / 1024 px longest-edge / JPEG
// quality 60 — a reasonable default for voice-room screen share
// that won't saturate a mobile uplink. Tunable later via a quality
// preset if we add one on mobile.
//
// ── Ordering, and why this class no longer opens the projection ──
//
// From Android 14 (API 34) MediaProjectionManager.getMediaProjection()
// throws SecurityException unless a foreground service of type
// `mediaProjection` is ALREADY in the foreground state at the moment
// of the call. Context.startForegroundService() does not satisfy that
// synchronously: it only enqueues onStartCommand on the main looper,
// so the service's startForeground() necessarily runs AFTER the
// message that called it has returned. The previous shape here —
//
//     activity.startForegroundService(svcIntent);
//     mProjection = mgr.getMediaProjection(resultCode, data);
//
// therefore always ran the two in the forbidden order and could only
// ever have worked on API <= 33.
//
// So the consent RESULT is handed to MediaProjectionService as intent
// extras, and the service calls back into onServiceForegrounded()
// from inside onStartCommand, after startForeground() has returned.
// That is the only point at which getMediaProjection() is legal.
//
// The consent itself is single-use from Android 14: a result Intent
// that has already been converted into a projection cannot be
// converted again. Nothing here caches one — every start goes through
// requestPermission() and a fresh createScreenCaptureIntent() — and
// the pending result is cleared the moment it is consumed so a retry
// cannot pick up a stale token.
//
// ── Threading ──
//
// Three threads reach this class:
//   * the Qt main thread, via JNI (configure/requestPermission/
//     stopCapture) — this is NOT the Android UI thread;
//   * the Android main (UI) thread, via onActivityResult and the
//     service callback;
//   * a dedicated capture HandlerThread, via ImageReader.
// Every mutation of the projection/display/reader state is therefore
// posted to the main looper, and the fields the capture thread reads
// are volatile. The JNI callbacks jump back onto the Qt event loop on
// the C++ side.

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.ImageFormat;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.hardware.display.DisplayManager;
import android.hardware.display.VirtualDisplay;
import android.media.Image;
import android.media.ImageReader;
import android.media.projection.MediaProjection;
import android.media.projection.MediaProjectionManager;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.util.DisplayMetrics;
import android.util.Log;
import android.view.Display;
import android.view.WindowManager;

import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;

public final class ScreenCaptureHelper {
    private static final String TAG = "BSFChatScreenCap";
    public static final int REQUEST_CODE = 7421;

    // Pipeline targets. Defaults are conservative; the actual
    // values are pushed in via configure() from C++ at start time
    // so user/server settings (Settings.screenShare{Fps,MaxWidth,
    // JpegQuality} clamped against the server policy) take effect
    // here without needing a Java rebuild for every tweak.
    //
    // volatile: written from the Qt thread, read from the capture
    // HandlerThread.
    private static volatile int sMaxLongEdge = 1280;
    private static volatile int sTargetFps   = 5;
    private static volatile int sJpegQuality = 60;

    public static void configure(int maxLongEdge,
                                 int targetFps,
                                 int jpegQuality) {
        sMaxLongEdge = Math.max(480, Math.min(3840, maxLongEdge));
        sTargetFps   = Math.max(1,   Math.min(60,   targetFps));
        sJpegQuality = Math.max(1,   Math.min(100,  jpegQuality));
    }

    private static ScreenCaptureHelper sInstance;

    private MediaProjection mProjection;
    private VirtualDisplay mVirtualDisplay;
    private ImageReader mImageReader;
    private HandlerThread mThread;
    private Handler mHandler;
    private long mLastFrameMs = 0;
    // Read from the capture HandlerThread in handleFrame().
    private volatile int mCapWidth, mCapHeight;

    // Guards against the reentrant teardown that MediaProjection.stop()
    // provokes: stop() synchronously invokes our registered onStop(),
    // which would otherwise call straight back into releasePipeline().
    private boolean mTearingDown = false;

    // Application context, captured from the Activity when a capture
    // session starts. stopCapture() needs a Context to stop the
    // foreground service and may run after the Activity is gone, so
    // the *application* context is the right one to hold — it is a
    // process singleton and holding it leaks nothing.
    //
    // This used to call org.qtproject.qt.android.QtNative.getContext().
    // That is Qt-internal and is not public in Qt 6.10 ("getContext()
    // is not public in QtNative; cannot be accessed from outside
    // package"), which broke the Java compile outright. Every entry
    // point into this class already receives the Activity, so there
    // was never a reason to ask Qt for one.
    private volatile Context mAppContext;

    private static final Handler sMain = new Handler(Looper.getMainLooper());

    public static synchronized ScreenCaptureHelper instance() {
        if (sInstance == null) sInstance = new ScreenCaptureHelper();
        return sInstance;
    }

    // Called from JNI on the Qt thread: kicks off the consent intent
    // through the currently-foregrounded BSFChatActivity. Posted to the
    // main looper because startActivityForResult is an Activity call and
    // the Qt thread is not the Android UI thread. The actual capture
    // starts once the user answers, in onActivityResult.
    public void requestPermission(final Activity activity) {
        if (activity == null) return;
        sMain.post(new Runnable() {
            @Override
            public void run() {
                MediaProjectionManager mgr = (MediaProjectionManager)
                    activity.getSystemService(Activity.MEDIA_PROJECTION_SERVICE);
                if (mgr == null) {
                    nativeOnError("Screen capture is not available on this device");
                    return;
                }
                try {
                    // ALWAYS a fresh intent. From Android 14 a consent
                    // result is single-use; reusing one throws
                    // SecurityException at getMediaProjection().
                    Intent intent = mgr.createScreenCaptureIntent();
                    activity.startActivityForResult(intent, REQUEST_CODE);
                } catch (Throwable t) {
                    Log.w(TAG, "consent intent failed: " + t);
                    nativeOnError("Could not ask for screen-capture permission");
                }
            }
        });
    }

    // Called from BSFChatActivity.onActivityResult, on the UI thread.
    // Does NOT open the projection: it starts the foreground service and
    // hands it the consent result, and the service calls us back at
    // onServiceForegrounded() once startForeground() has returned. See
    // the ordering note at the top of this file.
    public void onActivityResult(Activity activity,
                                  int resultCode, Intent data) {
        if (data == null || resultCode != Activity.RESULT_OK) {
            nativeOnPermissionDenied();
            return;
        }

        // Drop any previous session's pipeline, but leave the service
        // alone: we are about to (re)start it, and a stopService here
        // would race the startForegroundService below.
        releasePipeline();

        mAppContext = activity.getApplicationContext();

        Intent svcIntent = new Intent(activity, MediaProjectionService.class);
        svcIntent.putExtra(MediaProjectionService.EXTRA_RESULT_CODE, resultCode);
        svcIntent.putExtra(MediaProjectionService.EXTRA_RESULT_DATA, data);
        try {
            // API >= 26 always: minSdk is 28.
            activity.startForegroundService(svcIntent);
        } catch (Throwable t) {
            // ForegroundServiceStartNotAllowedException on 12+ if we
            // somehow got here while backgrounded. Cannot happen from a
            // consent result (the consent dialog is ours and we are
            // resumed behind it), but a thrown exception here would
            // otherwise take the process with it.
            Log.w(TAG, "screen-share service start refused: " + t);
            nativeOnError("Screen sharing could not start in the background");
        }
    }

    // Called by MediaProjectionService from inside onStartCommand, AFTER
    // startForeground() has returned. This is the only point at which
    // getMediaProjection() is legal on Android 14+.
    void onServiceForegrounded(Context serviceContext,
                               int resultCode, Intent data) {
        if (mAppContext == null && serviceContext != null)
            mAppContext = serviceContext.getApplicationContext();

        MediaProjectionManager mgr = (MediaProjectionManager)
            serviceContext.getSystemService(Context.MEDIA_PROJECTION_SERVICE);
        if (mgr == null) {
            nativeOnError("Screen capture is not available on this device");
            stopCapture();
            return;
        }

        try {
            mProjection = mgr.getMediaProjection(resultCode, data);
        } catch (Throwable t) {
            // SecurityException on 14+ for a reused/stale consent token,
            // or if the FGS type check still did not pass. Report rather
            // than let it unwind into the platform.
            Log.w(TAG, "getMediaProjection failed: " + t);
            mProjection = null;
        }
        if (mProjection == null) {
            nativeOnError("Screen-share permission was not accepted");
            stopCapture();
            return;
        }

        // Android 14 requires a stop-callback registered before
        // we create the VirtualDisplay; earlier APIs treat it as
        // optional but it doesn't hurt. Explicit main-looper handler:
        // a null handler would use the calling thread's looper, and
        // this can be reached from a thread that has none.
        mProjection.registerCallback(new MediaProjection.Callback() {
            @Override
            public void onStop() {
                Log.i(TAG, "MediaProjection stopped by system");
                stopCapture();
                nativeOnStopped();
            }
        }, sMain);

        // Resolution: scale display metrics so the long edge hits
        // sMaxLongEdge. Keeps aspect.
        DisplayMetrics dm = new DisplayMetrics();
        WindowManager wm = (WindowManager)
            serviceContext.getSystemService(Context.WINDOW_SERVICE);
        if (wm == null) {
            nativeOnError("Could not read the display size");
            stopCapture();
            return;
        }
        wm.getDefaultDisplay().getRealMetrics(dm);
        int srcW = dm.widthPixels, srcH = dm.heightPixels;
        int longEdge = Math.max(srcW, srcH);
        float scale = longEdge > sMaxLongEdge
            ? (float) sMaxLongEdge / longEdge : 1.0f;
        mCapWidth = Math.max(2, Math.round(srcW * scale) & ~1);    // even
        mCapHeight = Math.max(2, Math.round(srcH * scale) & ~1);

        mImageReader = ImageReader.newInstance(
            mCapWidth, mCapHeight,
            PixelFormat.RGBA_8888, 2);

        mThread = new HandlerThread("bsfchat-screencap");
        mThread.start();
        mHandler = new Handler(mThread.getLooper());

        mImageReader.setOnImageAvailableListener(
            new ImageReader.OnImageAvailableListener() {
                @Override
                public void onImageAvailable(ImageReader reader) {
                    handleFrame(reader);
                }
            }, mHandler);

        try {
            mVirtualDisplay = mProjection.createVirtualDisplay(
                "bsfchat-screencap",
                mCapWidth, mCapHeight, dm.densityDpi,
                DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR,
                mImageReader.getSurface(), null, mHandler);
        } catch (Throwable t) {
            Log.w(TAG, "createVirtualDisplay failed: " + t);
            mVirtualDisplay = null;
        }
        if (mVirtualDisplay == null) {
            nativeOnError("Screen capture could not be started");
            stopCapture();
            return;
        }

        nativeOnStarted(mCapWidth, mCapHeight);
    }

    // Called by the service when it could not reach the foreground at
    // all (notification blocked in a way that kills the start, or a
    // start-not-allowed refusal). The consent result dies with it.
    void onServiceFailed(String reason) {
        Log.w(TAG, "screen-share service failed: " + reason);
        nativeOnError(reason);
        stopCapture();
    }

    // Full teardown, including the foreground service. Reachable from
    // the Qt thread (JNI stop()), the main thread (projection onStop)
    // and our own failure paths, so it marshals onto the main looper.
    public void stopCapture() {
        if (Looper.myLooper() == Looper.getMainLooper()) {
            stopCaptureOnMain();
        } else {
            sMain.post(new Runnable() {
                @Override public void run() { stopCaptureOnMain(); }
            });
        }
    }

    private void stopCaptureOnMain() {
        if (mTearingDown) return;
        mTearingDown = true;
        try {
            releasePipeline();
            // Stop the foreground service. Calling context for the
            // stopService is the application context — the activity
            // may have been destroyed by the time we get here.
            try {
                Context ctx = mAppContext;
                if (ctx != null) {
                    ctx.stopService(
                        new Intent(ctx, MediaProjectionService.class));
                }
            } catch (Throwable ignored) { /* best effort */ }
        } finally {
            mTearingDown = false;
        }
    }

    // Everything except the foreground service, so a restart can reuse
    // the already-running service rather than racing a stop against a
    // start.
    private void releasePipeline() {
        if (mVirtualDisplay != null) {
            mVirtualDisplay.release();
            mVirtualDisplay = null;
        }
        if (mImageReader != null) {
            mImageReader.setOnImageAvailableListener(null, null);
            mImageReader.close();
            mImageReader = null;
        }
        if (mProjection != null) {
            MediaProjection p = mProjection;
            // Null the field first: stop() invokes our onStop callback
            // synchronously, and that calls back in here.
            mProjection = null;
            try { p.stop(); } catch (Throwable ignored) { }
        }
        if (mThread != null) {
            mThread.quitSafely();
            mThread = null;
            mHandler = null;
        }
        mLastFrameMs = 0;
    }

    // Frame-rate gate + JPEG-encode + JNI callback.
    private void handleFrame(ImageReader reader) {
        Image img = null;
        try {
            img = reader.acquireLatestImage();
            if (img == null) return;

            long now = System.currentTimeMillis();
            long minInterval = 1000L / sTargetFps;
            if (now - mLastFrameMs < minInterval) return;
            mLastFrameMs = now;

            // Copy the RGBA plane into a Bitmap — the most
            // straightforward path to JPEG on Android. ImageReader
            // returns a row-stride that may exceed w*4 for
            // alignment; we rebuild a tight Bitmap using pixel
            // copy.
            Image.Plane plane = img.getPlanes()[0];
            ByteBuffer buf = plane.getBuffer();
            int rowStride = plane.getRowStride();
            int pixelStride = plane.getPixelStride();
            int w = mCapWidth, h = mCapHeight;
            if (w <= 0 || h <= 0 || pixelStride <= 0) return;
            int rowPadding = rowStride - pixelStride * w;

            Bitmap bmp = Bitmap.createBitmap(
                w + rowPadding / pixelStride,
                h, Bitmap.Config.ARGB_8888);
            bmp.copyPixelsFromBuffer(buf);

            // Crop row-padding columns off the right edge.
            Bitmap tight = (rowPadding == 0)
                ? bmp
                : Bitmap.createBitmap(bmp, 0, 0, w, h);

            ByteArrayOutputStream baos = new ByteArrayOutputStream(w * h);
            tight.compress(Bitmap.CompressFormat.JPEG, sJpegQuality, baos);

            byte[] jpeg = baos.toByteArray();
            if (bmp != tight) bmp.recycle();
            tight.recycle();

            nativeOnFrame(jpeg);
        } catch (Throwable t) {
            Log.w(TAG, "frame error: " + t);
        } finally {
            if (img != null) img.close();
        }
    }

    // JNI callbacks, implemented C++-side in
    // src/voice/AndroidScreenShare.cpp. All invoked on the
    // capture HandlerThread; C++ side marshals to the Qt event
    // loop.
    private static native void nativeOnStarted(int width, int height);
    private static native void nativeOnStopped();
    private static native void nativeOnPermissionDenied();
    private static native void nativeOnFrame(byte[] jpeg);
    // A failure with something to say. Surfaces as
    // AndroidScreenShareController::lastError, which VoiceDock already
    // binds, so a refused projection reports itself instead of leaving
    // a share button that does nothing.
    private static native void nativeOnError(String message);
}
