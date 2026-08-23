// SPDX-License-Identifier: GPL-3.0
// PancraExport.java --- handing a snapshot of the record to another app
// Copyright 2026 Jakob Kastelic

/* THE EXPORT ADAPTER, and nothing else.
 *
 * Inside Ble.java -- whose own header calls it a dumb pipe to the BLE APIs
 * that interprets nothing -- this sits beside CSV snapshots, the share sheet,
 * file retention, the screen orientation, permission requests, notifications
 * and HTTPS. A class that
 * is four unrelated adapters in a trench coat cannot be reasoned about by
 * what it claims to be: the BLE link state and the export's file handling
 * sat in one lock domain and one 1700-line file, and every reader had to
 * establish for themselves which half they were in.
 *
 * So: one Android capability per class. This one turns "the user chose EXPORT
 * DATA" into a file and a share intent. It touches no BLE state, holds no
 * connection, and the only thing native asks of it is the one entry point
 * below.
 *
 * The bytes, the durability rule and the naming grammar are NOT here -- they
 * are in ExportSnapshot and ExportPolicy, which the host JVM tests
 * (`make exportjavatest`). What is here is the part that genuinely needs
 * Android: a files directory, a content:// URI and a chooser. */
package com.jk.pancra;

import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.util.Log;

public final class PancraExport {
    private static final String TAG = "pancra";

    private PancraExport() { }


    /* EXPORT DATA: build ONE file -- sensors.csv (the device map), a blank line,
     * readings.csv (the glucose log), a blank line, insulin.csv (the dose log),
     * a blank line, then weight.csv -- and hand it to another app via the
     * system share sheet. Each section carries its own '#' header row (the
     * weight one names GRAMS, since the display unit is only a preference and
     * an exported file must be readable without it), so the sections stay
     * self-describing
     * and a reader can tell them apart. The content:// URI comes from
     * PancraFiles (see the manifest); FLAG_GRANT_READ_URI_PERMISSION lets the
     * chosen app read it. */
    /* cutoff: rows whose leading epoch field is OLDER are left out (0 =
     * everything); header lines (no leading digit) are always kept. The
     * three flags mirror the EXPORT DATA menu's checkboxes; the DEVICES
     * section is a registry, not a time series, so it never filters. */
    /* ONE FILE PER EXPORT, AND IT IS NEVER WRITTEN AGAIN.
     *
     * A single name -- files/pancra.csv, truncated on every export -- shared
     * as a content:// URI cannot work. The receiving app opens that URI when
     * it needs the bytes, not when the chooser closes: a mail client reads the
     * attachment as the message is SENT, which for a draft, a queued send, or
     * a phone that regains network overnight is minutes or hours later. A
     * second export truncates and rewrites the file underneath it, so what
     * reaches the colleague or the doctor is empty, or half of
     * one export followed by half of another -- and the sender had no way to
     * know, having seen an ordinary share sheet and a correct file on the
     * phone.
     *
     * Now: a constrained unique name (see ExportPolicy.exportName -- the
     * grammar and the reasons are there), created with createNewFile() so the
     * filesystem itself refuses to reuse one, made read-only once written, and
     * kept for a day so a delayed reader still finds its own bytes. Only that
     * one URI is put in the intent, so only that one snapshot is granted. */
    public static void exportData(Context ctx, long cutoff, boolean glucose,
                                  boolean devices, boolean insulin,
                                  boolean weight) {
        java.io.File out = null;
        try {
            java.io.File dir = ctx.getFilesDir();
            java.io.File snapdir =
                new java.io.File(dir, ExportPolicy.EXPORT_DIR);
            /* A SUBDIRECTORY OF ITS OWN, so the cleanup below enumerates
             * snapshots and cannot see -- let alone remove -- readings.csv,
             * stelo.key or anything else the app depends on. */
            if (!snapdir.isDirectory() && !snapdir.mkdirs()) {
                Log.i(TAG, "export: cannot create " + ExportPolicy.EXPORT_DIR);
                return;
            }
            /* BEFORE the new snapshot exists, so it is not a candidate for its
             * own cleanup and the four-newest rule is spent on real history. */
            cleanupExports(snapdir);
            out = newSnapshot(snapdir);
            if (out == null) return; /* newSnapshot logged why */
            /* THE BYTES, THE CLOSE AND THE PUBLICATION ARE ExportSnapshot'S,
             * and its header says why each of the three is where it is. In
             * one line: a row is copied only once its own '\n' has been read,
             * so a source the native side is appending to cannot contribute a
             * half-written row that this side then terminates; and `out`
             * acquires its contents by a rename that happens AFTER the output
             * stream closed without throwing, so no URI can ever name a
             * prefix. A false return means every selected section was empty
             * -- nothing to share, and nothing left on disk. */
            java.util.ArrayList<ExportSnapshot.Section> secs =
                new java.util.ArrayList<ExportSnapshot.Section>();
            /* The DEVICES section is a registry, not a time series, so it is
             * never filtered -- cutoff 0. */
            if (devices)
                secs.add(new ExportSnapshot.FileSection(
                             new java.io.File(dir, "sensors.csv"), 0));
            if (glucose)
                secs.add(new ExportSnapshot.FileSection(
                             new java.io.File(dir, "readings.csv"), cutoff));
            if (insulin)
                secs.add(new ExportSnapshot.FileSection(
                             new java.io.File(dir, "insulin.csv"), cutoff));
            if (weight)
                secs.add(new ExportSnapshot.FileSection(
                             new java.io.File(dir, "weight.csv"), cutoff));
            if (!ExportSnapshot.write(new ExportSnapshot.FileSink(out),
                    secs.toArray(new ExportSnapshot.Section[0])))
                return;
            Uri uri = Uri.parse("content://com.jk.pancra.files/"
                                + out.getName());
            Intent send = new Intent(Intent.ACTION_SEND);
            /* THE GRANT IS THIS URI AND NOTHING ELSE. The flag grants what the
             * intent carries, and what it carries is one snapshot's name; the
             * provider is not exported, so nothing else in the URI space is
             * reachable without a grant, and PancraFiles refuses any name that
             * is not a snapshot's anyway. */
            send.putExtra(Intent.EXTRA_STREAM, uri);
            send.setType("text/csv");
            send.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            Intent chooser = Intent.createChooser(send, "Export pancra data");
            chooser.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            ctx.startActivity(chooser);
        } catch (Throwable t) {
            Log.i(TAG, "export: " + t);
            /* ExportSnapshot has already removed anything it wrote -- a
             * failed export leaves neither the temporary nor the name. This
             * covers the rest of the method: a snapshot that WAS published
             * and then could not be handed to a chooser is a share that never
             * happened, so it goes too rather than sitting in the directory
             * with a name nothing will ever ask for. */
            if (out != null) try {
                /* CHECKED, AND UNSERVABLE IF IT CANNOT GO. This
                 * delete's answer was dropped, so a published snapshot the
                 * chooser refused could stay in the directory under a name
                 * the provider serves -- a share the user never completed,
                 * left reachable by whatever asks next. removeOrQuarantine
                 * renames it out of the servable name space when it cannot
                 * remove it, and either way says what is left. */
                String left = ExportSnapshot.removeOrQuarantine(out);
                if (left != null)
                    Log.i(TAG, "export: could not clean up " + left);
            } catch (Throwable u) {
                Log.i(TAG, "export cleanup: " + u);
            }
        }
    }

    /* A file that did not exist a moment ago, named to the export grammar.
     *
     * createNewFile() is the point: it creates ONLY if the name is free, so
     * uniqueness is the filesystem's answer and not a promise made by the
     * random tag. The retries exist for the collision that tag makes
     * improbable (same second, same 32 bits) and for the one it does not
     * cover at all -- a snapshot the user has not exported yet this session
     * but whose name we happen to regenerate after a clock reset. Returning
     * null after a bounded number of tries beats looping in the UI thread of
     * a share the user is waiting for.
     *
     * SecureRandom rather than Random: the name is handed to another app, and
     * a guessable one is a name a future mistake could let something else ask
     * for. It costs one seeding on a user-initiated action. */
    private static java.io.File newSnapshot(java.io.File dir) {
        java.security.SecureRandom rnd = new java.security.SecureRandom();
        for (int tries = 0; tries < 8; tries++) {
            String name = ExportPolicy.exportName(
                System.currentTimeMillis() / 1000L, rnd.nextInt());
            /* THE GENERATOR CHECKS ITS OWN OUTPUT. If these two ever disagree
             * the export must fail here, where it can be logged, and not in
             * the provider -- where the user sees a share that produces an
             * empty or missing attachment in someone else's app. */
            if (!ExportPolicy.exportNameValid(name)) {
                Log.i(TAG, "export: generated an invalid name '" + name + "'");
                return null;
            }
            java.io.File f = new java.io.File(dir, name);
            try {
                if (f.createNewFile()) return f;
            } catch (java.io.IOException e) {
                Log.i(TAG, "export: createNewFile: " + e);
                return null;
            }
        }
        Log.i(TAG, "export: no free snapshot name after 8 tries");
        return null;
    }

    /* Remove snapshots that no reader can still be waiting for.
     *
     * The rule is ExportPolicy.exportSnapshotExpendable, which is where the
     * numbers and the asymmetry are argued: deleting one too early breaks a
     * recipient's read, deleting one too late costs kilobytes. This half is
     * only the enumeration, and it is deliberately narrow -- a file is a
     * candidate ONLY if its name is a valid snapshot name, so anything else
     * that ever appears in the directory is left strictly alone. */
    private static void cleanupExports(java.io.File dir) {
        try {
            java.io.File[] all = dir.listFiles();
            if (all == null) return;
            java.util.ArrayList<java.io.File> snaps =
                new java.util.ArrayList<java.io.File>();
            for (java.io.File f : all)
                if (f.isFile() && ExportPolicy.exportNameValid(f.getName()))
                    snaps.add(f);
            /* NEWEST FIRST, so "rank" means what the rule thinks it means.
             * By mtime and not by name: the name's timestamp comes from the
             * wall clock, which can move backwards between exports, and a
             * clock correction must not make the newest snapshot look like the
             * oldest and get it deleted. */
            java.util.Collections.sort(snaps,
                new java.util.Comparator<java.io.File>() {
                    @Override public int compare(java.io.File a, java.io.File b) {
                        long x = a.lastModified(), y = b.lastModified();
                        return (x == y) ? 0 : (x > y ? -1 : 1);
                    }
                });
            long now = System.currentTimeMillis();
            for (int i = 0; i < snaps.size(); i++) {
                java.io.File f = snaps.get(i);
                if (ExportPolicy.exportSnapshotExpendable(now,
                        f.lastModified(), i)) {
                    Log.i(TAG, "export: retiring " + f.getName());
                    /* THE RETENTION DELETE ANSWERS TOO. A
                     * snapshot that will not go on being expendable -- it
                     * stays past its window and stays servable -- is exactly
                     * the state the retention rule exists to prevent, and
                     * silence makes it invisible. Quarantined if it cannot be
                     * removed, so at least nothing can ask for it. */
                    String left = ExportSnapshot.removeOrQuarantine(f);
                    if (left != null)
                        Log.i(TAG, "export: could not retire " + left);
                }
            }
        } catch (Throwable t) { Log.i(TAG, "export cleanup: " + t); }
    }
}
