// SPDX-License-Identifier: GPL-3.0
// PancraFiles.java --- a tiny read-only ContentProvider for EXPORT DATA
// Copyright 2026 Jakob Kastelic

/* A file:// URI to a private file cannot be shared on modern Android
 * (FileUriExposedException), so a share needs a content:// URI backed by a
 * ContentProvider. The androidx FileProvider would need a support library this
 * (Gradle-free, platform-only) build does not carry -- so this is a minimal
 * hand-written provider instead. It serves ONLY export snapshots, read only,
 * by exact name.
 *
 * WHAT IT USED TO SERVE, and why that was two bugs in one line. The allowed
 * set was one fixed name -- files/pancra.csv -- which Ble.exportData rewrote
 * on every export. So the provider resolved a LIVE pathname: the URI a
 * recipient had been granted named a file whose contents changed under it, and
 * a second export truncated the first recipient's data mid-read. Snapshots
 * have unique names now, and this side has one job -- resolve a name that is
 * genuinely a snapshot's, and refuse everything else.
 *
 * REFUSING IS THE SECURITY HALF. The last path segment arrives from ANOTHER
 * process and arrives URL-DECODED, so "..", "%2F..%2F", an absolute path or a
 * name with a slash in it all reach this code as ordinary strings; resolving
 * them against getFilesDir() would read stelo.key or paircode.txt, which live
 * in the same tree. Two independent gates therefore stand in front of every
 * open: the name must match the export grammar exactly
 * (BoundaryLogic.exportNameValid, host-tested), and the resolved path must
 * still be inside the snapshot directory after canonicalisation.
 *
 * A HALF-WRITTEN EXPORT IS NOT A NAME THIS GRAMMAR ADMITS, and that is the
 * second job the length check does. ExportSnapshot writes into
 * `<snapshot>.part` and renames it into place only once the output stream has
 * closed cleanly, so the file being written is 34 characters long and fails
 * exportNameValid before anything else is considered. A URI can therefore
 * name a finished snapshot or nothing at all -- never a prefix of one, which
 * is what a recipient reading during an export used to get.
 *
 * WHY THE GRAMMAR IS NOT ALSO IN THE MANIFEST as a <grant-uri-permission
 * android:pathPattern>: that attribute's glob understands only '*' and '.*',
 * which cannot express "ten digits then eight lowercase hex", and a
 * mis-specified pattern silently denies EVERY share -- a failure that shows up
 * only on a phone, in someone else's app, as a missing attachment. The
 * constraint is in code, where it is executable, and the manifest keeps the
 * per-URI grant (exported=false, so an ungranted caller cannot reach this
 * provider at all). */
package com.jk.pancra;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.Context;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileNotFoundException;

public class PancraFiles extends ContentProvider {
    /* The URI space is EXACTLY the set of valid snapshot names: one path
     * segment, and that segment must be one. A multi-segment URI is refused
     * before its last segment is even looked at -- getLastPathSegment() on
     * content://.../a/b/pancra-....csv returns a name that would pass the
     * grammar, and answering it would mean this provider serves a URI it never
     * granted and never will. */
    private File resolve(Uri uri) throws FileNotFoundException {
        if (uri == null) throw new FileNotFoundException("no uri");
        java.util.List<String> seg = uri.getPathSegments();
        if (seg == null || seg.size() != 1)
            throw new FileNotFoundException("denied: not a one-segment path");
        String name = seg.get(0);
        if (!BoundaryLogic.exportNameValid(name))
            throw new FileNotFoundException("denied: " + name);
        Context c = getContext();
        if (c == null) throw new FileNotFoundException("no context");
        File dir = new File(c.getFilesDir(), BoundaryLogic.EXPORT_DIR);
        File f = new File(dir, name);
        /* THE SECOND GATE, and independent of the first: whatever the grammar
         * did or did not allow, the file this opens has to live in the
         * snapshot directory. Canonicalisation also resolves a symlink placed
         * where a snapshot should be -- which the grammar cannot see, because a
         * symlink's NAME is perfectly valid. */
        try {
            String want = dir.getCanonicalPath() + File.separator;
            if (!f.getCanonicalPath().startsWith(want))
                throw new FileNotFoundException("denied: escapes " + want);
        } catch (java.io.IOException e) {
            throw new FileNotFoundException("cannot canonicalise: " + name);
        }
        return f;
    }

    @Override public boolean onCreate() { return true; }

    /* READ ONLY, ALWAYS. A snapshot is made read-only on disk when it is
     * written, and no mode here can widen that. */
    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode)
            throws FileNotFoundException {
        return ParcelFileDescriptor.open(resolve(uri),
                ParcelFileDescriptor.MODE_READ_ONLY);
    }

    /* A type for a URI this provider would refuse to open is a lie the share
     * sheet acts on, so this validates too -- via the same resolve(). */
    @Override public String getType(Uri uri) {
        try { resolve(uri); } catch (FileNotFoundException e) { return null; }
        return "text/csv";
    }

    /* Share targets (Gmail, Drive, ...) query for the display name and size.
     * The size is the SNAPSHOT'S -- a fixed file that nothing rewrites -- so a
     * recipient that measures now and reads later can no longer be handed a
     * different number of bytes than it was promised. */
    @Override
    public Cursor query(Uri uri, String[] proj, String sel, String[] args,
                        String sort) {
        File f;
        try { f = resolve(uri); } catch (FileNotFoundException e) { return null; }
        MatrixCursor c = new MatrixCursor(
                new String[] { OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE });
        c.addRow(new Object[] { f.getName(), f.length() });
        return c;
    }

    /* Read-only: the rest are inert. */
    @Override public Uri insert(Uri u, ContentValues v) { return null; }
    @Override public int delete(Uri u, String s, String[] a) { return 0; }
    @Override public int update(Uri u, ContentValues v, String s, String[] a) {
        return 0;
    }
}
