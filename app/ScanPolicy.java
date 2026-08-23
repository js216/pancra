// SPDX-License-Identifier: GPL-3.0
package com.jk.pancra;

/* ANDROID-FREE POLICY FOR THE RADIO: starting and stopping a scan, and the
 * generation rules that say whether a late callback belongs to the scan that
 * is installed.
 *
 * Held in one class with the service lifecycle, the notification identity,
 * the export file names and the signed-HTTP budget, these are five unrelated
 * capabilities: every platform adapter would depend on all of them and a
 * reader of any one would have 1300 lines to
 * exclude. Each is its own class now, package-private, with its own tests.
 *
 * PURE: no Android type appears here, which is what lets the host JVM run
 * every case (test/app/ScanPolicyTest.java). */
final class ScanPolicy {
    interface Attempt { void run() throws Throwable; }

    static boolean stopScan(boolean active, Attempt stop) {
        if (!active) return true;
        try { stop.run(); return true; }
        catch (Throwable t) { return false; }
    }

    /* DOES THIS LATE SCAN FAILURE BELONG TO THE SCAN THAT IS INSTALLED?
     *
     * startScan() reports success as soon as the platform has taken the
     * request; the refusal -- registration failed, scanning too frequently,
     * internal error -- arrives afterwards on a Bluetooth binder thread, and
     * by then the scan it refers to may have been stopped and replaced. On
     * resume, on a DEVICES refresh, and either side of a pairing, a new
     * ScanCallback is installed with a new generation.
     *
     * Acting on a failure that names a superseded generation would cancel the
     * registration of the LIVE scan and tell native its scan is dead -- an app
     * that stops finding sensors, which is precisely the failure the whole
     * mechanism exists to end. So the failure must name the generation that is
     * installed, and something must actually be installed for it to name:
     *
     *   - `failedGen <= 0` is a failure carrying no generation at all. It must
     *     not match the "no scan installed" state (installedGen == 0) just
     *     because both are zero;
     *   - a mismatch is a superseded scan: ignore it completely, including
     *     leaving the current registration alone;
     *   - `installed == false` means a stop already released the callback, so
     *     there is nothing left to clear and native has already been told.
     *
     * Here rather than in Ble because it is pure: the host JVM can run all
     * four cases, and Ble.java's own copy of this decision is reachable only
     * from a phone whose Bluetooth stack is misbehaving. */
    static boolean scanFailureIsCurrent(int failedGen, int installedGen,
                                        boolean installed) {
        if (failedGen <= 0) return false;
        if (failedGen != installedGen) return false;
        return installed;
    }

    /* ---- GATT SETUP: ONE DECISION PER TRANSITION ------------------------
     *
     * A GATT link is not usable when the stack says CONNECTED. Three more
     * things have to happen -- an MTU negotiation, a service discovery, and
     * only then the handover to the protocol driver -- and every one of them
     * can fail in TWO different ways: the request can be refused on the spot
     * (the API returns false, so no completion callback is ever delivered),
     * or it can be accepted and then complete with a non-success status.
     *
     * WHAT IGNORING THEM COSTS. A callback that calls requestMtu() and
     * throws the boolean away; that ignores onMtuChanged's status and calls
     * discoverServices(), throwing that boolean away too; that ignores
     * onServicesDiscovered's status and calls native
     * onConnected() regardless. So:
     *
     *   - a refused requestMtu/discoverServices meant no callback would ever
     *     arrive, and the link simply stopped here. The driver sits in P_CONN
     *     with a connected sensor and no traffic. Nothing recovers it during
     *     FIRST-TIME PAIRING, because the stall watchdog
     *     (pancra_link_watchdog) skips links that are not `paired` yet -- so
     *     the user watches PAIRING on screen until they relaunch the app;
     *   - a FAILED discovery still declared the link ready. The service table
     *     is then absent, so every subscribe and every read that follows
     *     answers "char not found" -> onWritten(..., -1), which the driver
     *     reads as protocol failure on a sensor that is physically fine. The
     *     screen blames the sensor, and the state machine advances on a
     *     connection that was never set up.
     *
     * MTU FAILURE IS NOT DISCOVERY FAILURE, and that asymmetry is the whole
     * content of these five functions:
     *
     *   - the 185-byte MTU is an OPTIMISATION. Every message this app sends
     *     is already chunked to 20 bytes (the default ATT MTU of 23 minus the
     *     3-byte header) because that is what an un-negotiated link carries,
     *     so a link at the default MTU works -- it just needs more chunks per
     *     J-PAKE round. Both MTU failures therefore continue to discovery:
     *     a `requestMtu` that returns FALSE was never issued (no callback is
     *     coming, so this side must move on itself), and an accepted request
     *     that completes with a bad status leaves the link at the default MTU
     *     and does call back, so it can move on from there;
     *   - discovery has NO valid fallback. There is no public way to obtain a
     *     service table without it -- getServices() is populated by discovery
     *     and returns empty until it succeeds -- so continuing means handing
     *     the driver a link on which every characteristic lookup fails. Both
     *     discovery failures close the link and report it, which puts the
     *     driver's own reconnect (driver_on_disconnected -> drv_connect) in
     *     charge of retrying, and after MAX_FAILS says CONNECTION ERROR on
     *     screen rather than nothing at all.
     *
     * An EMPTY service table with a SUCCESS status is treated as a discovery
     * failure for the same reason: find() iterates getServices(), so zero
     * services is a link on which nothing can be found, and it is better
     * closed and retried than declared ready.
     *
     * Here rather than in Ble because it is pure. The interesting cases --
     * a stack that refuses requestMtu, a discovery that returns status 133,
     * a callback for a connection that has already been replaced -- cannot be
     * produced on a phone on demand, so on a phone this code is only ever
     * reached by accident. */
    static final int GATT_IGNORE = 0;   /* superseded generation: do nothing */
    static final int GATT_WAIT = 1;     /* request issued; its callback follows */
    static final int GATT_ASK_MTU = 2;  /* ask for the bigger MTU */
    static final int GATT_DISCOVER = 3; /* discover services now */
    static final int GATT_READY = 4;    /* set up: hand it to native */
    static final int GATT_FAIL = 5;     /* close, and report a failure */

    /* BluetoothGatt.GATT_SUCCESS, which this file may not import (the host JVM
     * compiles it without android.jar). Ble passes the framework's status
     * through unchanged; a framework that renumbered success would be a
     * framework that renumbered every status code in every app. */
    static final int GATT_STATUS_OK = 0;

    /* `cbGen` is the generation the callback was created for, `liveGen` the
     * link's current one -- the mechanism Link already uses everywhere else.
     * A callback for a replaced connection must do NOTHING: reporting its
     * failure would tear down the connection that replaced it, and the
     * teardown that replaced it has already reported one. */
    private static boolean gattMine(int cbGen, int liveGen) {
        return cbGen == liveGen;
    }

    /* The stack says CONNECTED. */
    static int gattConnected(int cbGen, int liveGen, int status) {
        if (!gattMine(cbGen, liveGen))
            return GATT_IGNORE;
        /* A non-success status alongside STATE_CONNECTED is a connection the
         * stack is telling us not to trust. Closing it costs one reconnect --
         * autoConnect waits for the sensor's next advert, which a CGM sends
         * every few minutes anyway -- while trusting it costs the whole
         * setup sequence, and during first pairing there is no watchdog to
         * end that. */
        if (status != GATT_STATUS_OK)
            return GATT_FAIL;
        return GATT_ASK_MTU;
    }

    /* What requestMtu() RETURNED. False means the stack never issued the
     * request, so onMtuChanged will never arrive: this side has to continue
     * by itself, and the default MTU is a link that works. */
    static int gattMtuRequested(boolean issued) {
        return issued ? GATT_WAIT : GATT_DISCOVER;
    }

    /* The MTU negotiation completed -- well or badly. Deliberately the same
     * answer either way: the status says only whether the link got the bigger
     * MTU, and this app does not need it. */
    static int gattMtuChanged(int cbGen, int liveGen, int status) {
        if (!gattMine(cbGen, liveGen))
            return GATT_IGNORE;
        return GATT_DISCOVER;
    }

    /* What discoverServices() RETURNED. False means no discovery is running
     * and no callback is coming; with no service table there is nothing to
     * fall back to. */
    static int gattDiscoverRequested(boolean issued) {
        return issued ? GATT_WAIT : GATT_FAIL;
    }

    /* Discovery completed. This is the ONLY gate in front of native
     * onConnected(). `nServices` is what the client's own service table
     * holds: a success that discovered nothing is not a set-up link. */
    static int gattDiscovered(int cbGen, int liveGen, int status,
                              int nServices) {
        if (!gattMine(cbGen, liveGen))
            return GATT_IGNORE;
        if (status != GATT_STATUS_OK)
            return GATT_FAIL;
        if (nServices <= 0)
            return GATT_FAIL;
        return GATT_READY;
    }

    /* THE MEDIA PLAYER, as the alarm's silence path uses it.
     *
     * An interface rather than the real MediaPlayer so the policy below is
     * executable on a host JVM. The policy is the whole point: it is what
     * stands between a user and a LOOPING alarm-usage player that nothing can
     * silence. Guaranteed only by javacheck grepping Alarm.java for the word
     * "release", it is satisfied by a dead call, a call in the wrong order,
     * or a call skipped by an earlier throw. */
}
