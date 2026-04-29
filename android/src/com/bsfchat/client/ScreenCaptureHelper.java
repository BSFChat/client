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
// Threading: ImageReader delivers frames on a dedicated Handler
// thread so the UI thread isn't touched; the JNI callback jumps
// back onto the Qt event loop on the C++ side.

import android.app.Activity;
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
import android.util.DisplayMetrics;
import android.util.Log;
import android.view.Display;
import android.view.WindowManager;

import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;

public final class ScreenCaptureHelper {
    private static final String TAG = "BSFChatScreenCap";
    public static final int REQUEST_CODE = 7421;

    // Pipeline targets. Long-edge 1024 keeps frame bytes modest;
    // 15 fps is enough to follow a pointer without overwhelming
    // the data-channel send queue.
    private static final int MAX_LONG_EDGE = 1024;
    private static final int TARGET_FPS = 15;
    private static final int JPEG_QUALITY = 60;

    private static ScreenCaptureHelper sInstance;

    private MediaProjection mProjection;
    private VirtualDisplay mVirtualDisplay;
    private ImageReader mImageReader;
    private HandlerThread mThread;
    private Handler mHandler;
    private long mLastFrameMs = 0;
    private int mCapWidth, mCapHeight;

    public static synchronized ScreenCaptureHelper instance() {
        if (sInstance == null) sInstance = new ScreenCaptureHelper();
        return sInstance;
    }

    // Called from JNI: kicks off the consent intent through the
    // currently-foregrounded BSFChatActivity. The actual capture
    // starts in onActivityResult → startCapture().
    public void requestPermission(Activity activity) {
        if (activity == null) return;
        MediaProjectionManager mgr = (MediaProjectionManager)
            activity.getSystemService(Activity.MEDIA_PROJECTION_SERVICE);
        if (mgr == null) return;
        Intent intent = mgr.createScreenCaptureIntent();
        activity.startActivityForResult(intent, REQUEST_CODE);
    }

    // Called from BSFChatActivity.onActivityResult. Forwards to
    // startCapture which actually opens the projection.
    public void onActivityResult(Activity activity,
                                  int resultCode, Intent data) {
        if (data == null || resultCode != Activity.RESULT_OK) {
            nativeOnPermissionDenied();
            return;
        }
        startCapture(activity, resultCode, data);
    }

    private void startCapture(Activity activity, int resultCode, Intent data) {
        stopCapture();  // clean any previous session

        // Start the foreground service BEFORE acquiring the
        // projection — Android 10+ requires the FGS to be alive
        // when getMediaProjection() is called, else the system
        // throws a SecurityException.
        Intent svcIntent = new Intent(activity, MediaProjectionService.class);
        if (android.os.Build.VERSION.SDK_INT
            >= android.os.Build.VERSION_CODES.O) {
            activity.startForegroundService(svcIntent);
        } else {
            activity.startService(svcIntent);
        }

        MediaProjectionManager mgr = (MediaProjectionManager)
            activity.getSystemService(Activity.MEDIA_PROJECTION_SERVICE);
        if (mgr == null) return;

        // Stop callback — fires if the user revokes from the
        // notification, screen locks, etc. We propagate so C++ can
        // clear the toggled-on UI state.
        mProjection = mgr.getMediaProjection(resultCode, data);
        if (mProjection == null) {
            nativeOnPermissionDenied();
            return;
        }
        // Android 14 requires a stop-callback registered before
        // we create the VirtualDisplay; earlier APIs treat it as
        // optional but it doesn't hurt.
        mProjection.registerCallback(new MediaProjection.Callback() {
            @Override
            public void onStop() {
                Log.i(TAG, "MediaProjection stopped by system");
                stopCapture();
                nativeOnStopped();
            }
        }, null);

        // Resolution: scale display metrics so the long edge hits
        // MAX_LONG_EDGE. Keeps aspect.
        DisplayMetrics dm = new DisplayMetrics();
        WindowManager wm = (WindowManager)
            activity.getSystemService(Activity.WINDOW_SERVICE);
        wm.getDefaultDisplay().getRealMetrics(dm);
        int srcW = dm.widthPixels, srcH = dm.heightPixels;
        int longEdge = Math.max(srcW, srcH);
        float scale = longEdge > MAX_LONG_EDGE
            ? (float) MAX_LONG_EDGE / longEdge : 1.0f;
        mCapWidth = Math.round(srcW * scale) & ~1;    // even
        mCapHeight = Math.round(srcH * scale) & ~1;

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

        mVirtualDisplay = mProjection.createVirtualDisplay(
            "bsfchat-screencap",
            mCapWidth, mCapHeight, dm.densityDpi,
            DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR,
            mImageReader.getSurface(), null, mHandler);

        nativeOnStarted(mCapWidth, mCapHeight);
    }

    public void stopCapture() {
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
            mProjection.stop();
            mProjection = null;
        }
        if (mThread != null) {
            mThread.quitSafely();
            mThread = null;
            mHandler = null;
        }
        // Stop the foreground service. Calling context for the
        // stopService is the application context — the activity
        // may have been destroyed by the time we get here.
        try {
            android.content.Context ctx =
                org.qtproject.qt.android.QtNative.getContext();
            if (ctx != null) {
                ctx.stopService(
                    new Intent(ctx, MediaProjectionService.class));
            }
        } catch (Throwable ignored) { /* best effort */ }
    }

    // Frame-rate gate + JPEG-encode + JNI callback.
    private void handleFrame(ImageReader reader) {
        Image img = null;
        try {
            img = reader.acquireLatestImage();
            if (img == null) return;

            long now = System.currentTimeMillis();
            long minInterval = 1000L / TARGET_FPS;
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
            int rowPadding = rowStride - pixelStride * mCapWidth;

            Bitmap bmp = Bitmap.createBitmap(
                mCapWidth + rowPadding / pixelStride,
                mCapHeight, Bitmap.Config.ARGB_8888);
            bmp.copyPixelsFromBuffer(buf);

            // Crop row-padding columns off the right edge.
            Bitmap tight = (rowPadding == 0)
                ? bmp
                : Bitmap.createBitmap(bmp, 0, 0, mCapWidth, mCapHeight);

            ByteArrayOutputStream baos = new ByteArrayOutputStream(
                mCapWidth * mCapHeight);
            tight.compress(Bitmap.CompressFormat.JPEG, JPEG_QUALITY, baos);

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
}
