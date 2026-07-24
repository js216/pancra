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
import android.content.Context;
import android.content.Intent;
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
     * readings.csv (the glucose log), a blank line, then insulin.csv (the dose
     * log) -- and hand it to another app via the system share sheet. Each
     * section carries its own '#' header row, so the three stay self-describing
     * and a reader can tell them apart. The content:// URI comes from
     * PancraFiles (see the manifest); FLAG_GRANT_READ_URI_PERMISSION lets the
     * chosen app read it. */
    public static void exportData(Context ctx) {
        try {
            java.io.File dir = ctx.getFilesDir();
            java.io.File out = new java.io.File(dir, "pancra.csv");
            java.io.FileOutputStream os = new java.io.FileOutputStream(out);
            copyInto(os, new java.io.File(dir, "sensors.csv"));
            os.write('\n'); /* blank line between sections */
            copyInto(os, new java.io.File(dir, "readings.csv"));
            os.write('\n');
            copyInto(os, new java.io.File(dir, "insulin.csv"));
            os.close();
            if (out.length() == 0) return;
            Uri uri = Uri.parse("content://com.jk.pancra.files/pancra.csv");
            Intent send = new Intent(Intent.ACTION_SEND);
            send.putExtra(Intent.EXTRA_STREAM, uri);
            send.setType("text/csv");
            send.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            Intent chooser = Intent.createChooser(send, "Export pancra data");
            chooser.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            ctx.startActivity(chooser);
        } catch (Throwable t) { Log.i(TAG, "export: " + t); }
    }

    private static void copyInto(java.io.OutputStream os, java.io.File f)
            throws java.io.IOException {
        if (!f.exists()) return;
        java.io.FileInputStream is = new java.io.FileInputStream(f);
        byte[] buf = new byte[4096];
        int n;
        while ((n = is.read(buf)) > 0) os.write(buf, 0, n);
        is.close();
    }

    /* ---- REMOTE push: one datapoint per call to the configured server ---- */

    /* One background thread with a small BOUNDED queue. Pushes must never block
     * the BLE binder thread that decoded the reading (it holds the driver lock),
     * and an unreachable server must not accumulate work without bound -- when
     * the queue fills, the OLDEST pending push is dropped: the server's page
     * shows recent data, so the newest points are the ones worth keeping. */
    private static final java.util.concurrent.ThreadPoolExecutor pushExec =
        new java.util.concurrent.ThreadPoolExecutor(0, 1, 30,
            java.util.concurrent.TimeUnit.SECONDS,
            new java.util.concurrent.ArrayBlockingQueue<Runnable>(64),
            new java.util.concurrent.ThreadPoolExecutor.DiscardOldestPolicy());

    /* Called from native on every newly stored datapoint (CGM, backfill, and
     * meter fingersticks alike). Enqueue-and-return; the network happens on
     * pushExec's thread. The body is one line of plain text --
     * "<epoch seconds> <mg/dL>\n" -- POSTed to http://ip:port/ (glucoserve.py
     * appends it to data.txt). Failures are logged and dropped: the phone's
     * readings.csv stays the record of truth, the remote page is a mirror. */
    public static void remotePush(final String ip, final int port,
                                  final long t, final int glu) {
        try {
            pushExec.execute(new Runnable() { @Override public void run() {
                java.net.HttpURLConnection c = null;
                try {
                    c = (java.net.HttpURLConnection)
                        new java.net.URL("http", ip, port, "/").openConnection();
                    c.setConnectTimeout(5000);
                    c.setReadTimeout(5000);
                    c.setRequestMethod("POST");
                    c.setDoOutput(true);
                    byte[] body = (t + " " + glu + "\n").getBytes("UTF-8");
                    c.setFixedLengthStreamingMode(body.length);
                    java.io.OutputStream os = c.getOutputStream();
                    os.write(body);
                    os.close();
                    int code = c.getResponseCode();
                    if (code / 100 != 2)
                        Log.i(TAG, "remote push: HTTP " + code);
                } catch (Throwable e) {
                    Log.i(TAG, "remote push: " + e);
                } finally {
                    if (c != null) c.disconnect();
                }
            }});
        } catch (Throwable e) { Log.i(TAG, "remote push enqueue: " + e); }
    }

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
        PancraService.start(ctx);
        PancraService.requestNoBatteryOpt(ctx);
    }

    /* Push the live glucose + a 3H plot bitmap into the ongoing notification
     * (shown on the lock screen / shade). Called from native each reading. */
    public static void showGlucose(Context ctx, String title, String text,
                                   int[] px, int w, int h) {
        PancraService.showGlucose(ctx, title, text, px, w, h);
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

    private static BluetoothLeScanner scanner;
    private static ScanCallback scanCb;

    /* One independent link per sensor.
     *
     * GATT operations must be serialised, but only WITHIN a link -- the stack
     * happily runs several connections at once. A single shared queue would
     * serialise them against each other, so a slow or stalled sensor would hold
     * up every other one and make it miss its advertising window. Each Link
     * therefore owns its own gatt, queue and busy flag, and its own callback
     * instance so every event already knows which link it belongs to. */
    static final int MAX_LINKS = 5;

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
        final ArrayDeque<Runnable> ops = new ArrayDeque<>();
        boolean busy;
        Link(int id) { this.id = id; }

        void enqueue(Runnable r) {
            synchronized (this) { ops.add(r); }
            pump();
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
            Runnable r;
            synchronized (this) {
                if (busy || ops.isEmpty() || gatt == null) return;
                busy = true;
                r = ops.poll();
            }
            try { r.run(); } catch (Throwable t) { Log.i(TAG, "op: " + t); done(); }
        }
        void done() {
            synchronized (this) { busy = false; }
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

    /* ---- scanning (unchanged pipe) ---- */
    public static String scan(Context ctx) {
        try {
            BluetoothManager bm =
                (BluetoothManager) ctx.getSystemService(Context.BLUETOOTH_SERVICE);
            BluetoothAdapter ad = (bm == null) ? null : bm.getAdapter();
            if (ad == null)      return "NO BLUETOOTH";
            if (!ad.isEnabled()) return "BLUETOOTH OFF";
            scanner = ad.getBluetoothLeScanner();
            if (scanner == null) return "NO LE SCANNER";
            scanCb = new ScanCallback() {
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
                @Override public void onScanFailed(int err) { Log.i(TAG, "scan failed: " + err); }
            };
            scanner.startScan(null,
                new ScanSettings.Builder()
                    .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build(), scanCb);
            return null;
        } catch (Throwable t) { return t.getClass().getSimpleName(); }
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
        if (scanner == null || scanCb == null) { scanCb = null; return true; }
        try {
            scanner.stopScan(scanCb);
        } catch (Throwable t) {
            Log.i(TAG, "stop: " + t);
            return false;   /* scanCb deliberately retained */
        }
        scanCb = null;
        return true;
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

    private static BluetoothGattCharacteristic find(Link L, String uuid) {
        if (L.gatt == null) return null;
        UUID u = UUID.fromString(uuid);
        for (BluetoothGattService s : L.gatt.getServices()) {
            BluetoothGattCharacteristic c = s.getCharacteristic(u);
            if (c != null) return c;
        }
        return null;
    }

    /* Enable notifications (indicate=false) or indications (indicate=true). */
    public static void subscribe(final int id, final String uuid, final boolean indicate) {
        final Link L = link(id);
        if (L == null) return;
        L.enqueue(new Runnable() { public void run() {
            Log.i(TAG, "op subscribe [" + id + "] " + uuid + (indicate ? " IND" : " NOT"));
            BluetoothGattCharacteristic c = find(L, uuid);
            if (c == null) { Log.i(TAG, "  char not found"); onWritten(id, uuid, -1); L.done(); return; }
            L.gatt.setCharacteristicNotification(c, true);
            BluetoothGattDescriptor d = c.getDescriptor(CCCD);
            if (d == null) { Log.i(TAG, "  no CCCD"); onWritten(id, uuid, -1); L.done(); return; }
            d.setValue(indicate ? BluetoothGattDescriptor.ENABLE_INDICATION_VALUE
                                : BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE);
            /* If the stack rejects the write (returns false), onDescriptorWrite
             * never fires — so, like write(), advance the queue ourselves instead
             * of stalling forever (the CCCD is often already enabled on a bonded
             * reconnect, so notifications still flow). */
            if (!L.gatt.writeDescriptor(d)) {
                Log.i(TAG, "  writeDescriptor false"); onWritten(id, uuid, -1); L.done();
            }
        }});
    }

    /* Read the live connection RSSI; result arrives in onReadRemoteRssi.
     * This only reads packets already being received — nothing is sent to the
     * sensor, so it costs the sensor no battery. */
    public static void readRssi(final int id) {
        final Link L = link(id);
        if (L == null) return;
        L.enqueue(new Runnable() { public void run() {
            if (L.gatt == null || !L.gatt.readRemoteRssi()) L.done();
        }});
    }

    /* Read a characteristic (e.g. Device Information Service strings); the value
     * arrives in onCharacteristicRead and is forwarded to native via onRead. */
    public static void read(final int id, final String uuid) {
        final Link L = link(id);
        if (L == null) return;
        L.enqueue(new Runnable() { public void run() {
            BluetoothGattCharacteristic c = find(L, uuid);
            if (c == null) { Log.i(TAG, "op read " + uuid + " -> char not found"); onRead(id, uuid, new byte[0]); L.done(); return; }
            Log.i(TAG, "op read [" + id + "] " + uuid);
            if (!L.gatt.readCharacteristic(c)) { Log.i(TAG, "  readCharacteristic false"); onRead(id, uuid, new byte[0]); L.done(); }
        }});
    }

    /* Write a characteristic; noResponse selects WRITE_TYPE_NO_RESPONSE. */
    public static void write(final int id, final String uuid, final byte[] data, final boolean noResponse) {
        final Link L = link(id);
        if (L == null) return;
        L.enqueue(new Runnable() { public void run() {
            Log.i(TAG, "op write [" + id + "] " + uuid + " len=" + data.length + (noResponse ? " NR" : " REQ"));
            BluetoothGattCharacteristic c = find(L, uuid);
            if (c == null) { Log.i(TAG, "  char not found"); onWritten(id, uuid, -1); L.done(); return; }
            c.setWriteType(noResponse ? BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
                                      : BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
            c.setValue(data);
            if (!L.gatt.writeCharacteristic(c)) { Log.i(TAG, "  writeCharacteristic false"); onWritten(id, uuid, -1); L.done(); }
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

    private static BluetoothGattCallback callbackFor(final Link L, final int gen) {
        final int id = L.id;
        return new BluetoothGattCallback() {
            @Override public void onConnectionStateChange(BluetoothGatt g, int status, int newState) {
                if (newState == BluetoothProfile.STATE_CONNECTED) {
                    g.requestMtu(185);       /* enough for a 20-byte-chunked round, +headroom */
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
                if (!isLive(L, gen)) return;
                g.discoverServices();
            }
            @Override public void onServicesDiscovered(BluetoothGatt g, int status) {
                if (!isLive(L, gen)) return;
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
                L.done();
            }
            @Override public void onDescriptorWrite(BluetoothGatt g,
                    BluetoothGattDescriptor d, int status) {
                if (!isLive(L, gen)) return;
                onWritten(id, d.getCharacteristic().getUuid().toString(), status);
                L.done();
            }
            @Override public void onReadRemoteRssi(BluetoothGatt g, int rssi, int status) {
                if (!isLive(L, gen)) return;
                if (status == BluetoothGatt.GATT_SUCCESS) onRssi(id, rssi);
                L.done();
            }
            @Override public void onCharacteristicRead(BluetoothGatt g,
                    BluetoothGattCharacteristic c, int status) {
                if (!isLive(L, gen)) return;
                byte[] v = (status == BluetoothGatt.GATT_SUCCESS) ? c.getValue() : null;
                Log.i(TAG, "onRead [" + id + "] " + c.getUuid() + " status=" + status + " len=" + (v == null ? -1 : v.length));
                onRead(id, c.getUuid().toString(), v == null ? new byte[0] : v);
                L.done();
            }
        };
    }
}
