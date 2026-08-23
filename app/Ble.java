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

    /* THE SYNC CLIENT'S ENTRY POINTS, declared here because RegisterNatives
     * binds natives to a CLASS (app/dexble.c) and this is that class. Nothing
     * about them is BLE: they belong to PancraNet, which is the only caller
     * and where every other part of the transport lives. They BLOCK for
     * several round trips -- syncRestore for one per missing bucket -- so
     * PancraNet only ever calls them on its own worker. */
    static native int syncRun();
    static native int syncPair(String email, String code);
    static native int syncRestore();

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
     * They are not main-thread-only fields: onScanFailed is delivered on a
     * binder thread and has to decide whether the callback it
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
         * A boolean "pending" flag cannot distinguish WHICH client a
         * DISCONNECTED callback belongs to once L.gatt is nulled, so any
         * disconnect in the window clears it and connect() then throws away
         * the live client it has just created. A generation does distinguish:
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
                 * An op body that reads L.gatt to find its characteristic
                 * and then reads L.gatt AGAIN to act on it, holding no lock
                 * across the two, has a window: disconnect() and the
                 * DISCONNECTED callback both run in it and both null the
                 * field -- so the second
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
     * Re-locks onto a sensor after an update that cleared the saved MAC but
     * kept the key, so reconnect never has to guess from adverts.
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
     * rather than being applied to whichever scan is live by then. */
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
                /* WHY THIS IS MORE THAN A LOG LINE.
                 *
                 * Native has already latched "a scan is running" by the time
                 * this runs -- and every recovery path it has starts only when
                 * that flag is clear -- so a scan the platform refuses here,
                 * merely logged, leaves the app never scanning again for the
                 * life of the process, with nothing on screen to say so. No sensor reconnect after a dropout, no
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
            mine = ScanPolicy.scanFailureIsCurrent(gen, installed, cb != null);
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
            if (!ScanPolicy.stopScan(true, new ScanPolicy.Attempt() {
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
     * Swallowing the exception and nulling scanCb regardless drops the only
     * handle to a callback that is still registered with the stack --
     * stopScan throws SecurityException once BLUETOOTH_CONNECT is revoked, which
     * the app's own settings screen can do. The C side sets g_scanning = 0 either
     * way, so the self-heal in on_timer then installs a SECOND callback:
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
        boolean stopped = ScanPolicy.stopScan(true, new ScanPolicy.Attempt() {
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
            return ExportPolicy.shortErr(t);
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
            /* Tear the previous client down under the monitor, but call
             * connectGatt
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
                 * A disconnect of the superseded client does not bump gen
                 * (it leaves L.gatt null without disturbing us), so the common
                 * reconnect-after-drop case publishes normally -- which a
                 * boolean flag cannot express. An explicit disconnect() DOES
                 * bump gen,
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

    /* Enable notifications (indicate=false) or indications (indicate=true).
     *
     * A LINK THAT IS NOT THERE IS A FAILED COMPLETION, not silence.
     * Returning quietly leaves native waiting for an answer to a request that
     * was never enqueued -- and the meter protocol, which has no other way to
     * learn that, then holds the link open until the meter powers itself off.
     * Every other refusal on this path already reports onWritten(..., -1); so
     * does this one. */
    public static void subscribe(final int id, final String uuid, final boolean indicate) {
        final Link L = link(id);
        if (L == null) { Log.i(TAG, "op subscribe [" + id + "] no such link"); onWritten(id, uuid, -1); return; }
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

    /* Write a characteristic; noResponse selects WRITE_TYPE_NO_RESPONSE.
     *
     * A LINK THAT IS NOT THERE IS A FAILED COMPLETION -- see subscribe(). */
    public static void write(final int id, final String uuid, final byte[] data, final boolean noResponse) {
        final Link L = link(id);
        if (L == null) { Log.i(TAG, "op write [" + id + "] no such link"); onWritten(id, uuid, -1); return; }
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

    /* The link's CURRENT generation, for the setup decisions in ScanPolicy.
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
     * see ScanPolicy's gatt* comment for what the user saw. What it must do
     * is exactly what the DISCONNECTED branch does, and for the same reasons:
     *
     *   - BUMP THE GENERATION FIRST, under the monitor. The g.disconnect()
     *     below makes the stack deliver DISCONNECTED for this same callback;
     *     with the generation already bumped, that branch sees a client that
     *     is not the link's any more and stays silent, so native is told
     *     exactly
     *     ONCE. Told twice, driver_on_disconnected double-counts ctx->fails
     *     and ctx->authfails -- and authfails >= 3 calls drv_key_clear(),
     *     which destroys a bond that for a worn sensor cannot be rebuilt;
     *   - REPORT AS A DISCONNECT, because that is the driver's own recovery
     *     entry point: it resets the phase, counts the failure and re-arms
     *     drv_connect (autoConnect, so it waits for the sensor's next advert).
     *     A setup that can never complete then reaches MAX_FAILS and says
     *     CONNECTION ERROR on screen -- which is the point: the alternative
     *     is a link that shows nothing at all, forever;
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
        if (ScanPolicy.gattDiscoverRequested(g.discoverServices())
                == ScanPolicy.GATT_FAIL)
            setupFailed(L, gen, g, "discoverServices refused", 0);
    }

    private static BluetoothGattCallback callbackFor(final Link L, final int gen) {
        final int id = L.id;
        return new BluetoothGattCallback() {
            @Override public void onConnectionStateChange(BluetoothGatt g, int status, int newState) {
                if (newState == BluetoothProfile.STATE_CONNECTED) {
                    /* THE STATUS AND THE GENERATION, BOTH, BEFORE ANYTHING
                     * ELSE. As one line -- requestMtu(185) -- this branch asks
                     * the platform for something on behalf of an attempt that
                     * may already have been replaced, ignores a status saying
                     * the connection is not to be trusted, and throws away the
                     * boolean that says whether the request was issued at
                     * all. */
                    int act = ScanPolicy.gattConnected(gen, liveGen(L), status);
                    if (act == ScanPolicy.GATT_IGNORE) return;
                    if (act == ScanPolicy.GATT_FAIL) {
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
                    if (ScanPolicy.gattMtuRequested(g.requestMtu(185))
                            == ScanPolicy.GATT_DISCOVER)
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
                     * how the meter loses its record index. disconnect()
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
                     * THIS attempt, so it closes its client rather than
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
                 * this must not do is ignore the RESULT of
                 * discoverServices(). */
                if (ScanPolicy.gattMtuChanged(gen, liveGen(L), status)
                        == ScanPolicy.GATT_IGNORE)
                    return;
                startDiscovery(L, gen, g,
                    status == BluetoothGatt.GATT_SUCCESS
                        ? "mtu " + mtu
                        : "mtu negotiation failed (status " + status
                          + "), default MTU");
            }
            @Override public void onServicesDiscovered(BluetoothGatt g, int status) {
                /* THE ONLY DOOR TO NATIVE onConnected(), and it is NOT open
                 * regardless of what discovery reported. A link handed
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
                int act = ScanPolicy.gattDiscovered(gen, liveGen(L), status, n);
                if (act == ScanPolicy.GATT_IGNORE) return;
                if (act == ScanPolicy.GATT_FAIL) {
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
