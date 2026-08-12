// SPDX-License-Identifier: GPL-3.0
// PancraFiles.java --- a tiny read-only ContentProvider for EXPORT DATA
// Copyright 2026 Jakob Kastelic

/* A file:// URI to a private file cannot be shared on modern Android
 * (FileUriExposedException), so a share needs a content:// URI backed by a
 * ContentProvider. The androidx FileProvider would need a support library this
 * (Gradle-free, platform-only) build does not carry -- so this is a minimal
 * hand-written provider instead. It serves ONLY the two exported CSVs, read
 * only, by exact name (no path traversal). */
package com.jk.pancra;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileNotFoundException;

public class PancraFiles extends ContentProvider {
    /* The exact set of files that may be handed out. Anything else -> denied. */
    private static final String[] ALLOWED = { "pancra.csv" };

    private static boolean allowed(String name) {
        if (name == null) return false;
        for (String a : ALLOWED) if (a.equals(name)) return true;
        return false;
    }

    private File resolve(Uri uri) throws FileNotFoundException {
        String name = uri.getLastPathSegment();
        if (!allowed(name)) throw new FileNotFoundException("denied: " + name);
        return new File(getContext().getFilesDir(), name);
    }

    @Override public boolean onCreate() { return true; }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode)
            throws FileNotFoundException {
        return ParcelFileDescriptor.open(resolve(uri),
                ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override public String getType(Uri uri) { return "text/csv"; }

    /* Share targets (Gmail, Drive, ...) query for the display name and size. */
    @Override
    public Cursor query(Uri uri, String[] proj, String sel, String[] args,
                        String sort) {
        String name = uri.getLastPathSegment();
        if (!allowed(name)) return null;
        File f = new File(getContext().getFilesDir(), name);
        MatrixCursor c = new MatrixCursor(
                new String[] { OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE });
        c.addRow(new Object[] { name, f.length() });
        return c;
    }

    /* Read-only: the rest are inert. */
    @Override public Uri insert(Uri u, ContentValues v) { return null; }
    @Override public int delete(Uri u, String s, String[] a) { return 0; }
    @Override public int update(Uri u, ContentValues v, String s, String[] a) {
        return 0;
    }
}
