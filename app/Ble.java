// SPDX-License-Identifier: GPL-3.0
// Ble.java --- BLE GATT pipe (scan/connect/read/write/subscribe)
// Copyright 2026 Jakob Kastelic

/* Dumb pipe to the Android BLE APIs: exposes primitives, interprets nothing.
 * All protocol meaning lives on the C side (dexble.c). Java only:
 *   - scans and reports advertisements,
 *   - connects / discovers / subscribes / writes on request,
 *   - serialises GATT operations (Android allows one in flight at a time),
 *   - forwards connection events, notifications and write-acks to native.
 */
package com.jk.pancra;

import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattDescriptor;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothProfile;
import android.bluetooth.le.BluetoothLeScanner;
import android.bluetooth.le.ScanCallback;
import android.bluetooth.le.ScanResult;
import android.bluetooth.le.ScanSettings;
import android.app.Activity;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.util.Log;

import java.util.ArrayDeque;
import java.util.UUID;

public final class Ble {
    private static final String TAG = "pancra";
    private static final UUID CCCD =
        UUID.fromString("00002902-0000-1000-8000-00805f9b34fb");

    /* ---- settings-menu helpers (ctx is the NativeActivity, i.e. an Activity) ---- */

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
     * This used to write files/pancra.csv -- one name, truncated on every
     * export -- and share a content:// URI naming it. The receiving app opens
     * that URI when it needs the bytes, not when the chooser closes: a mail
     * client reads the attachment as the message is SENT, which for a draft, a
     * queued send, or a phone that regains network overnight is minutes or
     * hours later. A second export truncated and rewrote the file underneath
     * it, so what reached the colleague or the doctor was empty, or half of
     * one export followed by half of another -- and the sender had no way to
     * know, having seen an ordinary share sheet and a correct file on the
     * phone.
     *
     * Now: a constrained unique name (see BoundaryLogic.exportName -- the
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
                new java.io.File(dir, BoundaryLogic.EXPORT_DIR);
            /* A SUBDIRECTORY OF ITS OWN, so the cleanup below enumerates
             * snapshots and cannot see -- let alone remove -- readings.csv,
             * stelo.key or anything else the app depends on. */
            if (!snapdir.isDirectory() && !snapdir.mkdirs()) {
                Log.i(TAG, "export: cannot create " + BoundaryLogic.EXPORT_DIR);
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
            if (out != null) try { out.delete(); } catch (Throwable u) { }
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
            String name = BoundaryLogic.exportName(
                System.currentTimeMillis() / 1000L, rnd.nextInt());
            /* THE GENERATOR CHECKS ITS OWN OUTPUT. If these two ever disagree
             * the export must fail here, where it can be logged, and not in
             * the provider -- where the user sees a share that produces an
             * empty or missing attachment in someone else's app. */
            if (!BoundaryLogic.exportNameValid(name)) {
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
     * The rule is BoundaryLogic.exportSnapshotExpendable, which is where the
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
                if (f.isFile() && BoundaryLogic.exportNameValid(f.getName()))
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
                if (BoundaryLogic.exportSnapshotExpendable(now,
                        f.lastModified(), i)) {
                    Log.i(TAG, "export: retiring " + f.getName());
                    f.delete();
                }
            }
        } catch (Throwable t) { Log.i(TAG, "export cleanup: " + t); }
    }

    /* ---- REMOTE push: one datapoint per call to the configured server ---- */

    /* One background thread with a small BOUNDED queue. Pushes must never block
     * the BLE binder thread that decoded the reading (it holds the driver lock),
     * and an unreachable server must not accumulate work without bound.
     *
     * THE QUEUE NEVER HOLDS MORE THAN ONE TASK, and that is a load-bearing
     * invariant, not an accident: all three submitters (remoteRange,
     * remoteCursor, remoteBatch) are `static synchronized` AND gated on
     * `sBusy != 0`, so at most one is ever in flight or queued. The capacity
     * and the discard policy below are therefore unreachable today.
     *
     * ANY NEW SUBMITTER MUST TAKE THE SAME sBusy GATE. Each of the three
     * clears sBusy in a `finally` INSIDE the submitted Runnable, and
     * DiscardOldestPolicy drops a task SILENTLY -- no exception, so the
     * submitting method's catch never runs. A fourth, ungated submitter that
     * filled the queue could therefore discard the Runnable carrying the
     * sBusy clear, wedging every future push for the life of the process
     * with nothing logged.
     *
     * (The old design pushed each datapoint individually, which is what the
     * bounded queue and "drop the oldest, the newest points matter most"
     * rationale were for. The cursor protocol below replaced it; nothing
     * re-reads that reasoning now.) */
    private static final java.util.concurrent.ThreadPoolExecutor pushExec =
        new java.util.concurrent.ThreadPoolExecutor(0, 1, 30,
            java.util.concurrent.TimeUnit.SECONDS,
            new java.util.concurrent.ArrayBlockingQueue<Runnable>(64),
            new java.util.concurrent.ThreadPoolExecutor.DiscardOldestPolicy());

    /* (The old cursor-driven push lived here: a cursor from GET /api/last,
     * then chronological batches to /glucose and /units with acknowledgement
     * tags so a lost reply could not double-write. All of it existed to make a
     * fire-and-forget push lossless; the replica protocol below asks the
     * server what it has and replaces whole buckets, so there is nothing left
     * to track.)
     *
     * ---- what remains of the old banner ----
     *
     * The old design pushed each new datapoint once and dropped it on any
     * failure -- every reading taken while the server was unreachable was
     * gone for good. Now native asks the server what it already has (the
     * CURSOR, GET /api/last), then sends everything newer in chronological
     * batches; a batch that fails for ANY reason is simply retried later
     * from the cursor, and the server skips what it already stored, so a
     * reply lost in flight cannot double-write. See srv/logs.c.
     *
     * All of it runs on pushExec (never a BLE binder thread, which holds
     * the driver lock). Native drives one step per tick and reads the
     * state back through the three getters below -- no callbacks into C
     * from arbitrary threads beyond the existing onRemoteOk. */
    /* The tag of the last batch the server CONFIRMED, per set. Native passes
     * its outbox position as the tag and advances only when it comes back
     * here -- so a batch that failed, timed out, or whose reply was lost
     * leaves the position untouched and is simply resent. */
    /* What the LAST attempt actually got back. Failures used to be visible
     * only in logcat, so a server quietly refusing every batch looked
     * exactly like a working link that had nothing to send. */


    /* ---- the PULL direction ----
     * (The pull direction went with it: the server holds exactly what this
     * phone gave it, so there is nothing there to import.) */


    /* A word that fits the row, instead of a Java class name. */
    private static String shortErr(Throwable e) {
        String n = e.getClass().getSimpleName();
        if (n.contains("Timeout")) return "TIMEOUT";
        if (n.contains("UnknownHost")) return "NO HOST";
        if (n.contains("Connect")) return "REFUSED";
        if (n.contains("NoRoute")) return "NO ROUTE";
        return "ERROR";
    }

    /* (The old one-point-per-reading push is gone: the cursor-driven sync
     * above replaces it and cannot drop a point. The server keeps its "/"
     * route for other clients.) */

    /* mode: 0 portrait, 1 landscape, 2 gravity (sensor always), 3 system (sensor
     * only if the OS auto-rotate setting allows it) */
    public static void setOrientation(Context ctx, int mode) {
        try {
            if (!(ctx instanceof Activity)) return;
            int o;
            switch (mode) {
                case 1:  o = ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE;   break;
                case 2:  o = ActivityInfo.SCREEN_ORIENTATION_FULL_SENSOR; break;
                case 3:  o = ActivityInfo.SCREEN_ORIENTATION_USER;        break;
                default: o = ActivityInfo.SCREEN_ORIENTATION_PORTRAIT;    break;
            }
            ((Activity) ctx).setRequestedOrientation(o);
        } catch (Throwable t) { Log.i(TAG, "orient: " + t); }
    }
    public static boolean permGranted(Context ctx, String perm) {
        try { return ctx.checkSelfPermission(perm) == PackageManager.PERMISSION_GRANTED; }
        catch (Throwable t) { return false; }
    }
    public static void requestPerm(Context ctx, String perm) {
        try {
            if (ctx instanceof Activity)
                ((Activity) ctx).requestPermissions(new String[]{ perm }, 0);
        } catch (Throwable t) { Log.i(TAG, "reqperm: " + t); }
    }
    /* app details page — the only place the user can REVOKE an already-granted one */
    public static void openAppSettings(Context ctx) {
        try {
            android.content.Intent i = new android.content.Intent(
                android.provider.Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                android.net.Uri.parse("package:" + ctx.getPackageName()));
            i.addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK);
            ctx.startActivity(i);
        } catch (Throwable t) { Log.i(TAG, "appsettings: " + t); }
    }

    /* ---- background-running controls a CGM needs alive ---- */
    /* battery-optimisation exemption: readable + requestable (revoke via settings) */
    public static boolean isBatteryUnrestricted(Context ctx) {
        try {
            android.os.PowerManager pm = ctx.getSystemService(android.os.PowerManager.class);
            return pm != null && pm.isIgnoringBatteryOptimizations(ctx.getPackageName());
        } catch (Throwable t) { return false; }
    }
    public static void requestBatteryOpt(Context ctx) {
        try {
            android.content.Intent i = new android.content.Intent(
                android.provider.Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS,
                android.net.Uri.parse("package:" + ctx.getPackageName()));
            i.addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK);
            ctx.startActivity(i);
        } catch (Throwable t) { Log.i(TAG, "reqbatt: " + t); }
    }
    /* app standby bucket: readable (own app), NOT settable without system privilege */
    public static int standbyBucket(Context ctx) {
        try {
            android.app.usage.UsageStatsManager u = (android.app.usage.UsageStatsManager)
                ctx.getSystemService(Context.USAGE_STATS_SERVICE);
            return u != null ? u.getAppStandbyBucket() : -1;
        } catch (Throwable t) { return -1; }
    }
    /* background-execution restriction: readable, changed only in app settings */
    public static boolean isBgRestricted(Context ctx) {
        try {
            android.app.ActivityManager am = (android.app.ActivityManager)
                ctx.getSystemService(Context.ACTIVITY_SERVICE);
            return am != null && am.isBackgroundRestricted();
        } catch (Throwable t) { return false; }
    }

    /* start the foreground service so BLE keeps running in the background, and
     * ask to be exempted from battery optimisation so Doze can't kill us */
    public static void startService(Context ctx) {
        /* The native side is up by the time this is called -- it is called
         * FROM native. Recording that is what lets a service recreated later,
         * on its own, tell "the app asked for me" from "I am alone in a fresh
         * process and can monitor nothing" (see PancraService.serviceAction).
         *
         * It also clears any MONITORING STOPPED warning: the condition it
         * describes has just been fixed by the app being open. */
        PancraService.nativeReady();
        PancraService.clearStoppedWarning(ctx);
        PancraService.start(ctx);
        PancraService.requestNoBatteryOpt(ctx);
    }

    /* Push the live glucose + a 3H plot bitmap into the ongoing notification
     * (shown on the lock screen / shade). Called from native each reading. */
    public static void showGlucose(Context ctx, String title, String text,
                                   String value,
                                   int[] px, int w, int h, int lockscr) {
        PancraService.showGlucose(ctx, title, text, value, px, w, h, lockscr);
    }

    /* ---- cloud sync: this class only moves bytes ----
     *
     * The protocol lives in native (src/sync.c); Java is here because the
     * platform's TLS is free and a C TLS stack would cost about a megabyte of
     * library in a 143 kB app. So: native decides every byte on the wire and
     * calls syncHttp to post it.
     *
     * syncRun/syncPair BLOCK -- they make several round trips -- so they are
     * only ever called on pushExec, never on a binder or UI thread. syncHttp
     * is then called back FROM that same worker thread, which is why nothing
     * here touches the main looper.
     */
    private static volatile int sSyncCode = -1;
    /* WHY the last one failed, when it failed before the server answered: a
     * BoundaryLogic.NET_* kind, read by native alongside the status. */
    private static volatile int sSyncFail = BoundaryLogic.NET_OK;
    private static volatile boolean sSyncBusy;

    static native int syncRun();
    static native int syncPair(String email, String code);
    /* Pull back everything the server holds and this phone does not. One
     * request per missing bucket, so it blocks for as long as the recovery
     * takes -- worker only, like the other two. */
    static native int syncRestore();

    /* The status of the LAST syncHttp call, read by native straight after. */
    static int syncCode() { return sSyncCode; }

    /* ...and, when there was no status, WHY. Native turns this into the
     * outcome the screen shows and the scheduler retries (or does not). */
    static int syncFail() { return sSyncFail; }

    /* THE WATCHDOG THAT ACTUALLY CUTS A WEDGED EXCHANGE OFF.
     *
     * Item 123's deadline is arithmetic, and arithmetic cannot interrupt a
     * blocked socket read. The loop in BoundaryLogic re-checks the clock
     * every time around, which ends a server that DRIBBLES -- one byte,
     * another byte -- but a server that accepts the connection and then says
     * nothing at all leaves the worker parked inside a single blocking call,
     * where no amount of checking between calls is ever reached. (The read
     * idle timeout does cover that one case; the write does not have one at
     * all, and the gaps between connect, request and response are covered by
     * nothing.) So there is a timer, and when it fires it CLOSES THE SOCKET,
     * which is the only thing that makes a blocked read or write return.
     *
     * One shared thread, not one per request: a sync is dozens of requests
     * and a restore is one per bucket. Core size 1 with core-thread timeout
     * on, so the thread evaporates when nothing has synced for half a minute
     * and a phone that is not syncing pays nothing for it. Daemon, so it can
     * never be the reason a process lingers. removeOnCancel, because the
     * normal path cancels every single one of these and a queue that keeps
     * cancelled tasks until their deadline would hold a reference to every
     * connection of the whole sync.
     *
     * This is NOT the sync worker. It must not be: the whole point is that it
     * runs while pushExec's only thread is stuck. */
    private static final java.util.concurrent.ScheduledThreadPoolExecutor
        syncWatch = new java.util.concurrent.ScheduledThreadPoolExecutor(1,
            new java.util.concurrent.ThreadFactory() {
                public Thread newThread(Runnable r) {
                    Thread t = new Thread(r, "pancra-syncwatch");
                    t.setDaemon(true);
                    return t;
                }
            });
    static {
        syncWatch.setKeepAliveTime(30, java.util.concurrent.TimeUnit.SECONDS);
        syncWatch.allowCoreThreadTimeOut(true);
        syncWatch.setRemoveOnCancelPolicy(true);
    }

    /* Called FROM native on the sync worker. Returns the response body, or
     * null; the status goes in sSyncCode, because a JNI call that returns two
     * things needs two calls and this is the cheaper pair.
     *
     * ---- WHAT THIS USED TO DO TO SOMEBODY WEARING A CGM (items 122, 123) --
     *
     * It read the reply `while ((n = in.read(buf)) > 0)` into a
     * ByteArrayOutputStream with no ceiling, then handed the finished array
     * to native -- which measured it and refused it if it did not fit. The
     * refusal was correct and it was three copies too late. A server that
     * answered a bucket fetch with a gigabyte, whether hostile,
     * misconfigured, or a captive portal serving an endless error page, got
     * this app to allocate until the heap gave out. An OutOfMemoryError does
     * not land where it was caused: it lands on the next allocation anywhere
     * in the process, which here is as likely to be the BLE callback decoding
     * a glucose reading. The screen stops updating, the low alarm stops
     * sounding, and nothing says anything is wrong -- because nothing knows.
     *
     * And the clock was per-read. setReadTimeout restarts on every byte, so a
     * server sending one byte every nineteen seconds never tripped it and
     * never finished. This method runs on pushExec, which has a MAXIMUM of
     * one thread and also carries pairing and restore, so one such server
     * stopped every sync, every pairing attempt and every restore for the
     * life of the process, silently, with the app believing a request was
     * merely still in progress.
     *
     * Both bounds now live in BoundaryLogic, where the host JVM runs them
     * against a dribbling stream and a lying Content-Length. What is left
     * here is what needs Android: the connection, the two idle timeouts, and
     * the watchdog above. */
    static byte[] syncHttp(String server, int port, String method, String path,
                           String hdr, byte[] body) {
        sSyncCode = -1;
        sSyncFail = BoundaryLogic.NET_OK;
        /* MONOTONIC, and the reason is item 70's: this is an elapsed-time
         * question, and currentTimeMillis answers a different one. An NTP
         * correction or the user setting the date mid-sync would move a wall
         * clock, and a deadline computed from a clock that moved BACKWARDS
         * grows -- it would hand exactly the wedged request this exists to
         * kill even more time. nanoTime has no epoch and cannot be set. */
        final long startMono = System.nanoTime();
        /* Written by the watchdog thread, read by this one: a close that WE
         * caused must be reported as a timeout, not as whatever SocketException
         * the platform raises on a socket somebody yanked. */
        final java.util.concurrent.atomic.AtomicBoolean cut =
            new java.util.concurrent.atomic.AtomicBoolean(false);
        java.util.concurrent.ScheduledFuture<?> alarm = null;
        java.net.HttpURLConnection c = null;
        try {
            /* https, always: the session this signs is not the only thing on
             * the wire -- the record itself is. The platform supplies the TLS,
             * so this costs nothing but the scheme. */
            final java.net.HttpURLConnection conn =
                (java.net.HttpURLConnection) new java.net.URL(
                    "https", server, port, path).openConnection();
            c = conn;
            /* ARMED HERE, before a single byte moves. openConnection() does
             * not connect -- HttpURLConnection is lazy -- so starting the
             * timer at this line is what makes the budget cover the connect,
             * the TLS handshake, the request body, the status line, the
             * response body AND the gaps between them. Arming it after
             * getResponseCode(), which is the obvious-looking place, would
             * have left the connect and the write outside the deadline: the
             * connect has its own 8 s, but the write has NO timeout of any
             * kind, so a server that accepts a connection and never drains
             * the request would have hung exactly as before. */
            alarm = syncWatch.schedule(new Runnable() {
                public void run() {
                    cut.set(true);
                    /* disconnect() closes the underlying socket, so whichever
                     * blocking call the worker is parked in returns with an
                     * IOException. It is the only lever that reaches a thread
                     * already inside read(). */
                    try { conn.disconnect(); } catch (Throwable t) { }
                }
            }, BoundaryLogic.SYNC_EXCHANGE_MS,
               java.util.concurrent.TimeUnit.MILLISECONDS);
            c.setConnectTimeout(8000);
            c.setReadTimeout(20000);
            c.setRequestMethod(method);
            c.setUseCaches(false);
            /* NO AUTOMATIC REDIRECTS, and this single line is item 124.
             * HttpURLConnection follows 3xx by default, and a followed
             * redirect changes the target -- or the METHOD -- of a request
             * whose signature covers exactly the method and path native
             * chose. The 2xx that comes back from wherever it went is then
             * reported as the answer to the request we signed. See
             * BoundaryLogic.redirectRefused for what that costs; the refusal
             * of every 3xx lives there, and `make javacheck` fails the build
             * if this line goes away. */
            c.setInstanceFollowRedirects(false);
            if (hdr != null) {
                /* "Name: value\r\n" lines, exactly as native built them. */
                for (String line : hdr.split("\r\n")) {
                    int colon = line.indexOf(':');
                    if (colon > 0)
                        c.setRequestProperty(line.substring(0, colon),
                                             line.substring(colon + 1).trim());
                }
            }
            /* THE LIMIT IS NATIVE'S LIMIT. BoundaryLogic.SYNC_BODY_MAX is
             * SYNC_BUF_MAX - 1 from app/sync.h, which is the `outcap` every
             * sync.c call site passes to jni_http and the exact length that
             * jni_http still accepts. `make javacheck` fails the build if the
             * two ever disagree, so this is one number with a copy the build
             * will not let rot -- not a second, independent guess.
             *
             * The exchange itself -- write the body and CLOSE it, then the
             * status, then refuse a 3xx, then the bounded read, with every
             * stream closed on every path -- is BoundaryLogic.runExchange,
             * for the same reason the read loop is: none of those decisions
             * needs Android, and on this side of the boundary they would be
             * testable only on a phone. What is left below is the wiring the
             * platform actually owns, including WHICH stream carries the body
             * (getInputStream for a 2xx, getErrorStream for the rest, and
             * getErrorStream is null when there is no error body). */
            BoundaryLogic.Exchange x = BoundaryLogic.runExchange(
                new BoundaryLogic.SyncConn() {
                    public java.io.OutputStream openRequest(int len)
                            throws java.io.IOException {
                        conn.setDoOutput(true);
                        conn.setFixedLengthStreamingMode(len);
                        return conn.getOutputStream();
                    }
                    public int status() throws java.io.IOException {
                        return conn.getResponseCode();
                    }
                    public String header(String name) {
                        return conn.getHeaderField(name);
                    }
                    public java.io.InputStream openResponse(int status)
                            throws java.io.IOException {
                        return (status / 100 == 2) ? conn.getInputStream()
                                                   : conn.getErrorStream();
                    }
                },
                body, new BoundaryLogic.MonoClock() {
                    public long nanos() { return System.nanoTime(); }
                },
                startMono, BoundaryLogic.SYNC_EXCHANGE_MS,
                BoundaryLogic.SYNC_BODY_MAX);
            if (x.body == null || cut.get()) {
                /* SET THE STATUS BACK TO -1, and this line is load-bearing.
                 * Native reads syncCode() separately from the returned array
                 * (syncjni.c, jni_http): a null return with sSyncCode still
                 * holding the 200 we got a moment ago is read there as a
                 * successful request with an EMPTY body -- an upload we never
                 * completed treated as accepted, a bucket fetch that returned
                 * nothing treated as a bucket that IS empty, which is the one
                 * kind of answer that drives the loop that deletes. A refused
                 * body is "nothing happened", and nothing happened is -1.
                 *
                 * IT IS ALSO BELT AND BRACES NOW, DELIBERATELY. Since item
                 * 124 the status is not published until there are bytes to
                 * publish it with -- runExchange returns code -1 with every
                 * null body, and the assignment below is the only one -- so
                 * this line writes -1 over the -1 set on entry. It stays
                 * because the invariant it enforces is one line's edit away
                 * from being lost: the obvious "simplification" here is to
                 * read the status straight off the connection as soon as it
                 * is known, and that is exactly the code this replaced. */
                sSyncCode = -1;
                sSyncFail = cut.get() ? BoundaryLogic.NET_TIMEOUT : x.fail;
                return null;
            }
            sSyncCode = x.code;
            return x.body;
        } catch (Throwable t) {
            /* Any failure is "the request did not happen": native retries the
             * whole sync later, and a partial answer must never look like a
             * complete one. But WHICH failure is not lost: a name that does
             * not resolve and a refused certificate are the user's to fix,
             * and retrying them for ever helps nobody. */
            sSyncCode = -1;
            /* A socket WE closed reports as a timeout. netFailure would see a
             * SocketException reading "Socket closed" and call it NET_OTHER,
             * which on the screen is an unexplained failure rather than "that
             * server is too slow" -- and NET_OTHER is retried on a schedule
             * that assumes a transient fault. */
            sSyncFail = cut.get() ? BoundaryLogic.NET_TIMEOUT
                                  : BoundaryLogic.netFailure(t);
            return null;
        } finally {
            if (alarm != null)
                alarm.cancel(false);
            /* NOT disconnect() ON THE NORMAL PATH: that tears the socket down,
             * and the socket is the expensive part. A TLS handshake to the
             * server costs 100-600 ms and the request itself costs 5, so a
             * sync that reconnects per bucket spends all its time shaking
             * hands. Leaving the connection in the pool lets the whole sync
             * ride one handshake.
             *
             * A connection the watchdog cut is the exception, and it is
             * already gone -- disconnect() is what the watchdog called. That
             * one must NOT go back in the pool: it is attached to a server
             * that just demonstrated it does not finish, and the next request
             * to reuse it would inherit the stall.
             *
             * ITEM 125 CHECKED THAT REASONING AND IT HOLDS -- but it was only
             * half the story while nothing closed the STREAMS. Not
             * disconnecting is what keeps the socket; closing the request and
             * response streams is what makes keeping it correct, because a
             * connection with unread bytes still on it is not a connection
             * the next request may use. runExchange now closes both on every
             * path, consumed or aborted, so what this block leaves in the
             * pool is a connection that is actually reusable rather than one
             * held open by a stream nobody ever finished with. There is
             * still nothing to do HERE: the closing belongs where the streams
             * are opened, inside try-with-resources, not in a finally that
             * would have to re-derive which of them exist. */
        }
    }

    /* Ask for a sync. Coalesced: a second request while one is running is
     * dropped rather than queued, because a sync always sends whatever is
     * current -- running it twice back to back would achieve nothing.
     *
     * RETURNS WHETHER THE WORKER TOOK IT, and false for that coalesced drop.
     * The native scheduler advances its "we have synced up to here" stamp
     * when it asks, so a dropped ask that reported success left it believing
     * the newest data was on its way when nothing had been queued -- and with
     * nothing new arriving afterwards, the next attempt waited out the
     * six-hour safety interval. A drop is not a failure of the sync, but it
     * IS a failure of the request, and only the caller can tell the
     * difference. */
    static boolean syncSoon() {
        if (sSyncBusy)
            return false;
        sSyncBusy = true;
        pushExec.execute(new Runnable() {
            public void run() {
                try { syncRun(); } catch (Throwable t) { /* reported native */ }
                finally { sSyncBusy = false; }
            }
        });
        return true;
    }

    /* A restore is user-initiated from the REMOTE screen and reports the same
     * way a sync does, through the LAST STATUS row. Not gated on sSyncBusy: a
     * restore and a sync must not overlap, and the worker is single-threaded,
     * so the queue already serialises them. */
    static void syncRestoreSoon() {
        pushExec.execute(new Runnable() {
            public void run() {
                try { syncRestore(); } catch (Throwable t) { /* reported native */ }
            }
        });
    }

    /* Pairing is user-initiated and its result is shown on screen, so it runs
     * on the same worker but reports through onSyncPaired. */
    static void syncPairSoon(final String email, final String code) {
        pushExec.execute(new Runnable() {
            public void run() { syncPair(email, code); }
        });
    }

    /* ---- Java -> C callbacks (bound via RegisterNatives in dexble.c) ---- */
    static native void onAdvert(String name, String mac, int rssi);
    /* Every GATT callback carries its link id, so native can route the event to
     * the protocol driver that owns that sensor. */
    static native void onConnected(int link);
    static native void onDisconnected(int link, int status);
    static native void onNotify(int link, String uuid, byte[] data);
    static native void onWritten(int link, String uuid, int status);
    static native void onRssi(int link, int rssi);
    static native void onRead(int link, String uuid, byte[] data);
    /* Service heartbeat -> native alarm re-evaluation. Not a BLE event: it
     * exists so the stale-data alarm still fires with no activity alive. */
    static native void onTick();
    /* Push worker -> native: the remote server acknowledged a datapoint. */
    static native void onRemoteOk();
    /* OS bond state changed for `mac`: BOND_NONE 10, BOND_BONDING 11,
     * BOND_BONDED 12 (the framework's own constants, passed through). */
    static native void onBondState(String mac, int state);

    /* Java -> C: the scan of generation `gen` never actually started.
     *
     * Separate from every callback above because it is not an event about a
     * sensor: it is this process learning that its own request was refused. */
    static native void onScanFailed(int gen, int err);

    private static BluetoothLeScanner scanner;
    private static ScanCallback scanCb;
    /* THE GENERATION OF THE CALLBACK IN scanCb; 0 when none is installed.
     *
     * Allocated by native (start_scan) and passed into scan(), so the side that
     * latches "a scan is running" is the side that names it. Read and written
     * under scanLock because the failure callback arrives on a Bluetooth binder
     * thread while the main thread starts and stops scans. */
    private static int scanGen;

    /* GUARDS scanner, scanCb AND scanGen AS ONE, and nothing else.
     *
     * They were main-thread-only fields, which they no longer are: onScanFailed
     * is delivered on a binder thread and has to decide whether the callback it
     * is complaining about is still the installed one -- a decision over two
     * fields at once, so reading them unsynchronised could release a
     * REPLACEMENT scan's callback while keeping the dead one's generation.
     *
     * Its own lock rather than the Ble class monitor: link() is `static
     * synchronized`, so the class monitor is held on binder threads for every
     * GATT operation, and a scan failure has no business waiting behind one.
     *
     * NEVER HELD ACROSS A CALL INTO NATIVE, for the reason spelt out in
     * Link.pump: native callbacks take the C driver_lock, which is a spin lock
     * the main thread also takes, and a lock cycle through JNI burns a core
     * rather than merely blocking. */
    private static final Object scanLock = new Object();

    /* One independent link per sensor.
     *
     * GATT operations must be serialised, but only WITHIN a link -- the stack
     * happily runs several connections at once. A single shared queue would
     * serialise them against each other, so a slow or stalled sensor would hold
     * up every other one and make it miss its advertising window. Each Link
     * therefore owns its own gatt, queue and busy flag, and its own callback
     * instance so every event already knows which link it belongs to. */
    static final int MAX_LINKS = 8; /* == LINK_MAX (dexdriver.h); crosschecked */

    /* ONE QUEUED GATT OPERATION, BOUND TO THE CLIENT IT WAS DEQUEUED FOR.
     *
     * A Runnable could only close over the Link and re-read L.gatt when it
     * ran, which is exactly the hazard: between enqueue and run, and between
     * one statement of the body and the next, a disconnect can clear that
     * field and a reconnect can replace it. The client and its generation are
     * arguments now, so an op body has nothing else to reach for. */
    interface Act { void run(BluetoothGatt g, int gen); }

    private static final class Link {
        final int id;
        BluetoothGatt gatt;
        /* Bumped whenever the link is torn down or a new connect starts.
         *
         * A boolean "pending" flag could not distinguish WHICH client a
         * DISCONNECTED callback belonged to once L.gatt had been nulled, so any
         * disconnect in the window cleared it and connect() then threw away the
         * live client it had just created. A generation does distinguish:
         * connect() publishes only if the generation it started with is still
         * current, and only the paths that genuinely invalidate a connect
         * (disconnect(), or a disconnect of the PUBLISHED client) bump it. */
        int gen;
        /* True between connect()'s clear and its publish, so disconnect() can
         * tell "an attempt is in flight" from "nothing to tear down". */
        boolean connecting;
        final ArrayDeque<Act> ops = new ArrayDeque<>();
        boolean busy;
        Link(int id) { this.id = id; }

        void enqueue(Act a) {
            synchronized (this) { ops.add(a); }
            pump();
        }
        /* Is this still the client the op was handed? Callers use it before
         * reporting a result, so a failure raised against a replaced client is
         * never mistaken for a failure of the live one. */
        synchronized boolean current(int myGen) {
            return gen == myGen && gatt != null;
        }
        /* Dequeue under the monitor, then run the op OUTSIDE it.
         *
         * An op body calls back into native on its failure paths (characteristic
         * not found, a write the stack rejects), and those callbacks take the C
         * driver_lock. The connect path meanwhile holds driver_lock and enters
         * this class. Running the op while holding the monitor closes that into
         * a deadlock -- and since driver_lock is a spin lock, a thread would
         * burn a core forever rather than merely block. Never hold this monitor
         * across a call into native. */
        void pump() {
            Act a;
            BluetoothGatt g;
            int myGen;
            synchronized (this) {
                if (busy || ops.isEmpty() || gatt == null) return;
                busy = true;
                a = ops.poll();
                /* THE CLIENT AND ITS GENERATION, CAPTURED TOGETHER, HERE.
                 *
                 * Every op body used to read L.gatt to find its characteristic
                 * and then read L.gatt AGAIN to act on it, holding no lock
                 * across the two. disconnect() and the DISCONNECTED callback
                 * both run in that window and both null it -- so the second
                 * read was a NullPointerException on a link that was merely
                 * disconnecting normally. Worse when a reconnect had already
                 * published a REPLACEMENT: the two reads then returned
                 * different clients, and the op read a characteristic off one
                 * connection and wrote it to another.
                 *
                 * Captured under the monitor, both are one decision: the op
                 * runs against the client it was dequeued for, or against
                 * nothing. */
                g = gatt;
                myGen = gen;
            }
            try {
                a.run(g, myGen);
            } catch (Throwable t) {
                Log.i(TAG, "op: " + t);
                done(myGen);
            }
        }
        /* $myGen is the generation the completing op was dequeued for.
         *
         * A completion from a REPLACED client must not advance the current
         * one. Android allows one operation in flight per client, so clearing
         * `busy` on behalf of a stale op pumps a queue that already has one
         * running, and the op it starts is silently dropped -- the link then
         * stalls, and during first-time pairing nothing recovers it. */
        void done(int myGen) {
            synchronized (this) {
                if (gen != myGen) return;
                busy = false;
            }
            pump();
        }
        synchronized void reset() { ops.clear(); busy = false; }
    }

    private static final Link[] links = new Link[MAX_LINKS];

    private static synchronized Link link(int id) {
        if (id < 0 || id >= MAX_LINKS) return null;
        if (links[id] == null) links[id] = new Link(id);
        return links[id];
    }

    /* MAC of a bonded device whose name starts with `prefix`, or "" if there is
     * none. Bonded-device names are reliable, unlike the advertised local name
     * (often absent), so this resolves our sensor's address deterministically.
     * Used to re-lock after an update that cleared the saved MAC but kept the
     * key, so reconnect never has to guess from adverts.
     *
     * The prefix comes from the caller -- the registry knows which family the
     * user actually paired. It is NOT hardcoded here: a phone typically has
     * other people's (or the official app's) sensors bonded, and matching any
     * Dexcom would let this grab one the user never chose.
     * Needs BLUETOOTH_CONNECT (held). */
    public static String bondedSensor(Context ctx, String prefix) {
        try {
            if (prefix == null || prefix.isEmpty()) return "";
            BluetoothManager bm =
                (BluetoothManager) ctx.getSystemService(Context.BLUETOOTH_SERVICE);
            BluetoothAdapter ad = (bm == null) ? null : bm.getAdapter();
            if (ad == null) return "";
            for (BluetoothDevice d : ad.getBondedDevices()) {
                String nm = d.getName();
                if (nm != null && nm.startsWith(prefix)) return d.getAddress();
            }
        } catch (Throwable t) { Log.i(TAG, "bondedSensor: " + t); }
        return "";
    }

    /* ---- scanning ----
     *
     * `gen` identifies this scan for as long as it is the installed one. Native
     * allocates it and latches its own "scanning" flag against it, so a refusal
     * that arrives seconds later can be matched to the scan it belongs to
     * instead of being applied to whichever scan is live by then. */
    public static String scan(Context ctx, final int gen) {
        try {
            BluetoothManager bm =
                (BluetoothManager) ctx.getSystemService(Context.BLUETOOTH_SERVICE);
            BluetoothAdapter ad = (bm == null) ? null : bm.getAdapter();
            if (ad == null)      return "NO BLUETOOTH";
            if (!ad.isEnabled()) return "BLUETOOTH OFF";
            BluetoothLeScanner sc = ad.getBluetoothLeScanner();
            if (sc == null) return "NO LE SCANNER";
            ScanCallback cb = new ScanCallback() {
                @Override public void onScanResult(int type, ScanResult r) {
                    String name = (r.getScanRecord() == null)
                                ? null : r.getScanRecord().getDeviceName();
                    /* The advertised local name is frequently absent in Dexcom
                     * adverts; fall back to the device's cached name (reliable
                     * for a device we've seen/bonded) so the Stelo/G7 filter has
                     * something to match. Needs BLUETOOTH_CONNECT (held). */
                    if (name == null || name.isEmpty()) {
                        try { name = r.getDevice().getName(); }
                        catch (Throwable t) { /* no CONNECT perm yet */ }
                    }
                    onAdvert(name == null ? "" : name,
                             r.getDevice().getAddress(), r.getRssi());
                }
                /* THIS USED TO BE THE WHOLE HANDLER: one log line.
                 *
                 * Native had already latched "a scan is running" -- and every
                 * recovery path it has starts only when that flag is clear --
                 * so a scan the platform refused here left the app never
                 * scanning again for the life of the process, with nothing on
                 * screen to say so. No sensor reconnect after a dropout, no
                 * meter noticed when switched on, and a last reading that
                 * simply stops ageing forward.
                 *
                 * `gen`, not "the current scan": by the time this arrives the
                 * scan may have been replaced, and cancelling a live
                 * replacement would be the same outage with a different
                 * cause. */
                @Override public void onScanFailed(int err) {
                    scanFailed(gen, err);
                }
            };
            /* PUBLISHED BEFORE THE REQUEST GOES OUT. onScanFailed can be
             * delivered on a binder thread before startScan() has returned
             * here, and a failure that found scanCb still unset would be
             * discarded as "nothing installed" -- leaving native latched on a
             * scan that had already died. */
            synchronized (scanLock) { scanner = sc; scanCb = cb; scanGen = gen; }
            sc.startScan(null,
                new ScanSettings.Builder()
                    .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build(), cb);
            return null;
        } catch (Throwable t) {
            /* A throw from startScan (SecurityException once BLUETOOTH_SCAN is
             * revoked) means no registration was ever made, so the callback
             * published above is not one: drop it, or the next stop() would
             * report success for a scan that never existed and a later failure
             * for this generation would be believed. Only if it is still ours
             * -- nothing else starts a scan while native holds the main
             * thread here, but the rule is the same rule as everywhere else. */
            synchronized (scanLock) {
                if (scanGen == gen) { scanCb = null; scanGen = 0; }
            }
            return t.getClass().getSimpleName();
        }
    }

    /* THE SCAN THIS PROCESS ASKED FOR NEVER STARTED. Binder thread.
     *
     * Called from the installed ScanCallback (and only from there). Clears the
     * matching registration and hands the failure to native, which resets its
     * scan state and enters the throttled retry that already exists for a scan
     * that is down. A failure naming a superseded generation is dropped whole:
     * it must not clear a replacement's callback, and it must not tell native
     * that the scan it currently believes in has died.
     *
     * Note the qualified `Ble.onScanFailed` below: this class has an anonymous
     * ScanCallback with its own one-argument onScanFailed, and inside such a
     * class the outer static of the same NAME is shadowed regardless of
     * signature -- so routing through this method (rather than calling native
     * from the override) is what keeps the two apart. */
    static void scanFailed(int gen, int err) {
        boolean mine;
        BluetoothLeScanner s;
        ScanCallback cb;
        int installed;
        synchronized (scanLock) {
            s = scanner;
            cb = scanCb;
            installed = scanGen;
            mine = BoundaryLogic.scanFailureIsCurrent(gen, installed, cb != null);
            /* Cleared here, under the lock, so a second failure for the same
             * generation -- the stack delivers duplicates -- finds nothing
             * installed and stops. */
            if (mine) { scanCb = null; scanGen = 0; }
        }
        if (!mine) {
            /* `installed` is the value the decision was made on, not a re-read:
             * a log line that contradicts the decision it explains is worse
             * than no log line. */
            Log.i(TAG, "scan failed " + err + " for gen " + gen
                       + " (installed " + installed + "); ignored");
            return;
        }
        Log.i(TAG, "scan failed " + err + " for gen " + gen
                   + "; releasing it and telling native");
        /* BEST EFFORT, AND THE FAILURE IS NOT FATAL. The registration did not
         * start scanning, so there is nothing to keep; a platform that also
         * refuses the cancel (SecurityException after the permission is
         * revoked) is not holding a registration we could rescue by keeping the
         * handle, and keeping it would only stop the retry that can recover.
         * That is the opposite trade-off from stop() -- there the scan is
         * believed to be LIVE, so a refused cancel must not be treated as a
         * stopped scan. */
        if (s != null && cb != null) {
            final BluetoothLeScanner fs = s;
            final ScanCallback fcb = cb;
            if (!BoundaryLogic.stopScan(true, new BoundaryLogic.Attempt() {
                    @Override public void run() { fs.stopScan(fcb); }
                }))
                Log.i(TAG, "scanFailed: the platform refused the cancel too");
        }
        /* OUTSIDE scanLock: this takes locks in C (see the scanLock comment). */
        try { Ble.onScanFailed(gen, err); }
        catch (Throwable t) { Log.i(TAG, "scanFailed -> native: " + t); }
    }

    /* Returns false if the scan could NOT be confirmed stopped.
     *
     * This used to swallow the exception and null scanCb regardless, which drops
     * the only handle to a callback that is still registered with the stack --
     * stopScan throws SecurityException once BLUETOOTH_CONNECT is revoked, which
     * the app's own settings screen can do. The C side set g_scanning = 0 either
     * way, so the self-heal in on_timer then installed a SECOND callback:
     * duplicate onAdvert delivery (double-counted adverts, two reconnect
     * attempts racing the per-link throttle) and, over repeated pause/resume
     * cycles, enough registered scan clients to trip Android's "app scanning too
     * frequently" block -- the exact sticky failure that self-heal is throttled
     * to avoid. Keep the handle on failure so the next stop can retry it. */
    public static boolean stop() {
        final BluetoothLeScanner s;
        final ScanCallback cb;
        /* SNAPSHOT UNDER THE LOCK. The failure callback clears these from a
         * binder thread, so reading scanner and scanCb separately could pair a
         * live scanner with a callback that has just been released. */
        synchronized (scanLock) {
            s = scanner;
            cb = scanCb;
            if (s == null || cb == null) { scanCb = null; scanGen = 0; return true; }
        }
        boolean stopped = BoundaryLogic.stopScan(true, new BoundaryLogic.Attempt() {
            @Override public void run() { s.stopScan(cb); }
        });
        if (!stopped) {
            Log.i(TAG, "stop: platform rejected stopScan; retaining callback");
            return false; /* scanCb and its generation deliberately retained */
        }
        /* CLEARED TOGETHER. The generation must go with the callback: a
         * registration that is gone must not still be nameable, or a failure
         * the stack delivers for the scan just stopped would be accepted as
         * current and reset a scan that has since been started again. */
        synchronized (scanLock) {
            if (scanCb == cb) { scanCb = null; scanGen = 0; }
        }
        return true;
    }

    /* ---- OS-level bonding ------------------------------------------------
     *
     * THE PROBLEM THIS SOLVES. Android bonds implicitly: the stack starts
     * pairing when a GATT operation returns insufficient-authentication, which
     * is whenever the sensor next connects and the driver first touches an
     * encrypted characteristic. That is somewhere between seconds and ten
     * minutes after the user tapped anything, so the system pairing dialog
     * arrives unannounced, long after the moment it belongs to, and a user who
     * is not staring at the screen simply misses it. Missing it leaves the
     * device registered but never bonded, with nothing on screen to say so.
     *
     * Doing it EXPLICITLY at commit time makes the dialog a consequence of the
     * tap: the request goes out while the user is still looking at the screen
     * they tapped on.
     *
     * WHAT IS NOT POSSIBLE: auto-accepting. setPairingConfirmation() and
     * setPin() are guarded by BLUETOOTH_PRIVILEGED, which is
     * signature|privileged -- system-image apps only. A normal app can choose
     * WHEN the prompt appears and can SEE how it resolves; it cannot answer it.
     * Do not add an ACTION_PAIRING_REQUEST auto-confirm path; it silently does
     * nothing outside a system build. */
    private static final String BOND_CH = "pancra-bond";
    private static final int BOND_NID = 3; /* != Alarm's NID, != the service's 1 */
    private static BroadcastReceiver bondRx;

    /* Start watching bond state. Registered ONCE, at native init, not lazily at
     * createBond(): the sensor can also start pairing on its own (that is the
     * implicit path above, which still happens on a reconnect after a bond is
     * cleared), and those transitions are exactly the ones the user needs told
     * about. Registering only around our own createBond would miss them. */
    public static synchronized void bondWatch(Context ctx) {
        if (bondRx != null) return;
        try {
            final Context app = ctx.getApplicationContext();
            bondRx = new BroadcastReceiver() {
                @Override public void onReceive(Context c, Intent i) {
                    try {
                        BluetoothDevice d = i.getParcelableExtra(
                            BluetoothDevice.EXTRA_DEVICE);
                        if (d == null) return;
                        int st = i.getIntExtra(BluetoothDevice.EXTRA_BOND_STATE,
                                               BluetoothDevice.BOND_NONE);
                        String mac = d.getAddress();
                        Log.i(TAG, "bond state " + mac + " -> " + st);
                        /* Tell the user WHILE the dialog is up, not after. The
                         * app may be backgrounded or the screen off -- which is
                         * precisely when the prompt goes unanswered -- so this
                         * is a heads-up notification, not an in-app string. The
                         * in-app string is native's job (onBondState). */
                        if (st == BluetoothDevice.BOND_BONDING)
                            bondNotify(app, mac);
                        else
                            bondCancelNotify(app);
                        onBondState(mac, st);
                    } catch (Throwable t) { Log.i(TAG, "bond rx: " + t); }
                }
            };
            IntentFilter f =
                new IntentFilter(BluetoothDevice.ACTION_BOND_STATE_CHANGED);
            /* RECEIVER_NOT_EXPORTED says nothing outside the OS may forge this
             * at us, and API 34 REQUIRES a flag on any exported-capable
             * registration. But that three-argument overload only exists from
             * API 33, and minSdk here is 29 -- calling it on 29..32 throws
             * NoSuchMethodError, which the catch below would swallow into
             * "bond watching silently does nothing" on exactly the older
             * phones least likely to be tested. So: try the flagged form,
             * fall back to the plain one. */
            try {
                app.registerReceiver(bondRx, f, Context.RECEIVER_NOT_EXPORTED);
            } catch (Throwable pre33) {
                app.registerReceiver(bondRx, f);
            }
        } catch (Throwable t) {
            bondRx = null;
            Log.i(TAG, "bondWatch: " + t);
        }
    }

    /* Ask the OS to bond NOW. Returns null when the request was accepted (or
     * the device is already bonded, which is success as far as the caller is
     * concerned), otherwise a short reason that fits a status row. */
    public static String createBond(Context ctx, String mac) {
        try {
            BluetoothManager bm =
                (BluetoothManager) ctx.getSystemService(Context.BLUETOOTH_SERVICE);
            BluetoothAdapter ad = (bm == null) ? null : bm.getAdapter();
            if (ad == null) return "NO BLUETOOTH";
            BluetoothDevice dev = ad.getRemoteDevice(mac);
            int st = dev.getBondState();
            if (st == BluetoothDevice.BOND_BONDED) return null;
            /* Already prompting: a second createBond() while one is in flight
             * is refused by the stack and would report a spurious failure. */
            if (st == BluetoothDevice.BOND_BONDING) return null;
            return dev.createBond() ? null : "PAIR REFUSED";
        } catch (Throwable t) {
            /* SecurityException when BLUETOOTH_CONNECT has been revoked -- the
             * app's own permissions screen can do that. */
            return shortErr(t);
        }
    }

    private static void bondNotify(Context app, String mac) {
        try {
            NotificationManager nm =
                app.getSystemService(NotificationManager.class);
            if (nm == null) return;
            if (nm.getNotificationChannel(BOND_CH) == null) {
                /* IMPORTANCE_HIGH so it heads-up over whatever is in front.
                 * The whole point is to catch a user who is NOT in the app. */
                NotificationChannel c = new NotificationChannel(
                    BOND_CH, "Pairing", NotificationManager.IMPORTANCE_HIGH);
                c.setDescription("Android is asking you to confirm a pairing");
                nm.createNotificationChannel(c);
            }
            Intent open = new Intent(app, android.app.NativeActivity.class);
            open.setAction(Intent.ACTION_MAIN);
            open.addCategory(Intent.CATEGORY_LAUNCHER);
            open.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK
                          | Intent.FLAG_ACTIVITY_SINGLE_TOP);
            PendingIntent pi = PendingIntent.getActivity(app, 0, open,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
            Notification n = new Notification.Builder(app, BOND_CH)
                .setContentTitle("Confirm pairing")
                .setContentText("Android is asking to pair " + mac
                                + " — open the notification shade and accept")
                .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
                .setCategory(Notification.CATEGORY_STATUS)
                .setContentIntent(pi)
                .setAutoCancel(true)
                .build();
            nm.notify(BOND_NID, n);
        } catch (Throwable t) { Log.i(TAG, "bondNotify: " + t); }
    }

    private static void bondCancelNotify(Context app) {
        try {
            NotificationManager nm =
                app.getSystemService(NotificationManager.class);
            if (nm != null) nm.cancel(BOND_NID);
        } catch (Throwable t) { Log.i(TAG, "bondCancelNotify: " + t); }
    }

    /* ---- connect / GATT ---- */
    public static String connect(Context ctx, String mac, int id) {
        /* Declared OUTSIDE the try so the catch can clear L.connecting. */
        Link L = link(id);
        if (L == null) return "BAD LINK";
        try {
            BluetoothManager bm =
                (BluetoothManager) ctx.getSystemService(Context.BLUETOOTH_SERVICE);
            BluetoothAdapter ad = (bm == null) ? null : bm.getAdapter();
            if (ad == null) return "NO BLUETOOTH";
            BluetoothDevice dev = ad.getRemoteDevice(mac);
            /* Close any client still open from a previous attempt before making a
             * new one — otherwise re-issuing connect() (e.g. the stall watchdog)
             * leaks a GATT client interface, which strands the link. Clean up
             * after ourselves so a fresh connect always starts from a clean slate. */
            /* Tear the old client down under the monitor, but call connectGatt
             * OUTSIDE it: connectGatt is a binder call into the Bluetooth
             * process, and the caller may hold the C driver_lock, which the
             * op-failure path acquires from inside this monitor. Holding the
             * monitor across the call closes that into a deadlock. */
            BluetoothGatt old;
            int myGen;
            synchronized (L) {
                old = L.gatt; L.gatt = null; L.reset(); myGen = ++L.gen;
                L.connecting = true;
            }
            if (old != null) {
                try { old.disconnect(); old.close(); } catch (Throwable t) { /* ignore */ }
            }
            /* autoConnect=true: connect when the sensor next advertises rather than
             * failing immediately (status 62). Robust for periodic CGM advertising
             * and gentle on the sensor battery (passive wait, no connect storms).
             * It also lets the controller interleave several pending links, which
             * is what keeps concurrent sensors from starving each other. */
            BluetoothGatt g = dev.connectGatt(ctx.getApplicationContext(), true,
                                              callbackFor(L, myGen), BluetoothDevice.TRANSPORT_LE);
            /* connectGatt registers its callback before returning, so a
             * disconnect for THIS client may already have run and nulled the
             * field. Publish only if nothing else claimed the slot meanwhile;
             * otherwise close the loser so no GATT client is leaked. */
            boolean keep;
            synchronized (L) {
                /* Publish only if nothing invalidated this attempt meanwhile.
                 * A disconnect of the OLD client does not bump gen (it leaves
                 * L.gatt null without disturbing us), so the common
                 * reconnect-after-drop case publishes normally -- the bug the
                 * boolean flag caused. An explicit disconnect() DOES bump gen,
                 * so a teardown racing a connect wins and we close the loser. */
                keep = (L.gen == myGen && L.gatt == null);
                if (keep) L.gatt = g;
                if (L.gen == myGen) L.connecting = false;
            }
            if (!keep && g != null) {
                try { g.close(); } catch (Throwable t) { /* ignore */ }
                return null;
            }
            return g == null ? "CONNECT NULL" : null;
        } catch (Throwable t) {
            /* Clear `connecting`, or it stays true forever. connectGatt throws
             * SecurityException when BLUETOOTH_CONNECT has been revoked (the
             * app's own settings screen can do that), and `connecting` is the
             * sole input to disconnect()'s `had` -- so the next teardown would
             * fire a PHANTOM onDisconnected for a link that never opened,
             * incrementing ctx->fails and, in auth/cert, ctx->authfails. Three
             * of those call drv_key_clear() and destroy the bond. */
            synchronized (L) { L.connecting = false; }
            return t.getClass().getSimpleName();
        }
    }

    public static void disconnect(int id) {
        Link L = link(id);
        if (L == null) return;
        /* Capture and clear under the monitor: an unsynchronised check-then-use
         * races onConnectionStateChange, which closes and nulls the same field
         * -- double close(), or an NPE between the check and the use. */
        BluetoothGatt g;
        /* Bump the generation even when there is nothing to close: a connect()
         * may be in flight with L.gatt still null, and without this the
         * teardown was a silent no-op -- dexble_link_close() would "succeed"
         * while the sensor reconnected moments later, including a sensor the
         * user had just forgotten (whose key had already been destroyed). */
        boolean had;
        synchronized (L) {
            g = L.gatt;
            had = (g != null) || L.connecting;
            L.gatt = null; L.reset(); L.gen++; L.connecting = false;
        }
        try { if (g != null) { g.disconnect(); g.close(); } }
        catch (Throwable t) { Log.i(TAG, "disc: " + t); }
        /* Deliver the event ourselves: we just cleared L.gatt, so the GATT
         * callback will see a non-matching client and stay silent. Native still
         * needs it -- the driver resets its phase, and ot_on_disconnected
         * persists the meter's record index on exactly this abort path (the
         * 90 s mid-sync watchdog closes the link this way). Fires once. */
        /* Deliver unconditionally, not only when a client was published. An
         * explicit teardown racing an in-flight connect() leaves g == null, and
         * staying silent there is how the meter lost its record index: the 90 s
         * mid-sync watchdog closes the link exactly this way, and
         * ot_on_disconnected is what persists the index. gen was already bumped
         * above, so the GATT callback for that attempt will stay silent and this
         * still fires exactly once. */
        /* Deliver only if this teardown actually ended something. The GATT
         * callback already delivers for a client it owned (and bumped gen), so
         * delivering unconditionally here made the UNION of the two paths fire
         * TWICE for one physical disconnect: driver_on_disconnected then
         * double-counts ctx->fails and ctx->authfails -- and authfails >= 3
         * calls drv_key_clear(), destroying the bond. `had` is false exactly
         * when the callback has already cleared the link. */
        if (had) onDisconnected(id, 0);
    }

    /* THE CAPTURED CLIENT, not L.gatt. Looking the characteristic up on one
     * client and acting on whatever L.gatt holds a moment later is the split
     * this whole change closes. */
    private static BluetoothGattCharacteristic find(BluetoothGatt g, String uuid) {
        if (g == null) return null;
        UUID u = UUID.fromString(uuid);
        for (BluetoothGattService s : g.getServices()) {
            BluetoothGattCharacteristic c = s.getCharacteristic(u);
            if (c != null) return c;
        }
        return null;
    }

    /* Enable notifications (indicate=false) or indications (indicate=true). */
    public static void subscribe(final int id, final String uuid, final boolean indicate) {
        final Link L = link(id);
        if (L == null) return;
        L.enqueue(new Act() { public void run(BluetoothGatt g, int gen) {
            Log.i(TAG, "op subscribe [" + id + "] " + uuid + (indicate ? " IND" : " NOT"));
            BluetoothGattCharacteristic c = find(g, uuid);
            if (c == null) { Log.i(TAG, "  char not found"); onWritten(id, uuid, -1); L.done(gen); return; }
            if (!L.current(gen)) { Log.i(TAG, "  client replaced"); onWritten(id, uuid, -1); L.done(gen); return; }
            g.setCharacteristicNotification(c, true);
            BluetoothGattDescriptor d = c.getDescriptor(CCCD);
            if (d == null) { Log.i(TAG, "  no CCCD"); onWritten(id, uuid, -1); L.done(gen); return; }
            d.setValue(indicate ? BluetoothGattDescriptor.ENABLE_INDICATION_VALUE
                                : BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE);
            /* If the stack rejects the write (returns false), onDescriptorWrite
             * never fires — so, like write(), advance the queue ourselves instead
             * of stalling forever (the CCCD is often already enabled on a bonded
             * reconnect, so notifications still flow). */
            if (!g.writeDescriptor(d)) {
                Log.i(TAG, "  writeDescriptor false"); onWritten(id, uuid, -1); L.done(gen);
            }
        }});
    }

    /* Read the live connection RSSI; result arrives in onReadRemoteRssi.
     * This only reads packets already being received — nothing is sent to the
     * sensor, so it costs the sensor no battery. */
    public static void readRssi(final int id) {
        final Link L = link(id);
        if (L == null) return;
        L.enqueue(new Act() { public void run(BluetoothGatt g, int gen) {
            if (!L.current(gen) || !g.readRemoteRssi()) L.done(gen);
        }});
    }

    /* Read a characteristic (e.g. Device Information Service strings); the value
     * arrives in onCharacteristicRead and is forwarded to native via onRead. */
    public static void read(final int id, final String uuid) {
        final Link L = link(id);
        if (L == null) return;
        L.enqueue(new Act() { public void run(BluetoothGatt g, int gen) {
            BluetoothGattCharacteristic c = find(g, uuid);
            if (c == null) { Log.i(TAG, "op read " + uuid + " -> char not found"); onRead(id, uuid, new byte[0]); L.done(gen); return; }
            Log.i(TAG, "op read [" + id + "] " + uuid);
            if (!L.current(gen)) { Log.i(TAG, "  client replaced"); onRead(id, uuid, new byte[0]); L.done(gen); return; }
            if (!g.readCharacteristic(c)) { Log.i(TAG, "  readCharacteristic false"); onRead(id, uuid, new byte[0]); L.done(gen); }
        }});
    }

    /* Write a characteristic; noResponse selects WRITE_TYPE_NO_RESPONSE. */
    public static void write(final int id, final String uuid, final byte[] data, final boolean noResponse) {
        final Link L = link(id);
        if (L == null) return;
        L.enqueue(new Act() { public void run(BluetoothGatt g, int gen) {
            Log.i(TAG, "op write [" + id + "] " + uuid + " len=" + data.length + (noResponse ? " NR" : " REQ"));
            BluetoothGattCharacteristic c = find(g, uuid);
            if (c == null) { Log.i(TAG, "  char not found"); onWritten(id, uuid, -1); L.done(gen); return; }
            c.setWriteType(noResponse ? BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
                                      : BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
            c.setValue(data);
            if (!L.current(gen)) { Log.i(TAG, "  client replaced"); onWritten(id, uuid, -1); L.done(gen); return; }
            if (!g.writeCharacteristic(c)) { Log.i(TAG, "  writeCharacteristic false"); onWritten(id, uuid, -1); L.done(gen); }
        }});
    }

    /* One callback instance per link, so every event carries its link identity
     * without having to reverse-map a BluetoothGatt back to an owner. */
    /* Does this callback still belong to the link's current connect attempt?
     *
     * Comparing the BluetoothGatt object against L.gatt cannot work: connectGatt
     * registers the callback BEFORE it returns, so a disconnect can arrive while
     * the client is not yet published (L.gatt == null) and would be judged
     * "someone else's" -- after which connect() published an already-closed
     * client and the link wedged with no disconnect ever delivered. The
     * generation is captured when the attempt starts, so it identifies the
     * attempt regardless of publish timing. */
    private static boolean isLive(Link L, int gen) {
        synchronized (L) { return L.gen == gen; }
    }

    /* The link's CURRENT generation, for the setup decisions in BoundaryLogic.
     * isLive() answers the same question, but those decisions need the value
     * itself: they distinguish IGNORE from FAIL, and a boolean cannot be
     * logged as "the callback for attempt 4 arrived while 5 is live". */
    private static int liveGen(Link L) {
        synchronized (L) { return L.gen; }
    }

    /* THE SETUP DID NOT COMPLETE: end this attempt, once, under its own
     * generation.
     *
     * This used not to exist, because no setup failure was noticed at all --
     * see BoundaryLogic's gatt* comment for what the user saw. What it must do
     * is exactly what the DISCONNECTED branch does, and for the same reasons:
     *
     *   - BUMP THE GENERATION FIRST, under the monitor. The g.disconnect()
     *     below makes the stack deliver DISCONNECTED for this same callback;
     *     with the generation already bumped, that branch sees a client that
     *     is no longer the link's and stays silent, so native is told exactly
     *     ONCE. Told twice, driver_on_disconnected double-counts ctx->fails
     *     and ctx->authfails -- and authfails >= 3 calls drv_key_clear(),
     *     which destroys a bond that for a worn sensor cannot be rebuilt;
     *   - REPORT AS A DISCONNECT, because that is the driver's own recovery
     *     entry point: it resets the phase, counts the failure and re-arms
     *     drv_connect (autoConnect, so it waits for the sensor's next advert).
     *     A setup that can never complete then reaches MAX_FAILS and says
     *     CONNECTION ERROR on screen -- which is the point: the old behaviour
     *     showed nothing at all, forever;
     *   - CLOSE UNCONDITIONALLY, even when the attempt has been superseded.
     *     The paths that supersede one (disconnect(), connect()'s cleanup of
     *     `old`) do close it, so this is normally redundant, and a redundant
     *     close is what the DISCONNECTED branch above already accepts: a
     *     leaked GATT client interface strands the link for the life of the
     *     process, while a second close is a logged no-op.
     *
     * `status` is passed to native as the framework status when there is one,
     * and 0 when the failure is a refused request (there is no status for
     * "the stack declined to issue it"). Native logs it and counts the
     * failure; nothing branches on the value. */
    private static void setupFailed(final Link L, final int gen,
                                    BluetoothGatt g, String why, int status) {
        final int id = L.id;
        boolean mine;
        synchronized (L) {
            mine = (L.gen == gen);
            if (mine) {
                L.gatt = null; L.reset(); L.gen++; L.connecting = false;
            }
        }
        try { if (g != null) { g.disconnect(); g.close(); } }
        catch (Throwable t) { Log.i(TAG, "setupFailed close: " + t); }
        if (!mine) {
            Log.i(TAG, "setup [" + id + "] " + why + " for gen " + gen
                       + " (live " + liveGen(L) + "); ignored");
            return;
        }
        Log.i(TAG, "setup [" + id + "] FAILED: " + why + " (status " + status
                   + "); closing and reporting");
        onDisconnected(id, status);
    }

    /* Ask for the service table. The ONE path to native onConnected() runs
     * through the discovery this starts, so a refusal here has to end the
     * attempt: without a service table find() cannot resolve a single
     * characteristic, and there is no way to obtain one except this call. */
    private static void startDiscovery(final Link L, final int gen,
                                       BluetoothGatt g, String why) {
        Log.i(TAG, "setup [" + L.id + "] discovering services (" + why + ")");
        if (BoundaryLogic.gattDiscoverRequested(g.discoverServices())
                == BoundaryLogic.GATT_FAIL)
            setupFailed(L, gen, g, "discoverServices refused", 0);
    }

    private static BluetoothGattCallback callbackFor(final Link L, final int gen) {
        final int id = L.id;
        return new BluetoothGattCallback() {
            @Override public void onConnectionStateChange(BluetoothGatt g, int status, int newState) {
                if (newState == BluetoothProfile.STATE_CONNECTED) {
                    /* THE STATUS AND THE GENERATION, BOTH, BEFORE ANYTHING
                     * ELSE. This branch used to be one line -- requestMtu(185)
                     * -- which asked the platform for something on behalf of
                     * an attempt that may already have been replaced, ignored
                     * a status saying the connection was not to be trusted,
                     * and threw away the boolean that says whether the request
                     * was issued at all. */
                    int act = BoundaryLogic.gattConnected(gen, liveGen(L), status);
                    if (act == BoundaryLogic.GATT_IGNORE) return;
                    if (act == BoundaryLogic.GATT_FAIL) {
                        setupFailed(L, gen, g, "CONNECTED with a bad status",
                                    status);
                        return;
                    }
                    /* 185: enough for a 20-byte-chunked round, +headroom. A
                     * refusal is NOT fatal -- see gattMtuRequested: the app's
                     * messages are chunked to fit the default 23-byte MTU
                     * anyway, so the fallback is to carry on at that MTU. What
                     * is fatal is doing nothing, because no onMtuChanged is
                     * coming and the link would stop here. */
                    if (BoundaryLogic.gattMtuRequested(g.requestMtu(185))
                            == BoundaryLogic.GATT_DISCOVER)
                        startDiscovery(L, gen, g, "requestMtu refused, "
                                                  + "continuing at the default MTU");
                } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                    try { g.close(); } catch (Throwable t) { /* ignore */ }
                    /* Deliver ONLY for the client this link currently
                     * publishes.
                     *
                     * onDisconnected carries no client identity, so native
                     * cannot tell a phantom from a real one: a disconnect for
                     * an already-replaced client was being applied to the link
                     * that had just been rebuilt, resetting its phase and
                     * incrementing ctx->fails. Three of those landing during
                     * auth/cert trip the authfails>=3 branch and DELETE THE
                     * SHARED KEY -- recovery needs the applicator code and a
                     * full J-PAKE re-pair, which for a worn sensor is
                     * unrecoverable.
                     *
                     * Requiring L.gatt == g would also swallow driver-initiated
                     * teardowns (disconnect() nulls the field first), which is
                     * how the meter used to lose its record index. disconnect()
                     * therefore delivers its own event -- see there -- so the
                     * two paths together fire exactly once each. */
                    boolean mine;
                    synchronized (L) {
                        mine = (L.gen == gen);
                        if (mine) {
                            L.gatt = null; L.reset(); L.gen++; L.connecting = false;
                        }
                    }
                    /* Bumping gen also invalidates an in-flight connect() for
                     * THIS attempt, so it closes its client instead of
                     * publishing a dead one. */
                    if (mine) onDisconnected(id, status);
                }
            }
            /* Every callback below belongs to a specific client. A callback
             * already dispatched for a client that connect() has since replaced
             * would otherwise advance the NEW connection's state machine -- an
             * extra tx_left decrement or sub_idx bump desyncs the subscribe/cert
             * sequence, and L.done() would clear busy and pump a queue that
             * already has an op in flight (Android allows one per client), so
             * one op is silently dropped. Either way the link stalls, and
             * during first-time pairing there is no watchdog to recover it. */
            @Override public void onMtuChanged(BluetoothGatt g, int mtu, int status) {
                /* A FAILED NEGOTIATION IS NOT A FAILED LINK, and this is the
                 * one transition where carrying on is right: the request was
                 * issued and has completed, so the link is live and simply
                 * stayed at the default MTU -- which every message this app
                 * sends already fits, because it is chunked to 20 bytes. What
                 * this must not do is what it used to do with the RESULT of
                 * discoverServices(): ignore it. */
                if (BoundaryLogic.gattMtuChanged(gen, liveGen(L), status)
                        == BoundaryLogic.GATT_IGNORE)
                    return;
                startDiscovery(L, gen, g,
                    status == BluetoothGatt.GATT_SUCCESS
                        ? "mtu " + mtu
                        : "mtu negotiation failed (status " + status
                          + "), default MTU");
            }
            @Override public void onServicesDiscovered(BluetoothGatt g, int status) {
                /* THE ONLY DOOR TO NATIVE onConnected(), and it used to be
                 * open regardless of what discovery reported. A link handed
                 * over with no service table answers "char not found" to every
                 * subscribe and read the driver then issues, which it reads as
                 * a failing sensor: the screen blames hardware that is fine,
                 * and the state machine advances on a connection that was
                 * never set up.
                 *
                 * The service COUNT is part of the decision, not decoration: a
                 * success that discovered nothing is a link on which find()
                 * -- which iterates getServices() -- can never resolve
                 * anything. getServices() is read defensively because this
                 * runs on a client that may be closing under us. */
                int n = 0;
                try {
                    java.util.List<BluetoothGattService> svc = g.getServices();
                    n = (svc == null) ? 0 : svc.size();
                } catch (Throwable t) { n = 0; }
                int act = BoundaryLogic.gattDiscovered(gen, liveGen(L), status, n);
                if (act == BoundaryLogic.GATT_IGNORE) return;
                if (act == BoundaryLogic.GATT_FAIL) {
                    setupFailed(L, gen, g, "service discovery failed ("
                                           + n + " services)", status);
                    return;
                }
                Log.i(TAG, "setup [" + id + "] ready: " + n + " services");
                onConnected(id);
            }
            @Override public void onCharacteristicChanged(BluetoothGatt g,
                    BluetoothGattCharacteristic c) {
                /* getValue() can return null; jni_notify calls GetArrayLength on it.
                 * The read path at onRead() already guards this exact case. */
                if (!isLive(L, gen)) return;
                byte[] nv = c.getValue();
                onNotify(id, c.getUuid().toString(), nv == null ? new byte[0] : nv);
            }
            @Override public void onCharacteristicWrite(BluetoothGatt g,
                    BluetoothGattCharacteristic c, int status) {
                if (!isLive(L, gen)) return;
                onWritten(id, c.getUuid().toString(), status);
                L.done(gen);
            }
            @Override public void onDescriptorWrite(BluetoothGatt g,
                    BluetoothGattDescriptor d, int status) {
                if (!isLive(L, gen)) return;
                onWritten(id, d.getCharacteristic().getUuid().toString(), status);
                L.done(gen);
            }
            @Override public void onReadRemoteRssi(BluetoothGatt g, int rssi, int status) {
                if (!isLive(L, gen)) return;
                if (status == BluetoothGatt.GATT_SUCCESS) onRssi(id, rssi);
                L.done(gen);
            }
            @Override public void onCharacteristicRead(BluetoothGatt g,
                    BluetoothGattCharacteristic c, int status) {
                if (!isLive(L, gen)) return;
                byte[] v = (status == BluetoothGatt.GATT_SUCCESS) ? c.getValue() : null;
                Log.i(TAG, "onRead [" + id + "] " + c.getUuid() + " status=" + status + " len=" + (v == null ? -1 : v.length));
                onRead(id, c.getUuid().toString(), v == null ? new byte[0] : v);
                L.done(gen);
            }
        };
    }
}
