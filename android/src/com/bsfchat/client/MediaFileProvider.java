package com.bsfchat.client;

// Hands a downloaded attachment to another app as a content:// URI.
//
// Opening an attachment used to pass a file:// URI for a file in our private
// cache. Since Android 7 that throws FileUriExposedException out of
// startActivity (StrictMode's VM policy, on by default for targetSdk 24+), and
// even where it did not, the receiving app cannot read our private storage.
// The supported way is a content provider plus a one-off read grant on the
// intent. androidx.core's FileProvider is not among the dependencies Qt's
// Gradle template pulls in, so this is the minimal equivalent:
//
//   * serves files under <cacheDir>/media ONLY (MediaDownloader's cache,
//     QStandardPaths::CacheLocation + "/media"), checked on the canonical path
//     so "..", symlinks and encoded slashes cannot reach anything else;
//   * read-only: any write mode is refused;
//   * not exported: other apps get in only through the per-URI grant
//     (FLAG_GRANT_READ_URI_PERMISSION) on the intent view() sends, which ends
//     when the receiving activity finishes.
//
// Declared in AndroidManifest.xml with authority "${applicationId}.media".

import android.content.ActivityNotFoundException;
import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.Context;
import android.content.Intent;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import android.util.Log;
import android.webkit.MimeTypeMap;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.util.Locale;

public final class MediaFileProvider extends ContentProvider {
    private static final String TAG = "BSFChatMediaProvider";
    private static final String AUTHORITY_SUFFIX = ".media";
    private static final String MEDIA_DIR = "media";

    // ── Called from C++ (src/util/ExternalBrowser.cpp) ──────────────────

    // Open `path` (a file inside the media cache) in whatever app handles its
    // type. Returns false when the path is outside the cache, nothing can
    // open it, or the start failed; the caller reports that to the user.
    public static boolean view(Context ctx, String path) {
        if (ctx == null || path == null) return false;
        try {
            File root = mediaRoot(ctx);
            File file = new File(path).getCanonicalFile();
            if (!isInside(root, file) || !file.isFile()) {
                Log.w(TAG, "refusing to share a file outside the media cache");
                return false;
            }
            String rel = root.toURI().relativize(file.toURI()).getPath();
            Uri uri = new Uri.Builder()
                .scheme("content")
                .authority(ctx.getPackageName() + AUTHORITY_SUFFIX)
                .path(rel)
                .build();
            Intent intent = new Intent(Intent.ACTION_VIEW);
            intent.setDataAndType(uri, mimeFor(file.getName()));
            intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                            | Intent.FLAG_ACTIVITY_NEW_TASK);
            ctx.startActivity(intent);
            return true;
        } catch (ActivityNotFoundException e) {
            Log.w(TAG, "no app can open this file: " + e);
            return false;
        } catch (IOException | RuntimeException e) {
            Log.w(TAG, "could not open attachment: " + e);
            return false;
        }
    }

    // ── ContentProvider ────────────────────────────────────────────────

    @Override
    public boolean onCreate() {
        return true;
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        if (mode == null || !mode.equals("r")) {
            throw new FileNotFoundException("read-only");
        }
        return ParcelFileDescriptor.open(resolve(uri), ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection,
                        String[] selectionArgs, String sortOrder) {
        File file;
        try {
            file = resolve(uri);
        } catch (FileNotFoundException e) {
            return null;
        }
        String[] cols = projection != null ? projection
            : new String[] { OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE };
        String[] outCols = new String[cols.length];
        Object[] values = new Object[cols.length];
        int n = 0;
        for (String col : cols) {
            if (OpenableColumns.DISPLAY_NAME.equals(col)) {
                outCols[n] = col;
                values[n++] = file.getName();
            } else if (OpenableColumns.SIZE.equals(col)) {
                outCols[n] = col;
                values[n++] = file.length();
            }
        }
        String[] finalCols = new String[n];
        Object[] finalValues = new Object[n];
        System.arraycopy(outCols, 0, finalCols, 0, n);
        System.arraycopy(values, 0, finalValues, 0, n);
        MatrixCursor cursor = new MatrixCursor(finalCols, 1);
        cursor.addRow(finalValues);
        return cursor;
    }

    @Override
    public String getType(Uri uri) {
        String last = uri.getLastPathSegment();
        return mimeFor(last == null ? "" : last);
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        throw new UnsupportedOperationException("read-only");
    }

    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs) {
        throw new UnsupportedOperationException("read-only");
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) {
        throw new UnsupportedOperationException("read-only");
    }

    // ── Helpers ─────────────────────────────────────────────────────────

    private static File mediaRoot(Context ctx) throws IOException {
        return new File(ctx.getCacheDir(), MEDIA_DIR).getCanonicalFile();
    }

    private static boolean isInside(File root, File file) {
        String r = root.getPath() + File.separator;
        return file.getPath().startsWith(r);
    }

    private File resolve(Uri uri) throws FileNotFoundException {
        Context ctx = getContext();
        String rel = uri.getPath();   // decoded
        if (ctx == null || rel == null || rel.isEmpty()) {
            throw new FileNotFoundException("no such file");
        }
        try {
            File root = mediaRoot(ctx);
            File file = new File(root, rel).getCanonicalFile();
            if (!isInside(root, file) || !file.isFile()) {
                throw new FileNotFoundException("no such file");
            }
            return file;
        } catch (IOException e) {
            throw new FileNotFoundException("no such file");
        }
    }

    private static String mimeFor(String name) {
        int dot = name.lastIndexOf('.');
        if (dot >= 0 && dot < name.length() - 1) {
            String ext = name.substring(dot + 1).toLowerCase(Locale.ROOT);
            String mime = MimeTypeMap.getSingleton().getMimeTypeFromExtension(ext);
            if (mime != null) return mime;
        }
        return "application/octet-stream";
    }
}
