// SPDX-License-Identifier: GPL-3.0
package com.jk.pancra;

/* Android-free lifecycle/failure policy. Platform adapters perform the actual
 * work; this class makes the safety decisions executable on the host JVM. */
final class BoundaryLogic {
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
     * WHAT USED TO HAPPEN. Ble's callback ignored all six of those signals.
     * It called requestMtu() and threw the boolean away; onMtuChanged ignored
     * its status and called discoverServices(), throwing that boolean away
     * too; onServicesDiscovered ignored its status and called native
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
     *     screen instead of nothing at all.
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
     * silence, and it was previously guaranteed only by javacheck grepping
     * Alarm.java for the word "release" -- which a dead call, a call in the
     * wrong order, or a call skipped by an earlier throw all satisfy. */
    interface Player {
        void stop() throws Throwable;
        void release() throws Throwable;
    }

    /* STOP, THEN RELEASE. BOTH ALWAYS, EACH INDEPENDENTLY.
     *
     * With one combined try, a throw from stop() skipped release() -- and the
     * caller then dropped its only reference to the player, leaving it looping
     * with no way to reach it. So release() is attempted whatever stop() did,
     * and the caller may drop the reference either way: a player that could
     * not be released is not recoverable by holding on to it, and keeping the
     * reference would only make the NEXT alarm reuse a broken one.
     *
     * Returns true if release() itself completed. */
    static boolean stopPlayer(Player p) {
        if (p == null) return true; /* nothing playing is not a failure */
        try { p.stop(); } catch (Throwable t) { /* release anyway */ }
        try { p.release(); } catch (Throwable t) { return false; }
        return true;
    }

    static boolean[] runIndependent(Attempt... stages) {
        boolean[] ok = new boolean[stages.length];
        for (int i = 0; i < stages.length; i++) {
            try { stages[i].run(); ok[i] = true; }
            catch (Throwable t) { ok[i] = false; }
        }
        return ok;
    }

    static boolean serviceCanRun(boolean hasStartIntent, boolean nativeLoaded) {
        return hasStartIntent && nativeLoaded;
    }

    /* WHAT A RECREATED SERVICE MUST DO.
     *
     * Android restarts a START_STICKY service with a NULL intent after killing
     * the process for memory. The native side -- BLE, the driver, the reading
     * log -- is initialised by the activity, so a service recreated on its own
     * cannot monitor anything.
     *
     * Stopping quietly is the wrong answer, and it is what used to happen: the
     * foreground notification disappears with the service, so a user whose
     * phone reclaimed the app sees exactly what they would see if everything
     * were fine -- no notification is also what a healthy phone looks like
     * when the app is simply not in the shade. Their glucose is not being
     * monitored and nothing anywhere says so.
     *
     * So there are three outcomes, not two, and this is the only place that
     * decides between them. */
    static final int SVC_RUN = 0;          /* normal start: monitor */
    static final int SVC_WARN_STOPPED = 1; /* cannot monitor: SAY SO, then stop */
    static final int SVC_STOP_QUIET = 2;   /* asked to stop; nothing to warn about */

    static int serviceAction(boolean hasStartIntent, boolean nativeLoaded,
                             boolean userStopped) {
        if (userStopped) return SVC_STOP_QUIET;
        if (serviceCanRun(hasStartIntent, nativeLoaded)) return SVC_RUN;
        return SVC_WARN_STOPPED;
    }

    /* ---- WHEN THE FOREGROUND PROMOTION IS REFUSED ------------------------
     *
     * WHAT THE PHONE DID. onStartCommand built a notification, called
     * startForeground(), and wrapped it in a try that logged whatever came
     * out. Then -- unconditionally, with the failure already swallowed -- it
     * acquired the partial wakelock, started the heartbeat thread, re-armed
     * the five-minute wake alarm and returned START_STICKY. Every one of
     * those says "monitoring is protected". None of them is true of a
     * service that has not presented its notification:
     *
     *   - Android gives a service started with startForegroundService() a few
     *     seconds to call startForeground(). Miss it and the system raises
     *     ForegroundServiceDidNotStartInTimeException against the app -- the
     *     process is killed, and it is the process holding the CGM link and
     *     the alarm;
     *   - even where it is not killed outright, a service that never became a
     *     foreground service is an ordinary background one. It has no
     *     notification, so the shade shows nothing; it is first in line when
     *     the phone reclaims memory; and Doze may freeze it between the
     *     sensor's five-minute cycles.
     *
     * So the user's phone was holding a WAKELOCK -- burning battery, the one
     * visible cost of monitoring -- while the app was about to stop
     * monitoring, and the only trace was a Log.i line nobody reads. The
     * screen said nothing. That is the exact shape of the failure the
     * recreation branch below already exists to prevent, arriving through a
     * different door.
     *
     * TWO FAILURES, NOT ONE, because they want opposite things:
     *
     *   - RECOVERABLE. Android 12+ refuses an FGS start made from the
     *     background without an exemption (ForegroundServiceStartNotAllowed-
     *     Exception). The identical call succeeds once the app is in the
     *     foreground, or from an allowlisted trigger such as the wake alarm.
     *     Giving up for good here would mean a phone that never resumes
     *     monitoring after one badly-timed start, so the wake alarm stays
     *     armed and a later start tries again;
     *   - TERMINAL. A SecurityException, a missing or mismatched
     *     foregroundServiceType: the manifest is what is wrong, and it is the
     *     same manifest on the next start and every start after it. Leaving
     *     the alarm armed there buys a five-minute loop -- wake, fail,
     *     wakelock, warn, stop, repeat -- for the life of the install. The
     *     alarm is cancelled and the app waits to be opened, which is what
     *     the MONITORING STOPPED notice tells the user to do anyway.
     *
     * BY CLASS NAME, not instanceof, for the platform types. This file is
     * compiled by the host JVM WITHOUT android.jar -- that is what makes the
     * decision executable off a phone -- so android.app.ForegroundService-
     * StartNotAllowedException cannot be named as a type here. It can be
     * named as a string, and a string is exactly what the host test can
     * construct, which is the whole point: these exceptions cannot be
     * produced on demand on a real device either. */
    static final int PROMO_UNKNOWN = -1; /* a failure this list does not name */
    static final int PROMO_OK = 0;       /* presented: the service is protected */
    static final int PROMO_RETRY = 1;    /* refused now; a later start may work */
    static final int PROMO_DEAD = 2;     /* refused for good; stop retrying */

    /* The kind a throwable's class name implies, or PROMO_UNKNOWN. */
    static int promotionKind(String className) {
        if (className == null)
            return PROMO_UNKNOWN;
        /* API 31+. A TIMING refusal: the app was in the background and had no
         * exemption. The same call from the foreground succeeds. */
        if (className.equals("android.app.ForegroundServiceStartNotAllowedException"))
            return PROMO_RETRY;
        /* API 34+. The declared foregroundServiceType is missing or does not
         * match the type passed to startForeground -- a manifest fault, which
         * no amount of retrying edits. */
        if (className.equals("android.app.MissingForegroundServiceTypeException")
            || className.equals("android.app.InvalidForegroundServiceTypeException"))
            return PROMO_DEAD;
        /* A permission this build was not granted (FOREGROUND_SERVICE_-
         * CONNECTED_DEVICE among them). Same manifest, same answer, forever. */
        if (className.equals("java.lang.SecurityException"))
            return PROMO_DEAD;
        return PROMO_UNKNOWN;
    }

    /* WHAT THE PROMOTION ATTEMPT ACTUALLY ACHIEVED.
     *
     * `built` is whether build() produced a notification at all -- it is
     * total and answers null only when even the plain fallback could not be
     * constructed, and there is then nothing to present, which is not a state
     * a retry in five minutes improves (the reason build() fails all the way
     * through is a context with no NotificationManager). `failure` is what
     * startForeground threw, or null when it returned normally.
     *
     * THE CAUSE CHAIN, bounded, for the same reason netFailure walks one: the
     * framework wraps freely, and a chain built by somebody else's code can
     * in principle loop -- a hang here would be a service start that never
     * returns, which Android answers with an ANR.
     *
     * An UNRECOGNISED failure counts as RETRY. It costs a wake alarm that may
     * achieve nothing; the alternative costs a phone that has silently
     * stopped watching a CGM and will not try again. Either way the user is
     * told, because both non-OK answers publish the stopped notice. */
    static int promotionOutcome(boolean built, Throwable failure) {
        if (!built)
            return PROMO_DEAD;
        if (failure == null)
            return PROMO_OK;
        int depth = 0;
        for (Throwable c = failure; c != null && depth < 8;
             c = c.getCause(), depth++) {
            int k = promotionKind(c.getClass().getName());
            if (k != PROMO_UNKNOWN)
                return k;
        }
        return PROMO_RETRY;
    }

    /* ---- EVERYTHING onStartCommand DOES TO THE PHONE, AS SEVEN CALLS -----
     *
     * An interface rather than the Service itself, for the same reason Player
     * above is one: the ORDER and the SET of these calls is the safety
     * property, and with them written inline in PancraService no test could
     * ever observe them. The interesting case -- a promotion the platform
     * refuses -- cannot be produced on a phone on demand, so on a phone this
     * path is only ever reached by accident, on somebody's actual device,
     * with a CGM attached to it. */
    interface ServiceOps {
        void holdWakelock();
        void releaseWakelock();
        void startTicking();
        void scheduleWake();
        void cancelWake();
        void warnStopped(); /* posts NOTIF_STOPPED; see PancraService */
        void stopSelf();
    }

    /* THE WHOLE OF onStartCommand'S DECISION, and what it returns is whether
     * the service should be sticky.
     *
     * PROMOTION IS CHECKED FIRST and it outranks the start intent: a service
     * that did not present its notification is not protected whatever the
     * intent said, so there is nothing for the SVC_RUN branch to run.
     *
     * THE WAKELOCK IS RELEASED, not merely left unacquired. It is a static
     * field that outlives one onStartCommand: a start that succeeded ten
     * minutes ago took it, and this start -- which is going to stop the
     * service -- would otherwise hand the phone back to the user still
     * holding the CPU awake for a link nothing is servicing. onDestroy
     * releases it too, but onDestroy is a later trip through the looper, and
     * "eventually" is not the claim worth making about a battery.
     *
     * NOT STICKY, in every failing case. Sticky means "restart me the same
     * way" and the same way is what just failed; the wake alarm is the retry,
     * and only where a retry can help.
     *
     * EVERY STEP IN ITS OWN try, which is runIndependent's rule applied by
     * hand -- this file is compiled for the app with -source 8 against
     * android.jar, where a lambda or a method reference will not link, so
     * runIndependent itself cannot be handed these. Each of them is an
     * Android call and each can throw; the tail of the list is the half that
     * matters, because that is where the user gets told and where the
     * stopSelf that discharges the startForegroundService obligation happens.
     * A throw from an earlier step must not take them with it. */
    static boolean serviceStart(int act, int promo, ServiceOps ops) {
        if (promo != PROMO_OK) {
            try { ops.releaseWakelock(); } catch (Throwable t) { /* go on */ }
            try { ops.warnStopped(); } catch (Throwable t) { /* go on */ }
            if (promo == PROMO_DEAD) {
                try { ops.cancelWake(); } catch (Throwable t) { /* go on */ }
            } else {
                try { ops.scheduleWake(); } catch (Throwable t) { /* go on */ }
            }
            try { ops.stopSelf(); } catch (Throwable t) { /* go on */ }
            return false;
        }
        if (act != SVC_RUN) {
            if (act == SVC_WARN_STOPPED) {
                try { ops.warnStopped(); } catch (Throwable t) { /* go on */ }
            }
            try { ops.stopSelf(); } catch (Throwable t) { /* go on */ }
            return false;
        }
        try { ops.holdWakelock(); } catch (Throwable t) { /* go on */ }
        try { ops.startTicking(); } catch (Throwable t) { /* go on */ }
        try { ops.scheduleWake(); } catch (Throwable t) { /* go on */ }
        return true;
    }

    /* WHY A SYNC REQUEST DID NOT HAPPEN.
     *
     * Ble.syncHttp catches a Throwable and used to report exactly one thing:
     * -1, "it did not work". Every screen then said SYNC FAILED -- for a
     * mistyped server name, an expired certificate, a phone with no network
     * and a server that was merely slow, three of which the user can act on
     * and only one of which the app should keep retrying.
     *
     * The classification is here, and not in Ble, because here it is a pure
     * function of a Throwable: the host test constructs the real exception
     * types and checks the answer, with no phone and no network.
     *
     * The numbers mirror enum sync_net_fail in app/syncstat.h, which is where
     * they become an outcome; `make javacheck` compares the two lists.
     */
    static final int NET_OK = 0;
    static final int NET_DNS = 1;
    static final int NET_TIMEOUT = 2;
    static final int NET_TLS = 3;
    static final int NET_UNREACHABLE = 4;
    static final int NET_OTHER = 5;

    static int netFailure(Throwable t) {
        /* THE WHOLE CAUSE CHAIN. HttpURLConnection wraps freely -- an
         * UnknownHostException commonly arrives inside an IOException from
         * getOutputStream -- so classifying only the outermost throwable
         * reports OTHER for the failures that are easiest to name. */
        /* BOUNDED. A cause chain is supposed to end, but it is built by
         * whatever library threw, and a cycle here would hang the sync
         * worker -- which holds no lock and shows no progress, so it would
         * look exactly like a server that never answers. Eight links is far
         * past any real wrapping depth. */
        int depth = 0;
        for (Throwable c = t; c != null && depth < 8; c = c.getCause(), depth++) {
            /* TLS FIRST, and by its own types: every one of these is an
             * IOException too, so a test written against IOException would
             * report the certificate failures -- the ones a user can act on
             * -- as the generic kind. */
            if (c instanceof javax.net.ssl.SSLException
                || c instanceof java.security.cert.CertificateException
                || c instanceof java.security.GeneralSecurityException)
                return NET_TLS;
            if (c instanceof java.net.UnknownHostException)
                return NET_DNS;
            /* SocketTimeoutException is an InterruptedIOException, and so is
             * the timeout HttpURLConnection throws while writing a body. */
            if (c instanceof java.io.InterruptedIOException)
                return NET_TIMEOUT;
            if (c instanceof java.net.ConnectException
                || c instanceof java.net.NoRouteToHostException
                || c instanceof java.net.PortUnreachableException)
                return NET_UNREACHABLE;
        }
        return t == null ? NET_OK : NET_OTHER;
    }

    private BoundaryLogic() {}

    /* ---- THE FOREGROUND NOTIFICATION'S CONTENT, AS ONE VALUE ------------
     *
     * PancraService used to hold title, text, value, lock state, pixels, width
     * and height in seven separate volatile fields, write them one after
     * another as each reading landed, and read them one after another while
     * building the notification. Every field was individually safe; the SET
     * was not, and two of the mixtures a build could see are worse than stale:
     *
     *   - new pixels with the OLD width and height. `px.length >= w * h` is
     *     the only thing standing between this and Bitmap.createBitmap reading
     *     past the end of the array, and a new large plot paired with old small
     *     dimensions PASSES it -- drawing a stretched corner of the new plot;
     *   - the value from one reading beside the title and text of another, so
     *     the status bar shows one number and the row beneath it another.
     *
     * The rule is here, and not only in PancraService, because it is pure: an
     * object either carries a plot whose dimensions match its pixels or it
     * carries none, and a call bringing no plot keeps the previous one. That is
     * three sentences of arithmetic guarding a read past the end of an array,
     * and on the host it can be executed instead of argued. */
    static final class Notif {
        final String title, text, value;
        final boolean lock;
        final int[] px; /* null, or EXACTLY w*h pixels this object owns */
        final int w, h;

        Notif(String title, String text, String value, boolean lock,
              int[] px, int w, int h) {
            this.title = title;
            this.text = text;
            this.value = value;
            this.lock = lock;
            /* COPIED, not referenced. The pixels arrive from native as an
             * array this class does not own, so keeping the caller's would let
             * the plot change under a build that had already measured it. */
            if (px != null && w > 0 && h > 0 && (long) w * h <= px.length) {
                int[] own = new int[w * h];
                System.arraycopy(px, 0, own, 0, w * h);
                this.px = own;
                this.w = w;
                this.h = h;
            } else {
                this.px = null;
                this.w = 0;
                this.h = 0;
            }
        }

        boolean hasPlot() { return px != null; }
    }

    /* The next snapshot: everything from this reading, and the PREVIOUS plot
     * when this reading brings none. `prev` may be null (nothing published
     * yet). Never returns null. */
    static Notif nextNotif(Notif prev, String title, String text, String value,
                           boolean lock, int[] px, int w, int h) {
        Notif n = new Notif(title, text, value, lock, px, w, h);
        if (!n.hasPlot() && prev != null && prev.hasPlot())
            return new Notif(title, text, value, lock, prev.px, prev.w, prev.h);
        return n;
    }

    /* ---- EVERY NOTIFICATION ID THIS APP POSTS, IN ONE PLACE -------------
     *
     * THERE WERE TWO OWNERS OF THE NUMBER 2. Alarm.java declared
     * `NID = 2` with the comment "distinct from the service's id 1", and
     * PancraService declared `WARN_ID = 2` -- so both were right about id 1 and
     * neither knew about the other. Android keys a notification by (tag, id),
     * so those are not two notices, they are one slot used by two things:
     *
     *   - a LOW GLUCOSE ALARM posted while the monitoring-stopped warning is up
     *     REPLACES it, or is replaced by it -- and which one wins depends on
     *     which arrived last, not on which matters more;
     *   - Alarm.silence() cancels id 2 and takes the stopped-monitoring warning
     *     down with it, so the app stops telling the user it is not watching;
     *   - PancraService's cancel of id 2 silences the ALARM'S notification
     *     while the sound and vibration keep going, because those are a
     *     separate mechanism -- a ringing phone with nothing on screen to say
     *     why.
     *
     * The two comments could not have caught it: each described its own id
     * correctly. Only a list of all of them can, so this is the list, and the
     * numbers exist nowhere else.
     *
     * Here rather than in either class, because "here" is the file the host JVM
     * compiles: a clash is then a thing boundaryjavatest can assert about
     * rather than a thing two comments each half-know. */
    static final int NOTIF_SERVICE = 1; /* the foreground-service notification */
    static final int NOTIF_ALARM = 2;   /* a glucose alarm is sounding */
    static final int NOTIF_STOPPED = 3; /* the app is no longer monitoring */

    /* Every id above, so a test can check them as a set. Adding an id without
     * adding it here is the mistake this is shaped to catch: the assertion is
     * over this array, so an id missing from it is an id nothing checks. */
    static int[] notifIds()
    {
        return new int[] {NOTIF_SERVICE, NOTIF_ALARM, NOTIF_STOPPED};
    }

    /* 1 when no two ids collide. Trivial to compute and not trivial to get
     * right by inspection, which is the whole reason the numbers moved here. */
    static boolean notifIdsDistinct()
    {
        int[] ids = notifIds();
        for (int i = 0; i < ids.length; i++)
            for (int j = i + 1; j < ids.length; j++)
                if (ids[i] == ids[j])
                    return false;
        return true;
    }

    /* ---- ONE EXPORT, ONE FILE, ONE NAME ---------------------------------
     *
     * WHAT THE USER SAW. EXPORT DATA wrote files/pancra.csv -- always that
     * name, always truncating -- and shared content://com.jk.pancra.files/
     * pancra.csv. The receiving app does not read the URI when the chooser
     * closes; it reads it when it needs the bytes, which for a mail client is
     * when the message is actually sent, and that can be minutes or hours
     * later (a draft, a queued send, a phone with no network). So a SECOND
     * export truncated and rewrote the file the FIRST recipient had not opened
     * yet. What arrives at the colleague or the doctor is then empty, or the
     * first half of one export and the second half of another. Nothing warns
     * anybody: the sender saw a normal share sheet, and the file the app kept
     * on disk is perfectly correct.
     *
     * So each export is its own file, created fresh, never rewritten, and the
     * URI names THAT file. Which makes the name load-bearing in two separate
     * ways, and this is why the rule lives here where it can be executed:
     *
     *   1. UNIQUENESS, so no export can land on another's bytes. The name
     *      carries the export instant and a random tag, and the file is
     *      created with createNewFile() -- which fails rather than truncates
     *      if the name is somehow already taken, so uniqueness is ultimately
     *      the filesystem's answer and not this function's promise;
     *   2. VALIDATION, which is the security half. A provider that resolves
     *      whatever name it is handed is a path-traversal surface: the segment
     *      arrives URL-DECODED from another process, so "..", "%2F..", an
     *      absolute path or a name with a slash in it would all resolve
     *      somewhere in the app's private storage -- stelo.key and
     *      paircode.txt live in the same tree. The provider is not exported
     *      and grants are per-URI, so today a caller must already hold a grant
     *      to get in at all; that is one mistake away from mattering, and the
     *      name rule is what makes the mistake harmless.
     *
     * THE GRAMMAR, exactly:
     *
     *     pancra-DDDDDDDDDD-HHHHHHHH.csv
     *
     *   - "pancra-", then EXACTLY 10 ASCII digits (the export instant in epoch
     *     seconds, zero-padded -- the same unit the CSV rows themselves are
     *     stamped in), then "-", then EXACTLY 8 LOWERCASE hex digits, then
     *     ".csv";
     *   - length is therefore always 30, and the character set is exactly
     *     [a-z0-9] plus the two '-' and the one '.' at fixed positions. No
     *     separator, no wildcard, no dot-dot and no percent-escape can be
     *     spelled in it, whatever the sender does to the URI;
     *   - lowercase only, so one snapshot has exactly ONE spelling: a
     *     validator that accepted both cases would accept names the generator
     *     never produces, which is a larger surface for nothing. */
    static final String EXPORT_DIR = "exports"; /* under getFilesDir() */
    static final String EXPORT_PREFIX = "pancra-";
    static final String EXPORT_SUFFIX = ".csv";
    static final int EXPORT_DIGITS = 10; /* epoch seconds, zero-padded */
    static final int EXPORT_HEX = 8;     /* random tag, lowercase hex */
    static final int EXPORT_NAME_LEN = 30; /* 7 + 10 + 1 + 8 + 4 */

    /* The name for an export taken at `epochSec` with random tag `rnd`.
     *
     * BUILT DIGIT BY DIGIT, not with String.format or SimpleDateFormat. Those
     * are locale-sensitive: in a locale whose default digits are not ASCII
     * (fa-IR, ar-EG among others) they render "1755..." in Eastern Arabic
     * numerals, and the generator would then produce a name its own validator
     * refuses -- EXPORT DATA would fail, on those phones only, with nothing to
     * point at.
     *
     * Out-of-range instants are folded into the grammar rather than allowed to
     * change its length: a negative clock counts as 0, and an instant past
     * year 2286 keeps its low 10 digits. Both keep the name valid and unique
     * (the tag does the uniqueness); a longer or shorter name would be
     * rejected by the provider, which is a failed export instead of an ugly
     * timestamp. */
    static String exportName(long epochSec, long rnd) {
        long s = epochSec < 0 ? 0 : epochSec;
        /* THE FOLDING IS THE LOOP, not a modulo above it. There was an
         * `s %= 10^EXPORT_DIGITS` here and a mutation test proved no assertion
         * could ever fail on its removal: taking each of the ten digits with
         * `% 10` already discards anything above them, so an instant past year
         * 2286 keeps its low ten digits either way. A redundant statement that
         * looks like the rule is worse than the rule being visible where it
         * happens. */
        StringBuilder b = new StringBuilder(EXPORT_NAME_LEN);
        b.append(EXPORT_PREFIX);
        for (int i = EXPORT_DIGITS - 1; i >= 0; i--) {
            long div = 1;
            for (int k = 0; k < i; k++)
                div *= 10;
            b.append((char) ('0' + (int) ((s / div) % 10)));
        }
        b.append('-');
        for (int i = EXPORT_HEX - 1; i >= 0; i--)
            b.append("0123456789abcdef".charAt((int) ((rnd >>> (4 * i)) & 0xf)));
        b.append(EXPORT_SUFFIX);
        return b.toString();
    }

    /* THE ONLY NAME TEST. Both sides use it -- the generator checks its own
     * output before anything is written, and the provider checks the segment
     * it was handed -- so there is no way for the two to disagree about what a
     * snapshot is called.
     *
     * Length FIRST. Every check after it indexes fixed positions, and a
     * shorter string would throw out of an inert boolean question; a throw
     * from a ContentProvider is a share that dies with a stack trace instead
     * of a denial. */
    static boolean exportNameValid(String name) {
        if (name == null || name.length() != EXPORT_NAME_LEN)
            return false;
        if (!name.startsWith(EXPORT_PREFIX))
            return false;
        if (!name.endsWith(EXPORT_SUFFIX))
            return false;
        int p = EXPORT_PREFIX.length();
        for (int i = 0; i < EXPORT_DIGITS; i++) {
            char c = name.charAt(p + i);
            if (c < '0' || c > '9')
                return false;
        }
        if (name.charAt(p + EXPORT_DIGITS) != '-')
            return false;
        int h = p + EXPORT_DIGITS + 1;
        for (int i = 0; i < EXPORT_HEX; i++) {
            char c = name.charAt(h + i);
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
                return false;
        }
        return true;
    }

    /* ---- WHEN A SNAPSHOT MAY BE DELETED --------------------------------
     *
     * Snapshots accumulate, so something has to remove them, and removing one
     * too early is the original bug wearing a different hat: the recipient's
     * read fails, or (worse, if a name were ever reused) reads someone else's
     * export. Deleting too late costs a few kilobytes of private storage.
     * That asymmetry decides every number here.
     *
     * "LONG ENOUGH FOR DELAYED READERS", concretely: 24 hours. A share target
     * reads the URI when it sends, not when the chooser closes -- Gmail with a
     * draft, a phone that regains network overnight, a file manager that
     * copies on a schedule -- and a day covers every one of those without
     * requiring the app to guess which app got it. AND the four newest
     * snapshots are kept whatever their age, so the export the user just made
     * (and the ones from the same sitting -- sending the same file to two
     * doctors, then a corrected one) can never be the file this rule removes.
     *
     * `rankNewest` is 0 for the newest snapshot, 1 for the next, and so on.
     * `modifiedMs` of 0 is what File.lastModified() answers when it cannot
     * tell -- which reads as 1970, i.e. infinitely old, i.e. delete it -- so
     * an unknown age keeps the file.
     *
     * Retention CANNOT extend a URI grant: if Android has revoked the
     * recipient's read permission, keeping the bytes does not help it. This
     * protects the reader that still holds a grant and opens late, which is
     * the case that was silently corrupting exports. */
    static final long EXPORT_RETAIN_MS = 24L * 60 * 60 * 1000;
    static final int EXPORT_KEEP_NEWEST = 4;

    static boolean exportSnapshotExpendable(long nowMs, long modifiedMs,
                                            int rankNewest) {
        if (rankNewest < EXPORT_KEEP_NEWEST)
            return false;
        if (modifiedMs <= 0)
            return false; /* age unknown: never guess it is old */
        long age = nowMs - modifiedMs;
        /* A timestamp in the FUTURE means the clock moved, not that the file
         * is old. Redundant with the comparison below as written -- a negative
         * age is not greater than a day -- and kept because the intent has to
         * survive the next edit of that comparison. No test can fail on its
         * removal, and the test file says so rather than pretending. */
        if (age < 0)
            return false;
        return age > EXPORT_RETAIN_MS;
    }

    /* ==== ITEM 122/123: BOUNDING AND TIMING THE SYNC EXCHANGE ==========
     *
     * (Appended as its own section. Ble.syncHttp is the only caller; these
     * are here because they are the only parts of a socket exchange that can
     * be RUN on a host JVM, and a rule nobody can execute is a rule nobody
     * has checked.)
     *
     * ---- WHAT THE PERSON HOLDING THE PHONE USED TO SEE -----------------
     *
     * Ble.syncHttp read the response `while ((n = in.read(buf)) > 0)` into a
     * ByteArrayOutputStream with NO ceiling of any kind, and only afterwards
     * handed the finished array to native, which measured it (syncjni.c,
     * jni_http) and returned -1 if it did not fit. So the refusal was real,
     * and it was too late by three copies: the growing BAOS, the
     * `toByteArray()` duplicate, and the byte[] handed across JNI. A server
     * that answered a bucket fetch with a gigabyte -- hostile, misconfigured,
     * or a captive-portal middlebox serving an error page with no end -- got
     * the app to allocate until the heap ran out.
     *
     * An OutOfMemoryError there does not land in the sync code. It lands
     * wherever the next allocation happens, which on this app is as likely to
     * be the BLE callback decoding a glucose reading. The phone stops showing
     * numbers, stops alarming on a low, and the notification says nothing is
     * wrong, because nothing THINKS anything is wrong. That is the failure
     * being prevented: not a failed sync, a monitor that quietly died.
     *
     * The second half is the clock. setReadTimeout is a PER-READ idle
     * timeout: it restarts on every byte. A server dripping one byte every
     * nineteen seconds never trips it and never finishes, and syncHttp is
     * called from the single worker that also runs pairing and restore
     * (Ble.pushExec: core 0, MAXIMUM 1). One such server therefore stopped
     * ALL syncing, pairing and restoring for the life of the process, with
     * nothing on screen -- no error, no timeout, no retry -- because from the
     * app's point of view a request was simply still in progress.
     *
     * ---- WHY THE LIMIT IS NOT A NUMBER CHOSEN HERE ---------------------
     *
     * SYNC_BODY_CAP MIRRORS `SYNC_BUF_MAX` in app/sync.h, which is the
     * `outcap` every sync.c call site passes to jni_http. Two independently
     * chosen numbers would drift the moment either side was tuned, and the
     * drift is silent in the direction that matters: a Java limit ABOVE the
     * native one puts back the allocate-then-refuse behaviour this removes,
     * and a Java limit BELOW it refuses bodies the protocol considers legal,
     * which reads to the user as a server that will not sync and no reason
     * given. So the Makefile's `javacheck` greps SYNC_BUF_MAX out of
     * app/sync.h and fails the build if it disagrees with the constant
     * below -- the same mechanical cross-check that already pins NET_* to
     * enum sync_net_fail. There is one number; this is a copy the build
     * refuses to let rot.
     *
     * SYNC_BODY_MAX is CAP - 1 rather than CAP, and that off-by-one is the
     * native contract, not caution: jni_http refuses when
     * `len > outcap - 1`, because it NUL-terminates at out[n] and sync.c
     * parses the result as a C string. A body of exactly CAP-1 bytes is
     * accepted by native and must be accepted here; a body of CAP bytes is
     * refused by native and must be refused here, before it is allocated. */
    static final long SYNC_BODY_CAP = 256L * 1024;
    static final long SYNC_BODY_MAX = SYNC_BODY_CAP - 1;

    /* HOW LONG A WHOLE EXCHANGE MAY TAKE, end to end: connect, request body,
     * status line, response body, and every gap between them.
     *
     * 45 s, against a connect timeout of 8 s and a read idle timeout of 20 s.
     * It has to be comfortably larger than either or an ordinary slow mobile
     * link would be cut off mid-sync, and small enough that a wedged worker
     * frees itself while the phone is still the same phone. A restore issues
     * one request per missing bucket and each gets its own budget, so this is
     * not a ceiling on a restore -- only on any single request within it. */
    static final long SYNC_EXCHANGE_MS = 45000L;

    /* HAS THE BODY OUTGROWN WHAT WE WILL ACCEPT?
     *
     * `read` is the running total INCLUDING the bytes just read and NOT yet
     * appended to the buffer, so a true answer means "stop now, before this
     * chunk is kept" -- the excess byte is never stored. Strictly greater
     * than: `limit` bytes is the largest body native accepts, so a body of
     * exactly the limit is legal and must not be refused here. */
    static boolean bodyBudgetExceeded(long read, long limit) {
        return read > limit;
    }

    /* SHOULD WE REFUSE BEFORE READING A BYTE, on the strength of the declared
     * Content-Length?
     *
     * This is an OPTIMISATION AND NOT THE BOUND. A lying or absent
     * Content-Length must not be able to defeat anything, which is why
     * bodyBudgetExceeded above still counts every byte; this only saves the
     * app from opening a stream it already knows it will abandon.
     *
     * The header arrives as a raw string because that is what it is on the
     * wire, and because every interesting case here is a string that is not a
     * number:
     *
     *   - ABSENT (null) or empty: chunked encoding, or a server that did not
     *     say. Not refused -- the byte counter is the real bound.
     *   - NOT A PLAIN COUNT ("12, 12", "+5", "0x10", " 5 " with inner junk):
     *     not refused HERE. Deciding what a malformed length "really means"
     *     is how request smuggling gets in; we decline to guess and let the
     *     counter do its job.
     *   - LONGER THAN A long WILL HOLD ("9" * 25): REFUSED. Long.parseLong
     *     THROWS on that, and a bounds check that throws is a bounds check
     *     that did not happen -- the throw would have unwound into syncHttp's
     *     catch and been reported as a transport failure, which is the right
     *     answer for the wrong reason and stops being right the moment the
     *     parse moves. Digits that cannot fit in a long describe a body no
     *     phone will ever hold, so they are refused as a length, deliberately
     *     and by name. (A pathological "000...0100" is refused with them.
     *     Twenty-plus leading zeros is not a thing a real server sends, and
     *     refusing a sync is recoverable; the alternative is arithmetic on
     *     attacker-chosen digit strings.) */
    static boolean contentLengthRefused(String header, long limit) {
        if (header == null)
            return false;
        String s = header.trim();
        if (s.isEmpty())
            return false;
        for (int i = 0; i < s.length(); i++) {
            char ch = s.charAt(i);
            if (ch < '0' || ch > '9')
                return false; /* not a plain count: the counter decides */
        }
        if (s.length() > 19)
            return true; /* too many digits for a long, let alone a phone */
        try {
            return Long.parseLong(s) > limit;
        } catch (NumberFormatException e) {
            /* Nineteen digits still overflows above 9223372036854775807, so
             * the catch is needed even with the length guard above.
             *
             * THE TWO ARE REDUNDANT, DELIBERATELY, AND THE MUTATION DRILL
             * SAYS SO RATHER THAN PRETENDING OTHERWISE: removing the length
             * guard alone fails no assertion, because the parse then throws
             * and lands here with the same answer. Removing BOTH is caught
             * only by the NumberFormatException escaping the suite -- a
             * crash, not a named failure. The guard stays because the intent
             * ("digits that cannot describe a body are refused as a length")
             * has to survive the next edit of the parse, and because a bounds
             * check whose only correct path is an exception handler is one
             * refactor away from being a bounds check that throws. */
            return true;
        }
    }

    /* HOW MUCH OF THE BUDGET IS LEFT, in milliseconds, never negative.
     *
     * BOTH ARGUMENTS ARE System.nanoTime() READINGS AND NOTHING ELSE. This is
     * the lesson item 70 paid for: a freshness test written against
     * currentTimeMillis went NEGATIVE when the wall clock stepped backwards
     * -- an NTP correction, a timezone database update, the user setting the
     * date -- and `now - stamp <= limit` was then true for a value that meant
     * "this happened in the future", so stale state read as fresh. Here the
     * same shape would be worse: a negative elapsed makes the REMAINING
     * budget larger than the budget, so a clock that moved would hand a
     * wedged request MORE time, which is precisely the hang this exists to
     * end. nanoTime has no epoch and no user-settable value, so it cannot
     * step; but the guard below stays anyway, because the next person to
     * touch this will not have read this paragraph.
     *
     * The elapsed value is computed as a SUBTRACTION and then compared to
     * zero, rather than comparing the two readings directly. nanoTime is
     * documented to be meaningful only as a difference -- it may start
     * anywhere, including near overflow -- and the difference stays correct
     * across the wrap where `now < start` does not. A non-positive elapsed
     * reading (backwards, or the same instant twice) yields ZERO REMAINING:
     * expired, refused, retried later. Refusing a sync that could have
     * succeeded costs one retry interval. Granting time to a request that
     * cannot finish costs every sync, pairing and restore from then on.
     *
     * AND IT IS CLAMPED AT ZERO RATHER THAN GOING NEGATIVE, which matters
     * for what this value is for next. A remaining budget is the natural
     * argument to setReadTimeout/setSoTimeout, and every Java socket API
     * reads a NEGATIVE timeout as an error and a ZERO one as INFINITE -- so
     * an overrun that reported "-3000 ms left" and was passed onward would
     * arm no timeout at all. That is item 123's hang, restored by the code
     * written to fix it. deadlineExpired's `<= 0` would mask a negative
     * return today, so the clamp is pinned by its own assertion in
     * boundaryjavatest and not left to be inferred from its only caller. */
    static long deadlineRemainingMs(long startMono, long nowMono,
                                    long budgetMs) {
        long elapsedNs = nowMono - startMono;
        if (elapsedNs < 0)
            return 0; /* time did not move forwards: no budget remains */
        long rem = budgetMs - elapsedNs / 1000000L;
        return rem > 0 ? rem : 0;
    }

    /* IS THE WHOLE-REQUEST DEADLINE UP? Non-positive remaining is expired,
     * so a budget of zero refuses immediately rather than meaning "for
     * ever" -- the reading of 0 that a missing configuration would produce. */
    static boolean deadlineExpired(long startMono, long nowMono,
                                   long budgetMs) {
        return deadlineRemainingMs(startMono, nowMono, budgetMs) <= 0;
    }

    /* HOW BIG A READ TO ASK FOR, so the refusal happens on the FIRST byte
     * past the limit rather than up to a bufferful later.
     *
     * Without this the loop reads 4096 at a time and notices the overrun only
     * once the whole chunk has landed, so "abort on the first excess byte"
     * would have been "abort within four kilobytes of it". Asking for exactly
     * one byte more than the budget allows makes the overrun detectable and
     * the buffer's peak exact: the accumulated body never exceeds the limit,
     * because the read that would have pushed it over is the read that ends
     * the exchange.
     *
     * Never returns less than 1 -- a read of 0 bytes returns 0, which the
     * loop's `> 0` condition reads as end of stream, and a truncated body
     * reported as a complete one is the exact failure mode syncjni.c's
     * oversize check was written to stop. */
    static int bodyReadSize(long read, long limit, int bufLen) {
        long want = limit - read + 1;
        if (want > bufLen)
            want = bufLen;
        if (want < 1)
            want = 1;
        return (int) want;
    }

    /* ---- THE READ LOOP ITSELF, WHERE A HOST JVM CAN RUN IT -------------
     *
     * This is deliberately NOT left in Ble.syncHttp. Everything that decides
     * whether a response is refused -- the byte counter, the declared length,
     * the deadline, the point at which accumulation stops -- operates on a
     * java.io.InputStream and a clock reading, and neither of those is an
     * Android type. Leaving the loop next to HttpURLConnection would have
     * made every one of the cases below testable only on a phone, which in
     * practice means untested; here `make boundaryjavatest` drives a stream
     * that dribbles one byte at a time, a server that lies about its
     * Content-Length, and a clock that runs backwards, on the host, with
     * assertions on where the reading actually stopped.
     *
     * What stays in Ble.syncHttp is the part that genuinely needs Android:
     * opening the connection, the connect/read idle timeouts, and the
     * watchdog that closes the socket when this deadline expires. */

    /* THE CLOCK, AS AN ARGUMENT. System.nanoTime() in production, a fake in
     * the test. Named `nanos` and not `now` because the unit is the whole
     * point: milliseconds here would be the wall clock, and the wall clock is
     * what item 70 had to unlearn. */
    interface MonoClock {
        long nanos();
    }

    /* WHAT THE READ CAME TO. Four fields rather than a byte[] and a null,
     * because "it was refused" and "it was refused BEFORE anything was
     * allocated" are different claims and only the second one is the fix.
     *
     *   body        the bytes, or null if the exchange was refused
     *   fail        NET_OK, or the NET_* kind the screen should show
     *   consumed    bytes actually pulled off the stream
     *   accumulated bytes ever held in the growing buffer -- THE ABORT POINT
     *
     * `accumulated` exists for the assertion and for nothing else. Returning
     * null proves the caller was told no; it does not prove the heap was
     * spared, and the heap is the item. A test that reads this field is
     * asserting the thing the person holding the phone actually cares
     * about: that a hostile gigabyte never became a gigabyte in memory. */
    static final class BodyRead {
        final byte[] body;
        final int fail;
        final long consumed;
        final long accumulated;

        BodyRead(byte[] body, int fail, long consumed, long accumulated) {
            this.body = body;
            this.fail = fail;
            this.consumed = consumed;
            this.accumulated = accumulated;
        }
    }

    /* READ A RESPONSE BODY, BOUNDED IN SIZE AND IN TIME.
     *
     * The order of the three refusals is not arbitrary:
     *
     *   1. THE DEADLINE, FIRST AND BEFORE ANY I/O. If the budget is already
     *      gone by the time the body starts -- a connect that took nearly all
     *      of it, a request body that crawled -- there is no point opening the
     *      stream at all, and checking afterwards would let a request that was
     *      already over time spend another full read timeout getting there.
     *   2. THE DECLARED LENGTH, second, before the first read. Refusing here
     *      is what makes a well-behaved oversized reply cost nothing.
     *   3. THE BYTE COUNTER, on every chunk, which is THE ACTUAL BOUND. A
     *      server that declares 12 and sends ten megabytes is refused by this
     *      and only by this. Trusting the header alone is the obvious wrong
     *      fix and it is worse than no fix, because it looks like one.
     *
     * The deadline is re-checked EVERY iteration, not once. That is the
     * whole of item 123: setReadTimeout is per-read and restarts on every
     * byte, so a server sending one byte every nineteen seconds satisfies it
     * for ever. Here the elapsed time is measured against a fixed start, so
     * the dribble ends at the budget no matter how many bytes arrived.
     *
     * A REFUSAL RETURNS NO BYTES AT ALL, never a prefix. sync.c parses what
     * it gets; half a bucket that parses is worse than no bucket, and the
     * asymmetry rule in lib/wirevec.h says an implementation that cannot hold
     * a legal body must DECLINE rather than truncate. The partial buffer is
     * dropped on the floor. */
    static BodyRead readBoundedBody(java.io.InputStream in, String lenHeader,
                                    long limit, MonoClock clock,
                                    long startMono, long budgetMs)
        throws java.io.IOException {
        if (deadlineExpired(startMono, clock.nanos(), budgetMs))
            return new BodyRead(null, NET_TIMEOUT, 0, 0);
        if (contentLengthRefused(lenHeader, limit))
            return new BodyRead(null, NET_OTHER, 0, 0);
        if (in == null)
            return new BodyRead(new byte[0], NET_OK, 0, 0);

        java.io.ByteArrayOutputStream out =
            new java.io.ByteArrayOutputStream();
        byte[] buf = new byte[4096];
        long consumed = 0;
        for (;;) {
            /* Ask for exactly one byte more than the budget allows, so the
             * overrun is detected on the first excess byte rather than up to
             * a bufferful past it. */
            int want = bodyReadSize(consumed, limit, buf.length);
            int n = in.read(buf, 0, want);
            if (n <= 0)
                break; /* end of stream: the body is whatever arrived */
            consumed += n;
            /* COUNT BEFORE APPENDING. The chunk that crosses the line is
             * never written, so `out` never holds more than `limit` bytes and
             * the peak allocation is the limit plus one 4 kB buffer -- not
             * whatever the server felt like sending. */
            if (bodyBudgetExceeded(consumed, limit))
                return new BodyRead(null, NET_OTHER, consumed, out.size());
            /* AND CHECK THE CLOCK EVERY TIME AROUND. A body that stays under
             * the size limit for ever is still a body that never ends. */
            if (deadlineExpired(startMono, clock.nanos(), budgetMs))
                return new BodyRead(null, NET_TIMEOUT, consumed, out.size());
            out.write(buf, 0, n);
        }
        return new BodyRead(out.toByteArray(), NET_OK, consumed, out.size());
    }

    /* ---- 124: A SIGNED REQUEST GOES WHERE IT WAS SIGNED TO GO, OR NOWHERE
     *
     * Native signs ONE method and ONE target. sync.c builds the string that
     * the HMAC covers out of the verb and the path it is about to ask for,
     * and the server checks the signature against the verb and path it
     * actually received; that is the whole authentication. A redirect breaks
     * the assumption underneath it in two different ways, and both of them
     * end with this app reporting success for an operation that never
     * happened:
     *
     *   - THE TARGET MOVES. 301/302/307/308 name a new location, and
     *     HttpURLConnection follows it by default, with the same headers,
     *     to a host this app never signed anything for. The signature we
     *     computed for /v1/bucket/7 travels to wherever the redirect points.
     *     Either it is rejected there -- and the rejection is reported for
     *     the ORIGINAL request -- or, if the redirect came from something
     *     that speaks for the server, it is honoured somewhere we did not
     *     mean.
     *   - THE METHOD MOVES. A 303 turns ANY method into a GET by
     *     specification, and 301/302 have rewritten a body-carrying request
     *     into a GET in practice since Netscape. The uploads here are PUTs
     *     (sync.c: signed_req("PUT", ...)), so the request that carried a
     *     bucket of readings arrives at the new location as a fetch with no
     *     body, the fetch answers 200, and the follower reports that 200 as
     *     the outcome of the upload. sync.c's `!= 200` passes, the phone
     *     believes those rows are on the server, and DELETES ITS COPY. That
     *     is the failure mode: not a failed sync, a silently discarded one.
     *     307/308 exist precisely to preserve the method, which is what makes
     *     them the ones a rule written against 302 alone lets straight
     *     through -- and a preserved PUT sent to a destination we did not
     *     sign for is worse than a mangled one, not better.
     *
     * So: setInstanceFollowRedirects(false) in Ble.syncHttp -- `make
     * javacheck` fails the build without it -- and then EVERY 3xx is a
     * protocol failure here. Not the status, which native would publish as
     * the server's answer; a transport failure, meaning "the request did not
     * happen", which is exactly true.
     *
     * THE WHOLE RANGE, including 304. This app sends no conditional requests,
     * so a Not Modified is as much a server we do not understand as a 302 is,
     * and it carries no body to return. The rule is the range, because the
     * next status number somebody invents in it will not be in a list written
     * today. If a future policy wants to follow one, it has to hand the
     * validated target back to native and have the request RE-SIGNED for it;
     * following without re-signing is the bug. */
    static boolean redirectRefused(int status) {
        return status >= 300 && status < 400;
    }

    /* ---- 125: THE CONNECTION, AS LITTLE OF IT AS IS DECIDABLE ------------
     *
     * The shadow of the three HttpURLConnection calls that the exchange
     * actually depends on, so the ORDER of them -- and the closing of what
     * they hand back -- is a thing the host JVM can run and assert on.
     * Everything genuinely Android stays behind this interface in
     * Ble.syncHttp: https, the two idle timeouts, the redirect switch, the
     * header lines, and the watchdog that closes the socket.
     *
     * openResponse takes the status because the platform splits the body in
     * two: getInputStream() for a 2xx and getErrorStream() for everything
     * else, with the second returning null when there is no error body at
     * all. Both are the caller's problem, not this file's -- but both must be
     * CLOSED, which is this file's problem. */
    interface SyncConn {
        /* The request body sink, sized. Null means "no body to send". */
        java.io.OutputStream openRequest(int len) throws java.io.IOException;
        /* The status line. This is where the request is actually sent. */
        int status() throws java.io.IOException;
        /* A response header, or null. */
        String header(String name);
        /* The response body, or null when there is none. */
        java.io.InputStream openResponse(int status)
            throws java.io.IOException;
    }

    /* WHAT THE EXCHANGE CAME TO.
     *
     *   body  the response bytes, or null when the exchange was refused
     *   code  what Ble.sSyncCode must become -- the server's status, or -1
     *   fail  NET_OK, or the NET_* kind the screen should show
     *
     * THE INVARIANT IS `code == -1 WHENEVER body == null`, and it is the
     * reason the status travels in here rather than being read off the
     * connection by the caller. Native reads syncCode() SEPARATELY from the
     * returned array (syncjni.c, jni_http), so a null body next to a live 200
     * is read there as a request that succeeded and returned nothing: an
     * upload we never completed treated as accepted, and -- for a bucket
     * fetch -- a bucket treated as EMPTY, which is the one answer that drives
     * the loop that deletes local rows. Refusing and publishing the status
     * are therefore not two decisions; they are one, and this type is where
     * they are made together. */
    static final class Exchange {
        final byte[] body;
        final int code;
        final int fail;

        Exchange(byte[] body, int code, int fail) {
            this.body = body;
            this.code = code;
            this.fail = fail;
        }
    }

    /* RUN ONE SIGNED REQUEST: send the body, get the status, read the reply.
     *
     * ---- WHAT ITEM 125 IS ABOUT ------------------------------------------
     *
     * None of these streams used to be closed. The request body was written
     * with `getOutputStream().write(body)` and left open; the response was
     * read to EOF and left open; the finally block deliberately closed
     * nothing, because disconnect() would have thrown away the pooled socket
     * and a sync is dozens of requests through one TLS handshake. Leaving the
     * socket in the pool is right. Leaving the STREAMS open is not the same
     * thing, and it is what actually happened:
     *
     *   - A REQUEST BODY IS NOT SENT UNTIL IT IS CLOSED. close() is where the
     *     final flush happens, so it is also where "the server refused this
     *     upload" and "the link died mid-write" are reported. A close that
     *     is never called cannot report them; a close that is called and
     *     whose exception is swallowed reports them as success, which is
     *     worse. Hence try-with-resources, and hence no catch around it.
     *   - AND IT MUST BE CLOSED BEFORE THE RESPONSE IS TOUCHED. Reading the
     *     status with a request body still open is asking a server for its
     *     answer to a question we have not finished asking.
     *   - AN UNCLOSED RESPONSE POISONS THE POOL. The connection the next
     *     request in the same sync reuses is the one this request left with
     *     bytes still unread on it. Fully consumed and closed is reusable;
     *     abandoned and closed is discarded by the platform and a fresh one
     *     is dialled. Abandoned and NOT closed is neither -- it is a socket
     *     and a file descriptor held until the finalizer gets to it, on a
     *     path that runs once per bucket per sync.
     *
     * So every stream here is a try-with-resources, INCLUDING ON THE REFUSED
     * PATHS -- the 3xx above, and item 122's over-limit abort, which is the
     * one that by construction leaves the most unread. Those two are the
     * paths that were never going to be closed by "read until EOF", because
     * they are the paths that stop early on purpose.
     *
     * IT THROWS RATHER THAN CLASSIFYING. An IOException out of any of this is
     * "the request did not happen", and Ble.syncHttp's catch already turns
     * that into a status of -1 and a named NET_* kind. Catching it here would
     * mean two places deciding what a failure means. */
    static Exchange runExchange(SyncConn conn, byte[] body, MonoClock clock,
                                long startMono, long budgetMs, long limit)
        throws java.io.IOException {
        /* `length > 0` AND NOT MERELY `!= null`. Native never sends a null:
         * jni_http builds a jbyteArray out of sync.c's `""` for every GET and
         * for the empty PUT that closes a bucket, so those arrive here as a
         * byte[0]. Opening a request body for them means setDoOutput(true)
         * with nothing to send, and setDoOutput(true) makes
         * HttpURLConnection send a POST -- so every signed GET would go out
         * as a verb it was not signed with, which is item 124's failure
         * arriving by the other door. */
        if (body != null && body.length > 0) {
            /* CLOSED BY THE BRACE, and the close is inside the try, so an
             * exception from it is thrown and not swallowed. */
            try (java.io.OutputStream out = conn.openRequest(body.length)) {
                out.write(body);
            }
        }
        int status = conn.status();
        if (redirectRefused(status)) {
            /* NOT FOLLOWED, NOT REPORTED, BUT STILL CLOSED. A redirect
             * usually carries a short courtesy body, and unread-and-unclosed
             * is what the next request in this sync would inherit. Nothing
             * is read from it -- there is nothing in a redirect this app
             * would believe -- so this is a plain close rather than a
             * try-with-resources: there is no body between the open and the
             * close for a resource block to guard. The null test is the
             * platform's: getErrorStream() returns null when the response
             * carried nothing. */
            java.io.InputStream in = conn.openResponse(status);
            if (in != null)
                in.close();
            return new Exchange(null, -1, NET_OTHER);
        }
        try (java.io.InputStream in = conn.openResponse(status)) {
            BodyRead r = readBoundedBody(in, conn.header("Content-Length"),
                                         limit, clock, startMono, budgetMs);
            /* THE STATUS TRAVELS ONLY WITH THE BYTES. See Exchange. */
            if (r.body == null)
                return new Exchange(null, -1, r.fail);
            return new Exchange(r.body, status, NET_OK);
        }
    }
}
