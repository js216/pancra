// SPDX-License-Identifier: GPL-3.0
package com.jk.pancra;

public final class BoundaryLogicTest {
    /* HOW MANY CHECKS ACTUALLY RAN. A suite whose assertions stopped being
     * reached passes exactly as loudly as one that checked everything, which
     * is the failure mode a mutation drill hits first: delete a rule, watch
     * the run go green, and only then notice the case that pinned it was
     * never entered. The count is printed and asserted non-zero. */
    private static int checks;

    private static void ck(boolean yes, String what) {
        checks++;
        if (!yes) throw new AssertionError(what);
    }

    /* ---- EVERY EFFECT onStartCommand HAS, RECORDED --------------------
     *
     * One letter per call, appended in order, so a whole service start is a
     * string an assertion can be written against -- and `wakelock` is not a
     * record of calls but the phone's ACTUAL state, because "did it release
     * the lock" and "did it call release" are different questions and only
     * the first one is about a battery. */
    private static final class Ops implements BoundaryLogic.ServiceOps {
        final StringBuilder log = new StringBuilder();
        boolean wakelock;
        int warns;
        int stops;
        boolean throwOnRelease;

        @Override public void holdWakelock() { log.append('H'); wakelock = true; }
        @Override public void releaseWakelock() {
            log.append('R');
            if (throwOnRelease) throw new IllegalStateException("not held");
            wakelock = false;
        }
        @Override public void startTicking() { log.append('T'); }
        @Override public void scheduleWake() { log.append('S'); }
        @Override public void cancelWake() { log.append('C'); }
        @Override public void warnStopped() { log.append('W'); warns++; }
        @Override public void stopSelf() { log.append('X'); stops++; }

        boolean did(char c) { return log.indexOf(String.valueOf(c)) >= 0; }
        int at(char c) { return log.indexOf(String.valueOf(c)); }
    }

    public static void main(String[] args) {
        final int[] calls = {0};
        ck(BoundaryLogic.stopScan(false, () -> calls[0]++),
           "an inactive scan is already stopped");
        ck(calls[0] == 0, "inactive stop does not touch Android");
        ck(!BoundaryLogic.stopScan(true, () -> { calls[0]++; throw new Exception(); }),
           "a failed platform stop remains retryable");
        ck(calls[0] == 1, "active stop reaches Android once");
        ck(BoundaryLogic.stopScan(true, () -> calls[0]++), "a successful stop completes");

        final StringBuilder order = new StringBuilder();
        boolean[] stages = BoundaryLogic.runIndependent(
            () -> { order.append('s'); throw new Exception(); },
            () -> order.append('v'), () -> order.append('n'));
        ck(order.toString().equals("svn"), "alarm stages retain safety order");
        ck(!stages[0] && stages[1] && stages[2],
           "one alarm output failure cannot suppress the others");

        ck(!BoundaryLogic.serviceCanRun(false, true), "null restart intent refuses zombie service");
        ck(!BoundaryLogic.serviceCanRun(true, false), "native-less service refuses fake monitoring");
        ck(BoundaryLogic.serviceCanRun(true, true), "normal native service runs");

        /* THE RECREATION CASE, which is the one that decides whether a user
         * whose phone killed the app finds out. Android hands a null intent
         * after a memory kill; the service cannot monitor without the native
         * side, and vanishing quietly looks exactly like a healthy phone. */
        ck(BoundaryLogic.serviceAction(false, true, false)
               == BoundaryLogic.SVC_WARN_STOPPED,
           "a null-intent recreation WARNS instead of disappearing");
        ck(BoundaryLogic.serviceAction(true, false, false)
               == BoundaryLogic.SVC_WARN_STOPPED,
           "a start without the native side warns too");
        ck(BoundaryLogic.serviceAction(true, true, false)
               == BoundaryLogic.SVC_RUN,
           "a normal start monitors");
        ck(BoundaryLogic.serviceAction(false, true, true)
               == BoundaryLogic.SVC_STOP_QUIET,
           "a user-requested stop is silent: they know, they asked");
        ck(BoundaryLogic.serviceAction(true, true, true)
               == BoundaryLogic.SVC_STOP_QUIET,
           "...even when everything else is healthy");
        /* WHY A SYNC REQUEST DID NOT HAPPEN. The exception types are the real
         * ones HttpURLConnection throws, constructed here, so this runs on
         * any JDK with no phone and no network -- which is the whole point:
         * the classification used to happen nowhere, and every one of these
         * reached the screen as the single word SYNC FAILED. */
        ck(BoundaryLogic.netFailure(new java.net.UnknownHostException("nope"))
               == BoundaryLogic.NET_DNS,
           "a server name that does not resolve is a DNS failure");
        ck(BoundaryLogic.netFailure(new java.net.SocketTimeoutException())
               == BoundaryLogic.NET_TIMEOUT,
           "a socket timeout is a timeout");
        ck(BoundaryLogic.netFailure(new javax.net.ssl.SSLHandshakeException("x"))
               == BoundaryLogic.NET_TLS,
           "a refused handshake is a TLS failure");
        ck(BoundaryLogic.netFailure(
               new java.security.cert.CertificateExpiredException())
               == BoundaryLogic.NET_TLS,
           "so is an expired certificate");
        ck(BoundaryLogic.netFailure(new java.net.ConnectException("refused"))
               == BoundaryLogic.NET_UNREACHABLE,
           "a refused connection is unreachable");
        ck(BoundaryLogic.netFailure(new java.net.NoRouteToHostException())
               == BoundaryLogic.NET_UNREACHABLE,
           "so is no route");
        ck(BoundaryLogic.netFailure(new java.io.IOException("stream closed"))
               == BoundaryLogic.NET_OTHER,
           "an unrecognised failure stays generic rather than guessing");
        ck(BoundaryLogic.netFailure(null) == BoundaryLogic.NET_OK,
           "no throwable is no failure");

        /* THE WRAPPING CASE, which is the common one and the reason this
         * walks the chain: HttpURLConnection reports the name failure from
         * getOutputStream as an IOException with the real cause inside, so
         * classifying only the outermost throwable answers OTHER for the
         * failures that are easiest to name. */
        ck(BoundaryLogic.netFailure(
               new java.io.IOException("write failed",
                   new java.net.UnknownHostException("nope")))
               == BoundaryLogic.NET_DNS,
           "a wrapped name failure is still a name failure");
        /* An SSLException IS an IOException, and the certificate failure that
         * explains it is usually one link further in. Both have to answer
         * TLS: "the certificate is not one this phone accepts" is the whole
         * actionable content, and it is lost the moment either link is read
         * as a plain I/O error. */
        ck(BoundaryLogic.netFailure(
               new javax.net.ssl.SSLException("handshake",
                   new java.security.cert.CertificateException("expired")))
               == BoundaryLogic.NET_TLS,
           "TLS is classified before the IOException it inherits from");
        /* A cause chain that loops must not hang the sync worker: it holds no
         * lock and shows no progress, so a hang there is indistinguishable
         * from a server that never answers. (Java rejects a throwable that
         * causes itself, but not a two-link ring.) */
        java.io.IOException a = new java.io.IOException("a");
        java.io.IOException b = new java.io.IOException("b", a);
        a.initCause(b);
        ck(BoundaryLogic.netFailure(a) == BoundaryLogic.NET_OTHER,
           "a cause chain that loops terminates");

        /* THE NUMBERS ARE A WIRE FORMAT: native reads them straight off
         * Ble.syncFail(). `make javacheck` compares them against enum
         * sync_net_fail in app/syncstat.h; this pins the Java side so a
         * renumbering has to break something. */
        ck(BoundaryLogic.NET_OK == 0 && BoundaryLogic.NET_DNS == 1
               && BoundaryLogic.NET_TIMEOUT == 2 && BoundaryLogic.NET_TLS == 3
               && BoundaryLogic.NET_UNREACHABLE == 4
               && BoundaryLogic.NET_OTHER == 5,
           "the failure kinds keep the values native expects");

        /* ---- THE ALARM'S SILENCE PATH: count, ORDER, and failure ----
         *
         * javacheck greps Alarm.java for "stopSound" and "release". A dead
         * call satisfies that grep. A call in the wrong order satisfies it. A
         * call skipped because an earlier statement threw satisfies it. This
         * asserts the effects instead: what ran, in what order, and what
         * still ran after something failed. */
        {
            final StringBuilder log = new StringBuilder();

            /* A well-behaved player: stop, then release, in that order. */
            boolean ok = BoundaryLogic.stopPlayer(new BoundaryLogic.Player() {
                @Override public void stop() { log.append("stop,"); }
                @Override public void release() { log.append("release,"); }
            });
            ck(ok, "a player that stops and releases reports released");
            ck(log.toString().equals("stop,release,"),
               "...having done both, stop FIRST");

            /* A player whose stop() throws: release MUST still happen. This
             * is the looping-alarm case -- the caller drops its reference
             * either way, so a skipped release is a player nothing can ever
             * reach again. */
            final StringBuilder l2 = new StringBuilder();
            ok = BoundaryLogic.stopPlayer(new BoundaryLogic.Player() {
                @Override public void stop() { l2.append("stop,");
                    throw new RuntimeException("stop failed"); }
                @Override public void release() { l2.append("release,"); }
            });
            ck(l2.toString().equals("stop,release,"),
               "a stop() that throws does NOT skip release()");
            ck(ok, "...and the release that succeeded is reported as one");

            /* A release that throws is reported, so the caller can say so. */
            ok = BoundaryLogic.stopPlayer(new BoundaryLogic.Player() {
                @Override public void stop() { }
                @Override public void release() {
                    throw new RuntimeException("release failed"); }
            });
            ck(!ok, "a release() that throws is reported as not released");

            ck(BoundaryLogic.stopPlayer(null),
               "nothing playing is not a failure");
        }

        /* ---- SILENCING IS NOT PARTLY DONE ----
         *
         * Sound, vibration and notification are three ways the user is being
         * alerted. A throw from one must not stop the others: the two that
         * still work are the two the user can still hear and feel. */
        {
            final StringBuilder sorder = new StringBuilder();
            boolean[] r = BoundaryLogic.runIndependent(
                new BoundaryLogic.Attempt() { @Override public void run() {
                    sorder.append("sound,");
                    throw new RuntimeException("player gone"); } },
                new BoundaryLogic.Attempt() { @Override public void run() {
                    sorder.append("vibrate,"); } },
                new BoundaryLogic.Attempt() { @Override public void run() {
                    sorder.append("notify,"); } });
            ck(sorder.toString().equals("sound,vibrate,notify,"),
               "a failed sound stage still leaves vibrate and notify to run");
            ck(!r[0] && r[1] && r[2],
               "...and each stage reports its OWN outcome");
        }

        /* ---- STOPPING A SCAN: the retry state is the point ----
         *
         * Ble.stop() keeps its ScanCallback when the platform refuses, so the
         * 1 Hz self-heal retries the SAME registration instead of stacking a
         * second scan client. That decision is this function's return value. */
        {
            final int[] ncalls = {0};
            ck(BoundaryLogic.stopScan(false, new BoundaryLogic.Attempt() {
                   @Override public void run() { ncalls[0]++; } }),
               "stopping a scan that is not running succeeds");
            ck(ncalls[0] == 0, "...without touching the platform at all");

            ck(BoundaryLogic.stopScan(true, new BoundaryLogic.Attempt() {
                   @Override public void run() { ncalls[0]++; } }),
               "an active scan is stopped");
            ck(ncalls[0] == 1, "...by exactly one call");

            ck(!BoundaryLogic.stopScan(true, new BoundaryLogic.Attempt() {
                   @Override public void run() {
                       throw new RuntimeException("rejected"); } }),
               "a platform that refuses is reported as NOT stopped, so the "
               + "caller keeps its callback and retries the same one");
        }

        /* ---- the notification snapshot ------------------------------- */
        /* THE MIXTURE IS THE BUG, not any one field. Seven volatile fields
         * published one at a time let a build pair new pixels with old
         * dimensions -- which PASSES the length guard when the new plot is
         * larger, and is a read past the end of the array when it is not. */
        {
            int[] small = new int[4 * 4];
            int[] big = new int[40 * 40];
            for (int k = 0; k < big.length; k++)
                big[k] = k + 1;

            BoundaryLogic.Notif n1 =
                BoundaryLogic.nextNotif(null, "T", "X", "100", true, big, 40, 40);
            ck(n1.hasPlot(), "a plot whose pixels match its dimensions is kept");
            ck(n1.w == 40 && n1.h == 40 && n1.px.length == 40 * 40,
               "...with exactly w*h pixels, no more");

            /* EXACTLY w*h, PROVEN -- which the assertion above cannot do on
             * its own, because there big.length IS w*h, so copying the whole
             * array and copying w*h pixels are the same act. A LONGER array
             * separates them. It matters because px.length is what the guard
             * compares against and w*h is what Bitmap.createBitmap reads: a
             * snapshot that kept the surplus would carry pixels no dimension
             * of it accounts for, and the next reader to trust the length
             * instead of w*h would read a plot that is partly the last one. */
            BoundaryLogic.Notif nsub =
                BoundaryLogic.nextNotif(null, "T", "X", "100", true, big, 20, 20);
            ck(nsub.hasPlot(), "a plot smaller than the array it came in is kept");
            ck(nsub.px.length == 20 * 20,
               "...trimmed to w*h, not the length of the caller's array");
            ck(nsub.px[0] == 1 && nsub.px[20 * 20 - 1] == 20 * 20,
               "...and it is the FIRST w*h pixels, in order");

            /* THE COPY. The caller's array is native-owned and can be reused,
             * so a snapshot holding a reference to it is a plot that changes
             * under a build that has already measured it. */
            big[0] = 0x7fffffff;
            ck(n1.px[0] != 0x7fffffff,
               "the snapshot OWNS its pixels: the caller's array cannot alter it");

            /* Pixels too few for the dimensions: no plot at all, rather than a
             * plot that reads past the end of the array. */
            BoundaryLogic.Notif n2 =
                BoundaryLogic.nextNotif(null, "T", "X", "100", true, small, 40, 40);
            ck(!n2.hasPlot(), "pixels too few for w*h are refused outright");
            ck(n2.w == 0 && n2.h == 0,
               "...and the dimensions go with them, so nothing can pair them");

            /* PIXELS WITH NO DIMENSIONS AT ALL. A zero width passes any
             * length test -- 0 pixels fit in any array -- so it is the one
             * bad plot arithmetic alone lets through, and it reaches
             * Bitmap.createBitmap, which rejects a zero-sized bitmap by
             * throwing. That throw is on the notification-building path, so
             * the cost of missing it here is the whole notification. */
            BoundaryLogic.Notif nz =
                BoundaryLogic.nextNotif(null, "T", "X", "100", true, big, 0, 0);
            ck(!nz.hasPlot(), "pixels with a zero dimension are not a plot");

            /* Overflow, not merely shortage: w*h must not wrap into a small
             * positive that the length test then accepts. */
            BoundaryLogic.Notif n3 = BoundaryLogic.nextNotif(
                null, "T", "X", "100", true, small, 65536, 65536);
            ck(!n3.hasPlot(), "dimensions whose product overflows int are refused");

            /* CARRIED FORWARD. A reading with no plot keeps the last plot --
             * and keeps it with ITS OWN dimensions, which is the pairing that
             * used to be done by declining to assign three of seven fields. */
            BoundaryLogic.Notif n4 =
                BoundaryLogic.nextNotif(n1, "T2", "X2", "101", false, null, 0, 0);
            ck(n4.hasPlot(), "a reading with no plot keeps the previous one");
            ck(n4.w == 40 && n4.h == 40 && n4.px.length == 40 * 40,
               "...paired with the dimensions that plot was made with");
            ck("101".equals(n4.value) && "T2".equals(n4.title) && !n4.lock,
               "...while every other field is this reading's, not the last one's");

            /* NO PIXELS BUT NONZERO DIMENSIONS -- the shape a caller that
             * forgot the array arrives in. The array is what is missing, so
             * the dimensions are worth nothing on their own; testing them
             * FIRST would dereference a null array to measure it, and the
             * throw would be caught two frames up in showGlucose, where the
             * whole reading (value, title, trend) is discarded with it, not
             * just its plot. */
            BoundaryLogic.Notif n45 =
                BoundaryLogic.nextNotif(n1, "T3", "X3", "102", true, null, 40, 40);
            ck(n45.hasPlot() && n45.px.length == 40 * 40,
               "dimensions with no pixels keep the previous plot, not a throw");
            ck("102".equals(n45.value), "...and this reading's value survives");

            /* A first-ever call with no plot is simply plotless. */
            BoundaryLogic.Notif n5 =
                BoundaryLogic.nextNotif(null, "T", "X", "", true, null, 0, 0);
            ck(!n5.hasPlot(), "with no previous snapshot there is no plot to keep");
        }

        /* ---- notification ids ---------------------------------------- */
        /* TWO OWNERS OF THE NUMBER 2. Alarm.java had `NID = 2` commented
         * "distinct from the service's id 1"; PancraService had `WARN_ID = 2`.
         * Both comments were correct about id 1 and neither knew about the
         * other, so a glucose alarm and the "no longer monitoring" warning
         * shared one slot: either replaced the other, and either cancel took
         * down the wrong one -- including silencing the alarm's notification
         * while the sound kept playing.
         *
         * Neither comment could have caught it. Only the set can, so the set is
         * what is asserted. */
        {
            ck(BoundaryLogic.notifIdsDistinct(),
               "no two notification ids collide");
            /* SPELLED OUT INDIVIDUALLY TOO. notifIdsDistinct() over a list that
             * forgot an id is a green check over the exact mistake it exists to
             * catch, so the three the app actually posts are named here: a
             * fourth id added without extending notifIds() fails nothing, but
             * one of these three quietly changing to another's value fails
             * both. */
            ck(BoundaryLogic.NOTIF_SERVICE != BoundaryLogic.NOTIF_ALARM,
               "the foreground service and a sounding alarm are different "
               + "notifications");
            ck(BoundaryLogic.NOTIF_ALARM != BoundaryLogic.NOTIF_STOPPED,
               "a sounding alarm and 'no longer monitoring' are different "
               + "notifications -- this is the pair that collided");
            ck(BoundaryLogic.NOTIF_SERVICE != BoundaryLogic.NOTIF_STOPPED,
               "the foreground service and 'no longer monitoring' are "
               + "different notifications");
            ck(BoundaryLogic.notifIds().length == 3,
               "...and the list covers all three");
        }

        /* ---- a scan failure the platform reports late ------------------ */
        /* WHAT WENT WRONG. Ble.scan() installs a ScanCallback and returns
         * success as soon as startScan() has been accepted; the refusal comes
         * later, on a binder thread. onScanFailed() only logged it, native had
         * already latched "a scan is running", and every recovery path it has
         * starts only when that flag is clear -- so the app stopped scanning
         * for the rest of the process. No sensor reconnect after a dropout, no
         * meter noticed when switched on, and nothing on screen: the last
         * reading simply stops ageing forward.
         *
         * The decision below is the half that cannot be tested on a phone
         * without a misbehaving Bluetooth stack, so it is tested here. */
        {
            ck(BoundaryLogic.scanFailureIsCurrent(7, 7, true),
               "the installed scan's own failure is acted on");
            /* SUPERSEDED, and only that: something IS installed and the
             * generations are one apart, so nothing but the comparison can
             * refuse this. Acting on it would release the live scan's callback
             * and tell native its working scan had died. */
            ck(!BoundaryLogic.scanFailureIsCurrent(7, 8, true),
               "a failure for a scan that has been replaced is ignored");
            ck(!BoundaryLogic.scanFailureIsCurrent(8, 7, true),
               "...in either direction: an unknown generation is not current");
            /* NO GENERATION AT ALL. The installed generation is 0 too -- which
             * is what "no scan installed" reads as -- so the two MATCH here and
             * `installed` is true: this case can only be refused by the
             * failedGen <= 0 rule, and becomes "current" the moment that rule
             * is dropped. */
            ck(!BoundaryLogic.scanFailureIsCurrent(0, 0, true),
               "a failure carrying no generation is never the current scan's");
            ck(!BoundaryLogic.scanFailureIsCurrent(-3, -3, true),
               "...nor is a negative one");
            /* ALREADY RELEASED. Generations match and are positive, so only the
             * `installed` rule can refuse it: a stop, or an earlier duplicate
             * failure, has already cleared the callback and told native. A
             * second pass would clear whatever is installed by then. */
            ck(!BoundaryLogic.scanFailureIsCurrent(7, 7, false),
               "a failure arriving after the callback was released is ignored");
        }

        /* ---- GATT SETUP: EVERY TRANSITION, ACCEPTED AND REFUSED --------
         *
         * WHAT WENT WRONG. Ble's GATT callback declared the link READY -- it
         * called native onConnected() -- on connections whose MTU negotiation
         * or service discovery had actually failed, and did nothing at all
         * when the platform REFUSED to issue one of those requests (the API
         * returns false and no callback ever follows). Both halves are silent:
         * a refused request leaves the link connected and idle forever, and
         * there is no watchdog during first-time pairing (pancra_link_watchdog
         * skips links that are not `paired`), so the user watches PAIRING
         * until they relaunch. A failed discovery is worse than silent: every
         * later characteristic lookup fails, so the app blames a sensor that
         * is fine while the driver's state machine advances on a connection
         * that was never set up.
         *
         * None of this can be produced on a phone on demand. */
        {
            /* THE ANSWERS MUST BE DISTINCT VALUES, first. Every assertion
             * below compares against one of these constants, so two of them
             * sharing a number would make a green check out of exactly the
             * confusion they exist to prevent (READY == FAIL being the worst
             * possible pair). */
            int[] acts = { BoundaryLogic.GATT_IGNORE, BoundaryLogic.GATT_WAIT,
                           BoundaryLogic.GATT_ASK_MTU,
                           BoundaryLogic.GATT_DISCOVER,
                           BoundaryLogic.GATT_READY, BoundaryLogic.GATT_FAIL };
            for (int i = 0; i < acts.length; i++)
                for (int j = i + 1; j < acts.length; j++)
                    ck(acts[i] != acts[j],
                       "the GATT setup answers are distinct values");

            /* CONNECTED. */
            ck(BoundaryLogic.gattConnected(4, 4, 0) == BoundaryLogic.GATT_ASK_MTU,
               "a clean CONNECTED asks for the bigger MTU");
            /* SUPERSEDED, and only that: the status is success, so nothing but
             * the generation comparison can refuse it. Acting would send a
             * request on behalf of an attempt that has been replaced. */
            ck(BoundaryLogic.gattConnected(4, 5, 0) == BoundaryLogic.GATT_IGNORE,
               "a CONNECTED for a replaced attempt is ignored");
            ck(BoundaryLogic.gattConnected(5, 4, 0) == BoundaryLogic.GATT_IGNORE,
               "...in either direction");
            /* BAD STATUS, and only that: the generations match, so only the
             * status test can refuse it. 133 is the stack's generic GATT
             * error, the one that actually shows up here. */
            ck(BoundaryLogic.gattConnected(4, 4, 133) == BoundaryLogic.GATT_FAIL,
               "CONNECTED with a failure status is closed, not set up");

            /* THE REQUEST RETURN VALUES. These are the two signals that were
             * thrown away entirely, and they are NOT the same decision: an
             * MTU request that was never issued can be worked around, a
             * discovery that was never issued cannot. */
            ck(BoundaryLogic.gattMtuRequested(true) == BoundaryLogic.GATT_WAIT,
               "an issued MTU request is waited for");
            ck(BoundaryLogic.gattMtuRequested(false)
                   == BoundaryLogic.GATT_DISCOVER,
               "a REFUSED MTU request falls back to discovery at the default "
               + "MTU -- every message is chunked to 20 bytes anyway, and "
               + "waiting for a callback that is not coming is the stall");
            ck(BoundaryLogic.gattDiscoverRequested(true)
                   == BoundaryLogic.GATT_WAIT,
               "an issued discovery is waited for");
            ck(BoundaryLogic.gattDiscoverRequested(false)
                   == BoundaryLogic.GATT_FAIL,
               "a REFUSED discovery has no fallback: there is no other way to "
               + "get a service table, so the link is closed and reported");

            /* MTU COMPLETION. The asymmetry, asserted: a failed negotiation
             * still proceeds, because the link is live and the default MTU
             * carries everything this app sends. */
            ck(BoundaryLogic.gattMtuChanged(4, 4, 0)
                   == BoundaryLogic.GATT_DISCOVER,
               "a negotiated MTU proceeds to discovery");
            ck(BoundaryLogic.gattMtuChanged(4, 4, 133)
                   == BoundaryLogic.GATT_DISCOVER,
               "and so does a FAILED negotiation -- the bigger MTU is an "
               + "optimisation, not a requirement");
            ck(BoundaryLogic.gattMtuChanged(4, 5, 0)
                   == BoundaryLogic.GATT_IGNORE,
               "an MTU completion for a replaced attempt is ignored");

            /* DISCOVERY COMPLETION: the only door to native onConnected(). */
            ck(BoundaryLogic.gattDiscovered(4, 4, 0, 6)
                   == BoundaryLogic.GATT_READY,
               "a successful discovery with services is the link being ready");
            ck(BoundaryLogic.gattDiscovered(4, 4, 133, 6)
                   == BoundaryLogic.GATT_FAIL,
               "a FAILED discovery is never ready, however many services the "
               + "table happens to hold");
            /* SUCCESS WITH AN EMPTY TABLE. Status and generation are both
             * fine, so only the service-count rule can refuse this -- and
             * find() iterates getServices(), so zero services is a link on
             * which every subscribe answers 'char not found'. */
            ck(BoundaryLogic.gattDiscovered(4, 4, 0, 0)
                   == BoundaryLogic.GATT_FAIL,
               "a discovery that found NOTHING is not a set-up link");
            ck(BoundaryLogic.gattDiscovered(4, 5, 0, 6)
                   == BoundaryLogic.GATT_IGNORE,
               "a discovery completing for a replaced attempt is ignored -- "
               + "declaring THAT ready hands the driver another connection");
        }

        /* ---- EXPORT SNAPSHOTS: THE NAME ---------------------------------
         *
         * WHAT THE USER SAW. Every export wrote files/pancra.csv and shared a
         * URI naming it. The recipient opens that URI when it needs the bytes
         * -- a mail client reads the attachment as the message is SENT -- so a
         * second export truncated and rewrote the first recipient's file
         * mid-read. The colleague or the doctor received an empty file, or
         * half of one export and half of another, and the sender saw an
         * ordinary share sheet and a correct file on the phone.
         *
         * The name is now two rules in one string: unique, so exports cannot
         * collide, and CONSTRAINED, so the provider that resolves it cannot be
         * walked out of the snapshot directory. */
        {
            String n = BoundaryLogic.exportName(1755000000L, 0x0badf00dL);
            ck("pancra-1755000000-0badf00d.csv".equals(n),
               "the export name is prefix, 10 epoch digits, 8 lowercase hex, "
               + ".csv -- exactly");
            ck(n.length() == BoundaryLogic.EXPORT_NAME_LEN,
               "...which is 30 characters");
            ck(BoundaryLogic.exportNameValid(n),
               "...and the generator's own output passes the validator");

            /* ZERO-PADDED, BOTH FIELDS. A short timestamp or a small tag must
             * not shorten the name: the provider's first test is the exact
             * length, so a 9-digit clock would make every export fail to
             * open, in the recipient's app, with nothing on this phone to
             * see. */
            ck("pancra-0000012345-00000000.csv"
                   .equals(BoundaryLogic.exportName(12345L, 0L)),
               "a small clock and a zero tag are padded, not shortened");
            /* Out-of-range instants are folded in rather than allowed to
             * change the length -- year 2286 and beyond keeps its low digits,
             * and a negative clock counts as zero. Uniqueness is the tag's
             * job, not the timestamp's. */
            ck(BoundaryLogic.exportName(10000000000L + 42L, 0L)
                   .equals("pancra-0000000042-00000000.csv"),
               "an 11-digit instant keeps the grammar's 10 digits");
            String neg = BoundaryLogic.exportName(-5L, 0xffffffffL);
            ck(neg.equals("pancra-0000000000-ffffffff.csv"),
               "a clock before the epoch still produces a valid name");
            ck(BoundaryLogic.exportName(1L, 0xabcdef0123456789L)
                   .endsWith("-23456789.csv"),
               "the tag is the LOW 32 bits, 8 hex digits, never more");
            /* DIFFERENT TAGS, DIFFERENT FILES: this is the whole uniqueness
             * claim the filesystem's createNewFile() then enforces. */
            ck(!BoundaryLogic.exportName(1755000000L, 1L)
                    .equals(BoundaryLogic.exportName(1755000000L, 2L)),
               "two exports in the same second get different names");

            /* ---- WHAT THE PROVIDER MUST REFUSE ----
             *
             * TRAVERSAL FIRST, and honestly: each of these is refused by
             * SEVERAL rules at once (length, prefix, character set), so none
             * of them pins any single rule -- they pin the conjunction, which
             * is what a hostile or careless segment actually meets. The
             * one-character mutations further down are the isolating cases. */
            ck(!BoundaryLogic.exportNameValid(null),
               "a null segment is refused");
            ck(!BoundaryLogic.exportNameValid(""),
               "an empty segment is refused");
            ck(!BoundaryLogic.exportNameValid(".."),
               "'..' is refused");
            ck(!BoundaryLogic.exportNameValid("/"),
               "a bare slash is refused");
            ck(!BoundaryLogic.exportNameValid("../../databases/keys.db"),
               "a relative path out of the directory is refused");
            ck(!BoundaryLogic.exportNameValid(
                   "/data/data/com.jk.pancra/files/stelo.key"),
               "an absolute path is refused");
            ck(!BoundaryLogic.exportNameValid("pancra.csv"),
               "the OLD live filename is refused: it is the shared, "
               + "truncatable file this whole change replaces");
            ck(!BoundaryLogic.exportNameValid(
                   "../exports/pancra-1755000000-0badf00d.csv"),
               "a valid name behind a path prefix is still refused");
            ck(!BoundaryLogic.exportNameValid(
                   "pancra-1755000000-0badf00d.csv/x"),
               "a valid name with anything appended after a slash is refused");
            ck(!BoundaryLogic.exportNameValid(
                   " pancra-1755000000-0badf00d.csv"),
               "a leading space is refused");
            ck(!BoundaryLogic.exportNameValid(
                   "pancra-1755000000-0badf00d.csv\n"),
               "a trailing newline is refused");

            /* ---- ONE CHARACTER, ONE RULE: the isolating cases ----
             *
             * Each of these differs from the valid name above in a single
             * character and passes every rule except one, so each is refused
             * by exactly one clause of the validator. Take that clause out and
             * this is the assertion that fails. */
            ck(!BoundaryLogic.exportNameValid("pancrb-1755000000-0badf00d.csv"),
               "one wrong character in the prefix is refused (prefix rule)");
            ck(!BoundaryLogic.exportNameValid("pancra-175500000a-0badf00d.csv"),
               "a letter among the ten digits is refused (digit rule)");
            ck(!BoundaryLogic.exportNameValid("pancra-1755000000_0badf00d.csv"),
               "the separator must be '-' (separator rule)");
            ck(!BoundaryLogic.exportNameValid("pancra-1755000000-0badg00d.csv"),
               "'g' is not a hex digit (hex rule)");
            ck(!BoundaryLogic.exportNameValid("pancra-1755000000-0BADF00D.csv"),
               "UPPERCASE hex is refused: one snapshot, one spelling "
               + "(hex rule, lowercase half)");
            ck(!BoundaryLogic.exportNameValid("pancra-1755000000-0badf00d.tsv"),
               "a different extension is refused (suffix rule)");
            /* OVER-LENGTH, ISOLATED. This is the hard one to isolate: extra
             * characters at the END also break the .csv suffix test, and
             * extra characters at the FRONT break the prefix test. Inserting
             * them in the MIDDLE leaves the prefix, the ten digits, the
             * separator, the eight hex digits and the .csv suffix all intact
             * -- so only the length test refuses it, and only the length test
             * stands between the provider and a name it was never granted. */
            ck(!BoundaryLogic.exportNameValid(
                   "pancra-1755000000-0badf00dextra.csv"),
               "a longer name whose every checked position is valid is still "
               + "refused (length rule, in isolation)");
            /* UNDER-LENGTH. Note honestly: this one is refused by the length
             * test, and WITHOUT that test the validator would index past the
             * end and throw instead of answering false -- which is why the
             * length test is first, and why a provider must never let a
             * question about a name become an exception. */
            ck(!BoundaryLogic.exportNameValid("pancra-175500000-0badf00d.csv"),
               "a name one digit short is refused (length rule)");

            /* ---- RETENTION: 'LONG ENOUGH FOR A DELAYED READER' ----
             *
             * The recipient reads when it sends, which can be hours later. So
             * a snapshot is kept for 24 hours, and the four newest are kept
             * whatever their age. Deleting one early breaks a recipient's
             * read; keeping one late costs kilobytes of private storage. */
            long day = BoundaryLogic.EXPORT_RETAIN_MS;
            long now = 1755000000000L;
            /* THE EXPORT JUST MADE, and the three before it. Only the rank
             * rule can refuse these -- they are far older than the retention
             * window -- and the newest one is the file the share sheet is
             * pointing at right now. */
            for (int r = 0; r < BoundaryLogic.EXPORT_KEEP_NEWEST; r++)
                ck(!BoundaryLogic.exportSnapshotExpendable(now,
                       now - 100 * day, r),
                   "the newest snapshots are kept however old they are "
                   + "(rank rule)");
            ck(BoundaryLogic.exportSnapshotExpendable(now,
                   now - day - 1, BoundaryLogic.EXPORT_KEEP_NEWEST),
               "an older snapshot past the retention window is expendable");
            /* THE BOUNDARY, both sides. A reader that opens exactly a day
             * later still finds its bytes. */
            ck(!BoundaryLogic.exportSnapshotExpendable(now, now - day,
                   BoundaryLogic.EXPORT_KEEP_NEWEST),
               "exactly one day old is still retained (the comparison is "
               + "strictly greater)");
            ck(!BoundaryLogic.exportSnapshotExpendable(now, now - day / 2,
                   BoundaryLogic.EXPORT_KEEP_NEWEST),
               "half a day old is retained");
            /* AN UNKNOWN AGE READS AS 1970. File.lastModified() answers 0
             * when it cannot tell, and 0 is infinitely old: without this rule
             * the file a reader is waiting for is deleted BECAUSE the
             * filesystem could not answer a question about it. */
            ck(!BoundaryLogic.exportSnapshotExpendable(now, 0,
                   BoundaryLogic.EXPORT_KEEP_NEWEST),
               "a snapshot whose age cannot be read is kept (mtime rule)");
            /* A FUTURE TIMESTAMP -- a clock that moved -- is kept. This one
             * cannot fail on its own: a negative age is not greater than a
             * day either, so the retention comparison already refuses it. It
             * is asserted as behaviour, and the guard in BoundaryLogic says in
             * its comment that no test can pin it. */
            ck(!BoundaryLogic.exportSnapshotExpendable(now, now + day,
                   BoundaryLogic.EXPORT_KEEP_NEWEST),
               "a snapshot stamped in the future is kept, not treated as old");
            ck(BoundaryLogic.EXPORT_KEEP_NEWEST >= 1
                   && BoundaryLogic.EXPORT_RETAIN_MS >= 60L * 60 * 1000,
               "the retention rule keeps at least one snapshot, for at least "
               + "an hour: a cleanup that can empty the directory is the "
               + "truncation bug with extra steps");
        }

        /* ---- WHEN THE FOREGROUND PROMOTION IS REFUSED -------------------
         *
         * This is the case a phone cannot be made to produce on demand.
         * startForeground() is refused by a platform decision -- an FGS start
         * from the background on Android 12+, a foregroundServiceType the
         * manifest does not carry, a permission that was not granted -- and
         * until it happens on somebody's actual device, with a CGM attached
         * to it, the path is never taken. So it is arranged here instead.
         *
         * A SUCCESSFUL promotion proves nothing about any of this: the old
         * code, which caught every failure and carried on regardless, passes
         * every assertion about a start that worked. The isolating cases are
         * the refusals. */
        {
            /* WHICH REFUSAL IS THIS. By class NAME, because BoundaryLogic is
             * compiled without android.jar and cannot name the platform
             * types -- which is also what lets these run at all. */
            ck(BoundaryLogic.promotionKind(
                   "android.app.ForegroundServiceStartNotAllowedException")
                   == BoundaryLogic.PROMO_RETRY,
               "an FGS start refused for being in the background is "
               + "RECOVERABLE: the same call from the foreground succeeds");
            ck(BoundaryLogic.promotionKind(
                   "android.app.MissingForegroundServiceTypeException")
                   == BoundaryLogic.PROMO_DEAD,
               "a missing foregroundServiceType is a manifest fault: it will "
               + "fail identically on every start");
            ck(BoundaryLogic.promotionKind(
                   "android.app.InvalidForegroundServiceTypeException")
                   == BoundaryLogic.PROMO_DEAD,
               "so is a type that does not match the one declared");
            ck(BoundaryLogic.promotionKind("java.lang.SecurityException")
                   == BoundaryLogic.PROMO_DEAD,
               "a permission this build was not granted is terminal too");
            ck(BoundaryLogic.promotionKind("java.lang.IllegalStateException")
                   == BoundaryLogic.PROMO_UNKNOWN,
               "a name this list does not carry is not classified");
            ck(BoundaryLogic.promotionKind(null)
                   == BoundaryLogic.PROMO_UNKNOWN,
               "and neither is no name at all");

            ck(BoundaryLogic.promotionOutcome(true, null)
                   == BoundaryLogic.PROMO_OK,
               "a notification that was built and posted is a promoted "
               + "service");
            /* NOTHING COULD BE BUILT. build() is total and answers null only
             * when even the plain fallback failed -- a context with no
             * NotificationManager -- so there is no notification to present
             * and no retry that produces one. */
            ck(BoundaryLogic.promotionOutcome(false, null)
                   == BoundaryLogic.PROMO_DEAD,
               "no notification at all is a service that cannot be promoted, "
               + "however many times it tries");
            ck(BoundaryLogic.promotionOutcome(true, new SecurityException("x"))
                   == BoundaryLogic.PROMO_DEAD,
               "a real SecurityException is classified by its own name");
            ck(BoundaryLogic.promotionOutcome(true,
                   new IllegalStateException("who knows"))
                   == BoundaryLogic.PROMO_RETRY,
               "an unrecognised refusal is assumed recoverable: the cost is "
               + "a wake alarm, and the alternative is a phone that has "
               + "silently stopped watching and will not try again");
            /* WRAPPED, which is how the framework usually delivers it. */
            ck(BoundaryLogic.promotionOutcome(true,
                   new RuntimeException("start failed",
                       new SecurityException("no permission")))
                   == BoundaryLogic.PROMO_DEAD,
               "a wrapped permission failure is still a permission failure");
            /* A CHAIN THAT LOOPS must terminate: this runs inside a Service
             * lifecycle method, and a hang there is an ANR that kills the
             * process holding the CGM link. */
            java.io.IOException p1 = new java.io.IOException("p");
            java.io.IOException p2 = new java.io.IOException("q", p1);
            p1.initCause(p2);
            ck(BoundaryLogic.promotionOutcome(true, p1)
                   == BoundaryLogic.PROMO_RETRY,
               "a looping cause chain terminates rather than hanging "
               + "onStartCommand");

            /* ---- THE ISOLATING CASE: A REFUSED PROMOTION ----------------
             *
             * THE WAKELOCK IS HELD ON THE WAY IN, and that is the whole
             * point of arranging it that way. The field is static and
             * outlives one onStartCommand, so the realistic shape of this
             * failure is a start that succeeded ten minutes ago (taking the
             * lock) followed by a wake-alarm start the platform refuses. An
             * assertion that started from an unheld lock would pass on code
             * that merely declines to acquire one -- which is not the
             * question. The question is whether the phone is still being
             * held awake for a service that is about to stop. */
            Ops r = new Ops();
            r.wakelock = true;
            boolean sticky = BoundaryLogic.serviceStart(
                BoundaryLogic.SVC_RUN, BoundaryLogic.PROMO_RETRY, r);
            ck(!r.wakelock,
               "a REFUSED promotion does not leave the CPU held awake");
            ck(r.warns == 1,
               "...and publishes the monitoring-stopped notice exactly once");
            ck(r.stops == 1, "...and stops the service");
            ck(!sticky,
               "...and is NOT sticky: sticky means restart me the same way, "
               + "and the same way is what just failed");
            ck(!r.did('H'),
               "no wakelock is taken on a service that is not protected");
            ck(!r.did('T'),
               "no heartbeat is started: it would tick a service the OS is "
               + "about to stop");
            ck(r.did('S'),
               "a RECOVERABLE refusal leaves the wake alarm armed, so a "
               + "later start can try again");
            ck(!r.did('C'), "...and does not cancel it");
            ck(r.at('R') < r.at('X') && r.at('W') < r.at('X'),
               "the lock is released and the user told BEFORE the service "
               + "stops, not left to onDestroy");

            /* TERMINAL: the alarm is CANCELLED. Leaving it armed for a
             * manifest fault buys a five-minute loop -- wake, fail, warn,
             * stop, repeat -- for the life of the install. */
            Ops d = new Ops();
            d.wakelock = true;
            ck(!BoundaryLogic.serviceStart(BoundaryLogic.SVC_RUN,
                                           BoundaryLogic.PROMO_DEAD, d),
               "a terminal refusal is not sticky either");
            ck(!d.wakelock, "a terminal refusal releases the wakelock too");
            ck(d.warns == 1, "...and warns");
            ck(d.did('C'),
               "...and CANCELS the wake alarm: a refusal that cannot heal "
               + "must not become a five-minute retry loop forever");
            ck(!d.did('S'), "...and does not re-arm it");
            ck(!d.did('H') && !d.did('T'), "...and monitors nothing");

            /* PROMOTION OUTRANKS THE START INTENT. Both cases above pass
             * SVC_RUN -- a perfectly ordinary start with a live activity and
             * native loaded -- so what refuses them is the promotion and
             * nothing else. This is the assertion the OLD code fails: it
             * reached holdWakelock/startTicking/scheduleWake on exactly this
             * input. */
            ck(!d.did('H') && !d.did('T') && !r.did('H') && !r.did('T'),
               "a normal SVC_RUN start does NOT monitor when the service was "
               + "never promoted");

            /* ONE NOTICE, NOT TWO, when both things are wrong at once. */
            Ops both = new Ops();
            BoundaryLogic.serviceStart(BoundaryLogic.SVC_WARN_STOPPED,
                                       BoundaryLogic.PROMO_RETRY, both);
            ck(both.warns == 1 && both.stops == 1,
               "a recreation whose promotion was also refused warns once and "
               + "stops once");

            /* THE HEALTHY START still does all three things, or this policy
             * would be a very thorough way of never monitoring anything. */
            Ops ok = new Ops();
            ck(BoundaryLogic.serviceStart(BoundaryLogic.SVC_RUN,
                                          BoundaryLogic.PROMO_OK, ok),
               "a promoted, intended start is sticky");
            ck(ok.wakelock && ok.did('T') && ok.did('S'),
               "...and holds the CPU, ticks the alarm and re-arms the wake");
            ck(ok.warns == 0 && ok.stops == 0,
               "...and neither warns nor stops");

            /* THE RECREATION CASES are unchanged by any of this: a promoted
             * service with a null intent still warns and stops, and a
             * user-requested stop still goes quietly. */
            Ops rec = new Ops();
            ck(!BoundaryLogic.serviceStart(BoundaryLogic.SVC_WARN_STOPPED,
                                           BoundaryLogic.PROMO_OK, rec),
               "a null-intent recreation is not sticky");
            ck(rec.warns == 1 && rec.stops == 1 && !rec.did('H'),
               "...and warns, stops, and takes no wakelock");
            Ops quiet = new Ops();
            ck(!BoundaryLogic.serviceStart(BoundaryLogic.SVC_STOP_QUIET,
                                           BoundaryLogic.PROMO_OK, quiet),
               "a user-requested stop is not sticky");
            ck(quiet.warns == 0 && quiet.stops == 1,
               "...and stops without a notice: they asked, they know");

            /* A STEP THAT THROWS MUST NOT CANCEL THE ONES AFTER IT. Every
             * one of these is an Android call. releaseWakelock throwing
             * (a lock released twice, a dead PowerManager) used to be the
             * kind of thing that skipped the notice and the stopSelf with
             * it -- and the stopSelf is what discharges the
             * startForegroundService obligation the system otherwise kills
             * the process over. */
            Ops thrower = new Ops();
            thrower.wakelock = true;
            thrower.throwOnRelease = true;
            BoundaryLogic.serviceStart(BoundaryLogic.SVC_RUN,
                                       BoundaryLogic.PROMO_DEAD, thrower);
            ck(thrower.warns == 1 && thrower.stops == 1,
               "a throw from one step still leaves the user warned and the "
               + "service stopped");
        }

        /* ==== ITEMS 122 AND 123: THE BOUNDED, DEADLINED SYNC READ ======
         *
         * (Appended as its own section, below everything above it.)
         *
         * These are the two rules that stop a sync server -- hostile, broken,
         * or a captive portal in a hotel -- from taking down the process that
         * is watching somebody's glucose. Every case here runs the SAME
         * method Ble.syncHttp calls; the streams and the clock are fakes,
         * the bounds are not.
         *
         * The stream fakes below deliberately serve MORE than they are asked
         * to, forever, because that is the shape of the attack: a server that
         * never stops. If a bound is removed, these do not fail an assertion
         * -- they hang or die of OutOfMemoryError. Both are counted as kills
         * in the mutation notes, and both are named there rather than
         * pretended to be clean assertion failures. */
        /* WRAPPED rather than adding `throws` to main(): main is shared with
         * every other section of this suite and a signature change there is a
         * change to their code. None of the fakes below can actually throw --
         * they are arrays and counters -- so an IOException here would mean
         * the test harness itself is broken, and it says so. */
        try {
            /* A clock that only moves when told. Nothing here sleeps: a test
             * that waited out a 45-second budget would be a test nobody
             * runs. */
            final long[] mono = {1000000000L};
            BoundaryLogic.MonoClock clk = () -> mono[0];

            /* A STREAM THAT NEVER ENDS, one byte at a time -- the item 123
             * server exactly: it is never idle long enough for a per-read
             * timeout to fire, and it is never going to finish. `served`
             * records how much it was actually asked for, and `maxWant` the
             * largest single request, which is how the read-size rule gets
             * pinned. */
            final class Dribble extends java.io.InputStream {
                long served;
                int maxWant;
                long tickNs; /* how far the clock moves per read */

                @Override public int read() {
                    byte[] one = new byte[1];
                    return read(one, 0, 1) < 0 ? -1 : one[0] & 0xff;
                }
                @Override public int read(byte[] b, int off, int len) {
                    if (len > maxWant) maxWant = len;
                    mono[0] += tickNs;
                    b[off] = (byte) 'x';
                    served++;
                    return 1; /* ONE byte, whatever was asked for */
                }
            }

            /* A stream of exactly n bytes, handed over as fast as asked. */
            final class Fixed extends java.io.InputStream {
                long left;
                long served;
                Fixed(long n) { left = n; }

                @Override public int read() {
                    byte[] one = new byte[1];
                    return read(one, 0, 1) < 0 ? -1 : one[0] & 0xff;
                }
                @Override public int read(byte[] b, int off, int len) {
                    if (left <= 0) return -1;
                    int n = (int) Math.min(len, Math.min(left, 4096));
                    java.util.Arrays.fill(b, off, off + n, (byte) 'x');
                    left -= n;
                    served += n;
                    return n;
                }
            }

            final long BIG = 1000000L;   /* a budget nothing here can exhaust */
            final long LIM = 100L;       /* a small, obvious size limit */

            /* ---- 122: THE SIZE BOUND ------------------------------------
             *
             * The predicate first, on its own, then the loop that uses it. */
            ck(!BoundaryLogic.bodyBudgetExceeded(LIM, LIM),
               "a body of exactly the limit is within budget");
            ck(BoundaryLogic.bodyBudgetExceeded(LIM + 1, LIM),
               "ONE byte past the limit is over budget");

            /* EXACTLY AT THE LIMIT IS ACCEPTED. This is the case that stops
             * the fix from being "refuse everything", and it is the one an
             * off-by-one in either direction breaks. */
            Fixed atLimit = new Fixed(LIM);
            BoundaryLogic.BodyRead ok =
                BoundaryLogic.readBoundedBody(atLimit, null, LIM, clk,
                                              mono[0], BIG);
            ck(ok.body != null, "a body of exactly the limit is accepted");
            ck(ok.body.length == LIM, "...and arrives whole, all 100 bytes");
            ck(ok.fail == BoundaryLogic.NET_OK, "...and reports no failure");

            /* ONE BYTE OVER IS REFUSED. THE ISOLATING CASE for item 122: a
             * comfortably oversized body would also be refused by a limit
             * that was wrong by a hundred bytes, or by a thousand. This one
             * differs from the accepted case above by a single byte, so only
             * a limit that is exactly right passes both. */
            Fixed overByOne = new Fixed(LIM + 1);
            BoundaryLogic.BodyRead no =
                BoundaryLogic.readBoundedBody(overByOne, null, LIM, clk,
                                              mono[0], BIG);
            ck(no.body == null, "a body ONE byte over the limit is refused");
            ck(no.fail == BoundaryLogic.NET_OTHER,
               "...and is reported as a transport failure, not a timeout");
            ck(no.accumulated <= LIM,
               "...and the over-limit byte was never accumulated");

            /* THE ABORT POINT, which is the actual item. Returning null only
             * proves the caller was told no; it says nothing about the heap,
             * and the heap is what an OutOfMemoryError in the BLE callback
             * comes out of. This server offers ten megabytes. Fewer than 101
             * bytes may be held, and fewer than 102 read. */
            Fixed flood = new Fixed(10L * 1024 * 1024);
            BoundaryLogic.BodyRead cut =
                BoundaryLogic.readBoundedBody(flood, null, LIM, clk,
                                              mono[0], BIG);
            ck(cut.body == null, "a ten-megabyte answer is refused");
            ck(cut.accumulated <= LIM,
               "...having accumulated no more than the limit");
            ck(cut.consumed <= LIM + 1,
               "...and having READ no more than one byte past it: the abort "
               + "is on the first excess byte, not the first excess buffer");
            ck(flood.served <= LIM + 1,
               "...so the server got to send 101 bytes of its ten megabytes");

            /* THE READ-SIZE RULE that makes that exact. Without it the loop
             * asks for 4096 every time and overshoots by up to a bufferful. */
            ck(BoundaryLogic.bodyReadSize(0, LIM, 4096) == LIM + 1,
               "the first read asks for exactly one byte past the limit");
            ck(BoundaryLogic.bodyReadSize(LIM, LIM, 4096) == 1,
               "at the limit it still asks for one byte, to detect the "
               + "overrun");
            ck(BoundaryLogic.bodyReadSize(0, 10L * 1024 * 1024, 4096) == 4096,
               "a limit larger than the buffer does not enlarge the buffer");
            ck(BoundaryLogic.bodyReadSize(LIM + 5, LIM, 4096) == 1,
               "a read size is NEVER zero -- zero reads as end of stream and "
               + "would report a truncated body as a complete one");

            /* ---- 122: CONTENT-LENGTH ------------------------------------ */
            ck(BoundaryLogic.contentLengthRefused("101", LIM),
               "a declared length over the limit is refused");
            ck(!BoundaryLogic.contentLengthRefused("100", LIM),
               "a declared length exactly at the limit is not");
            ck(!BoundaryLogic.contentLengthRefused(null, LIM),
               "an absent Content-Length is not, by itself, a refusal");
            ck(BoundaryLogic.contentLengthRefused("9999999999999999999999999",
                                                  LIM),
               "a length too long to fit in a long is refused rather than "
               + "thrown on -- Long.parseLong would raise here, and a bounds "
               + "check that throws is a bounds check that did not run");
            ck(!BoundaryLogic.contentLengthRefused("not-a-number", LIM),
               "a malformed length is not guessed at; the counter decides");

            /* REFUSED BEFORE A SINGLE BYTE IS READ. The point of the header
             * check is that an honest oversized reply costs nothing, so the
             * assertion is on `served`, not on the return value. */
            Dribble untouched = new Dribble();
            BoundaryLogic.BodyRead early =
                BoundaryLogic.readBoundedBody(untouched, "999999", LIM, clk,
                                              mono[0], BIG);
            ck(early.body == null, "a declared oversize length is refused");
            ck(untouched.served == 0,
               "...BEFORE reading a byte of it: the stream was never touched");
            ck(early.consumed == 0, "...and nothing was consumed");

            /* A LYING CONTENT-LENGTH. The obvious wrong fix is to trust the
             * header, and it is worse than no fix because it looks like one:
             * this server declares twelve bytes and then sends for ever. The
             * header check passes it. The BYTE COUNTER is what refuses it,
             * and this case is the only one that says so. */
            Dribble liar = new Dribble();
            ck(!BoundaryLogic.contentLengthRefused("12", LIM),
               "a small declared length passes the header check");
            BoundaryLogic.BodyRead lied =
                BoundaryLogic.readBoundedBody(liar, "12", LIM, clk,
                                              mono[0], BIG);
            ck(lied.body == null,
               "a server that declares 12 bytes and streams for ever is "
               + "still refused");
            ck(lied.consumed <= LIM + 1,
               "...on the streamed bytes, at the same limit as any other");
            ck(lied.accumulated <= LIM,
               "...without accumulating past the limit either");

            /* AND WITH NO CONTENT-LENGTH AT ALL -- chunked encoding, which is
             * what a streaming middlebox actually sends. Same bound. */
            Dribble chunked = new Dribble();
            BoundaryLogic.BodyRead noLen =
                BoundaryLogic.readBoundedBody(chunked, null, LIM, clk,
                                              mono[0], BIG);
            ck(noLen.body == null,
               "an endless body with NO declared length is still bounded");
            ck(noLen.consumed <= LIM + 1, "...at the same limit");

            /* ---- THE LIMIT IS NATIVE'S, NOT A NUMBER CHOSEN IN JAVA ----
             *
             * app/sync.h: SYNC_BUF_MAX is 256*1024, and syncjni.c's jni_http
             * refuses `len > outcap - 1`. So 262143 is accepted by native and
             * 262144 is not, and this side must agree exactly or the two
             * halves disagree about what a legal reply is. `make javacheck`
             * greps the C constant and fails the build on a mismatch; this
             * pins the off-by-one that the grep cannot see. */
            ck(BoundaryLogic.SYNC_BODY_CAP == 256L * 1024,
               "the Java cap mirrors SYNC_BUF_MAX");
            ck(BoundaryLogic.SYNC_BODY_MAX == 262143L,
               "the largest ACCEPTED body is outcap-1, which is what "
               + "jni_http still takes -- it NUL-terminates at out[n]");
            ck(!BoundaryLogic.bodyBudgetExceeded(262143L,
                                                 BoundaryLogic.SYNC_BODY_MAX),
               "262143 bytes, which native accepts, is accepted here");
            ck(BoundaryLogic.bodyBudgetExceeded(262144L,
                                                BoundaryLogic.SYNC_BODY_MAX),
               "262144 bytes, which native refuses, is refused here -- and "
               + "now BEFORE the heap has held it");

            /* ---- 123: THE WHOLE-REQUEST DEADLINE ------------------------
             *
             * All of these use a size limit far larger than anything the
             * streams produce, so nothing below can be refused for being too
             * big. Whatever refuses them is the clock. */
            final long ROOMY = 10L * 1024 * 1024;

            ck(BoundaryLogic.deadlineRemainingMs(0, 0, 45000) == 45000,
               "at the start, the whole budget remains");
            ck(BoundaryLogic.deadlineRemainingMs(0, 44000000000L, 45000)
                   == 1000,
               "44 seconds in, one second remains");
            ck(!BoundaryLogic.deadlineExpired(0, 44000000000L, 45000),
               "...and it has not expired");
            ck(BoundaryLogic.deadlineExpired(0, 45000000000L, 45000),
               "at exactly the budget it HAS expired: non-positive remaining "
               + "is expired, so a budget of zero refuses at once rather "
               + "than meaning 'for ever'");
            ck(BoundaryLogic.deadlineExpired(0, 0, 0),
               "a zero budget is expired immediately, not infinite");

            /* A CLOCK THAT RAN BACKWARDS MUST NOT READ AS TIME REMAINING.
             * This is item 70's bug in its most dangerous form: there, a
             * negative elapsed made stale state look fresh. Here it would
             * make the remaining budget LARGER than the budget -- the wedged
             * request that this whole item exists to kill would be granted
             * more time by the clock moving. */
            ck(BoundaryLogic.deadlineRemainingMs(50000000000L, 0, 45000) == 0,
               "a clock that went backwards leaves NO budget, rather than "
               + "more than the budget");
            ck(BoundaryLogic.deadlineExpired(50000000000L, 0, 45000),
               "...and reads as expired, not as fresh");
            ck(BoundaryLogic.deadlineRemainingMs(0, -1, 45000) == 0,
               "one nanosecond backwards is already no budget: the guard is "
               + "on the sign of the difference, not on a tolerance");

            /* AN OVERRUN REPORTS ZERO REMAINING, NEVER A NEGATIVE NUMBER.
             * deadlineExpired's `<= 0` hides the difference, so without this
             * line the clamp is a rule no test can fail on -- it survived the
             * mutation drill until this assertion existed. It is worth
             * pinning because of what a remaining budget is for next: it is
             * the natural argument to setReadTimeout, and a NEGATIVE timeout
             * is an error to every Java socket API while a ZERO one means
             * INFINITE. A negative value passed onward arms no timeout at
             * all, which is this very item's hang reintroduced by its own
             * fix. */
            ck(BoundaryLogic.deadlineRemainingMs(0, 60000000000L, 45000) == 0,
               "fifteen seconds past a 45-second budget reports ZERO "
               + "remaining, not minus fifteen thousand");

            /* ALREADY EXPIRED WHEN THE BODY STARTS: refuse without opening
             * the stream. A connect that ate the entire budget must not then
             * be allowed a full read timeout on top of it. */
            Dribble neverRead = new Dribble();
            mono[0] = 100000000000L;
            BoundaryLogic.BodyRead late =
                BoundaryLogic.readBoundedBody(neverRead, null, ROOMY, clk,
                                              mono[0] - 46000000000L, 45000);
            ck(late.body == null, "an already-expired deadline refuses");
            ck(late.fail == BoundaryLogic.NET_TIMEOUT,
               "...as a TIMEOUT, so the screen says the server was too slow "
               + "and the retry schedule treats it as one");
            ck(neverRead.served == 0,
               "...without reading a byte: THE ISOLATING CASE for the "
               + "deadline, since a stream that was never touched cannot "
               + "have been refused for its size");

            /* THE DRIBBLE, WHICH IS ITEM 123 ITSELF. This server sends one
             * byte every ten seconds: it satisfies a twenty-second per-read
             * idle timeout for ever, and it never finishes. Before this
             * change the worker sat here until the process died, and with it
             * every sync, every pairing and every restore -- because pushExec
             * has a maximum of one thread.
             *
             * Five bytes at ten seconds each is fifty seconds against a
             * forty-five second budget, so the exchange ends mid-stream. */
            Dribble slow = new Dribble();
            slow.tickNs = 10000000000L; /* ten seconds per byte */
            long began = mono[0];
            BoundaryLogic.BodyRead dribbled =
                BoundaryLogic.readBoundedBody(slow, null, ROOMY, clk, began,
                                              45000);
            ck(dribbled.body == null,
               "a server dribbling one byte inside every read timeout is cut "
               + "off by the absolute deadline");
            ck(dribbled.fail == BoundaryLogic.NET_TIMEOUT,
               "...and reported as a timeout");
            ck(slow.served == 5,
               "...after five bytes and fifty seconds, not for ever: the "
               + "deadline is re-checked EVERY time around the loop, which "
               + "is the whole difference from a per-read timeout");
            ck(dribbled.accumulated < 5,
               "...and the partial body is discarded, never handed back as "
               + "a short answer");

            /* THE SAME SERVER, UNDER A PER-READ TIMEOUT, would never stop.
             * Stated as an assertion rather than a comment: at ten seconds a
             * byte, no individual read ever approaches the twenty-second idle
             * timeout, so nothing but the absolute deadline can end it. */
            ck(!BoundaryLogic.deadlineExpired(0, 10000000000L, 20000),
               "no single ten-second gap trips a twenty-second idle timeout "
               + "-- which is exactly why the idle timeout never fired");

            /* A DRIBBLE THAT FITS still succeeds, or the rule above would be
             * a very thorough way of never syncing. Four bytes at ten seconds
             * is forty seconds, inside the budget, and the stream ends. */
            final class Slow extends java.io.InputStream {
                int left = 4;
                @Override public int read() {
                    if (left-- <= 0) return -1;
                    mono[0] += 10000000000L;
                    return 'x';
                }
                @Override public int read(byte[] b, int off, int len) {
                    int v = read();
                    if (v < 0) return -1;
                    b[off] = (byte) v;
                    return 1;
                }
            }
            mono[0] = 500000000000L;
            long t0 = mono[0];
            BoundaryLogic.BodyRead within =
                BoundaryLogic.readBoundedBody(new Slow(), null, ROOMY, clk,
                                              t0, 45000);
            ck(within.body != null && within.body.length == 4,
               "a slow but FINISHING exchange inside the budget still "
               + "delivers its body");
            ck(within.fail == BoundaryLogic.NET_OK, "...and reports success");
        } catch (java.io.IOException e) {
            throw new AssertionError("no fake stream in this section does "
                                     + "I/O that can fail", e);
        }

        /* ---- 124 & 125: THE EXCHANGE ITSELF ---------------------------
         *
         * Item 124 is a request that goes somewhere it was not signed for.
         * Item 125 is a stream nobody closed. They share a section because
         * they share the code path and because the interesting cases are the
         * same cases: the paths that stop EARLY -- a refused redirect, a body
         * over the limit -- are both the ones a redirect-follower reports as
         * success and the ones a "read until EOF, then close" would never
         * close at all. */
        try {
            final long[] emono = {1000000000L};
            BoundaryLogic.MonoClock eclk = () -> emono[0];
            final long BUDGET = 1000000L; /* nothing here can exhaust it */
            final long ELIM = 100L;       /* a small, obvious size limit */

            /* A RESPONSE BODY THAT KNOWS WHETHER IT WAS CLOSED. That is the
             * whole of item 125's assertion surface: `closed` is the file
             * descriptor and the pooled socket, and `served` is how much of
             * it was pulled before somebody stopped. */
            final class Reply extends java.io.InputStream {
                final StringBuilder log;
                long left;
                long served;
                boolean closed;

                Reply(StringBuilder log, long n) { this.log = log; left = n; }

                @Override public int read() {
                    byte[] one = new byte[1];
                    return read(one, 0, 1) < 0 ? -1 : one[0] & 0xff;
                }
                @Override public int read(byte[] b, int off, int len) {
                    if (left <= 0) return -1;
                    int n = (int) Math.min(len, Math.min(left, 4096));
                    java.util.Arrays.fill(b, off, off + n, (byte) 'x');
                    left -= n;
                    served += n;
                    return n;
                }
                @Override public void close() {
                    closed = true;
                    log.append('c');
                }
            }

            /* ONE CONNECTION, WITH EVERY CALL RECORDED IN ORDER.
             *
             *   O  the request body was opened     C  ...and closed
             *   W  the request body was written    S  the status was asked
             *   R  the response was opened         c  ...and closed
             *
             * The ORDER is an assertion and not decoration: "the request body
             * is finished before the response is read" is a claim about where
             * C sits relative to S, and no other kind of check can make it. */
            final class Conn implements BoundaryLogic.SyncConn {
                final StringBuilder log = new StringBuilder();
                int code = 200;          /* what the server answers */
                int afterRedirect = 200; /* what the TARGET would answer */
                long replyLen = 4;       /* a small, legal body */
                String lenHeader;
                boolean closeReqThrows;
                boolean noBody;          /* getErrorStream() returned null */
                int statusCalls;
                boolean reqOpened;
                boolean reqClosed;
                Reply reply;

                @Override public java.io.OutputStream openRequest(int len) {
                    reqOpened = true;
                    log.append('O');
                    return new java.io.OutputStream() {
                        @Override public void write(int b) { }
                        @Override public void write(byte[] b, int o, int n) {
                            log.append('W');
                        }
                        @Override public void close()
                                throws java.io.IOException {
                            reqClosed = true;
                            log.append('C');
                            /* WHERE A FIXED-LENGTH BODY IS ACTUALLY SENT.
                             * close() is the flush, so it is also the call
                             * that reports a link that died mid-upload. */
                            if (closeReqThrows)
                                throw new java.io.IOException("flush failed");
                        }
                    };
                }
                @Override public int status() {
                    statusCalls++;
                    log.append('S');
                    /* THE SECOND ASK IS THE REDIRECT TARGET'S ANSWER. A
                     * client that follows re-issues the request and reads a
                     * fresh status; this fake will hand out a cheerful 200 to
                     * anything that asks twice, so a suite that lets a
                     * follower through sees the 200 and says so. */
                    return statusCalls == 1 ? code : afterRedirect;
                }
                @Override public String header(String name) {
                    return lenHeader;
                }
                @Override public java.io.InputStream openResponse(int st) {
                    log.append('R');
                    if (noBody)
                        return null; /* getErrorStream() with no error body */
                    reply = new Reply(log, replyLen);
                    return reply;
                }
            }

            /* THE POOL, WHICH IS WHY CLOSING MATTERS AT ALL. A sync is dozens
             * of requests over one TLS handshake, so the connection the next
             * bucket rides is the one this bucket finished with. Handing out
             * a connection whose previous response was never closed is the
             * bug being prevented, so this fake refuses to. */
            final class Pool {
                Conn live;
                Conn next() {
                    if (live != null && live.reply != null
                        && !live.reply.closed)
                        throw new IllegalStateException(
                            "the pooled connection still has an unclosed "
                            + "response on it");
                    live = new Conn();
                    return live;
                }
            }

            /* ---- 124: THE PREDICATE, WHOLE RANGE ------------------------
             *
             * Each redirect status NAMED AND ASSERTED SEPARATELY. 303 forces
             * a GET, 307 and 308 preserve the method, and 301/302 do whatever
             * the client has always done -- a rule written against 302 alone
             * passes a suite that only tests 302, and lets the two that
             * preserve a signed PUT straight through. */
            ck(BoundaryLogic.redirectRefused(301), "301 is refused");
            ck(BoundaryLogic.redirectRefused(302), "302 is refused");
            ck(BoundaryLogic.redirectRefused(303), "303 is refused");
            ck(BoundaryLogic.redirectRefused(307), "307 is refused");
            ck(BoundaryLogic.redirectRefused(308), "308 is refused");
            ck(BoundaryLogic.redirectRefused(304),
               "304 is refused too -- this app sends no conditional "
               + "requests, so a Not Modified is a server we do not "
               + "understand, and it carries no body to hand back");
            ck(BoundaryLogic.redirectRefused(300)
               && BoundaryLogic.redirectRefused(399),
               "the rule is the RANGE, not a list of the numbers that "
               + "existed when it was written");
            ck(!BoundaryLogic.redirectRefused(200)
               && !BoundaryLogic.redirectRefused(299),
               "a 2xx is not a redirect");
            ck(!BoundaryLogic.redirectRefused(400)
               && !BoundaryLogic.redirectRefused(500),
               "an error status is the server's answer, not a redirect");

            /* ---- 124: EVERY REDIRECT, THROUGH THE WHOLE EXCHANGE --------
             *
             * The fake serves a perfectly good four-byte body AT THE REDIRECT
             * ITSELF, which is the strongest form of the case: even when the
             * answer appears to be right there, a 3xx is not an answer to a
             * request signed for somewhere else. */
            int[] hops = {301, 302, 303, 307, 308};
            for (int hop : hops) {
                Conn rc = new Conn();
                rc.code = hop;
                BoundaryLogic.Exchange rx = BoundaryLogic.runExchange(
                    rc, new byte[] {'p', 'u', 't'}, eclk, emono[0],
                    BUDGET, ELIM);
                ck(rx.body == null,
                   "a " + hop + " hands back no body, however plausible the "
                   + "one it came with");
                ck(rx.code == -1,
                   "a " + hop + " is reported as a TRANSPORT failure and not "
                   + "as status " + hop + " -- native reads syncCode() "
                   + "separately, and any non-negative value there is a "
                   + "request that happened");
                ck(rx.fail == BoundaryLogic.NET_OTHER,
                   "...with a named network failure for the screen");
                ck(rc.statusCalls == 1,
                   "...and the " + hop + " target is never asked -- the "
                   + "request is not re-issued anywhere, so the 200 waiting "
                   + "at the new location can never be reported as the "
                   + "outcome of the one we signed");
                ck(rc.reply != null && rc.reply.closed,
                   "...and the redirect's own body is CLOSED, or the next "
                   + "request in this sync inherits the connection it is "
                   + "still sitting on");
                ck(rc.reply.served == 0,
                   "...having been read from not at all");
            }

            /* A REDIRECT WITH NO BODY AT ALL. getErrorStream() returns null
             * for a response that carried nothing, and a close of null is a
             * NullPointerException out of the transport -- which would fail
             * the sync for the wrong reason and, worse, would do it on the
             * path this item added. */
            Conn bare = new Conn();
            bare.code = 302;
            bare.noBody = true;
            BoundaryLogic.Exchange bx = BoundaryLogic.runExchange(
                bare, null, eclk, emono[0], BUDGET, ELIM);
            ck(bx.body == null && bx.code == -1,
               "a redirect with no body is refused like any other");

            /* ---- CONTROLS: THE ORDINARY ANSWERS STILL WORK --------------
             *
             * A rule that refuses everything would pass every assertion above
             * and sync nothing, for ever. */
            Conn okc = new Conn();
            okc.code = 200;
            BoundaryLogic.Exchange ox = BoundaryLogic.runExchange(
                okc, new byte[] {'p', 'u', 't'}, eclk, emono[0],
                BUDGET, ELIM);
            ck(ox.body != null && ox.body.length == 4,
               "CONTROL: a 200 still delivers its body");
            ck(ox.code == 200, "CONTROL: ...and publishes the status 200");
            ck(ox.fail == BoundaryLogic.NET_OK,
               "CONTROL: ...and reports no failure");

            Conn errc = new Conn();
            errc.code = 404;
            BoundaryLogic.Exchange ex4 = BoundaryLogic.runExchange(
                errc, null, eclk, emono[0], BUDGET, ELIM);
            ck(ex4.body != null && ex4.body.length == 4,
               "CONTROL: a 404 still hands its error body to native");
            ck(ex4.code == 404,
               "CONTROL: ...as status 404, which is the server's answer and "
               + "not a transport failure -- sync.c decides what a 404 means");
            ck(ex4.fail == BoundaryLogic.NET_OK,
               "CONTROL: ...with no network failure of our own");

            /* ---- 125: THE ORDER, AND THE CLOSING ------------------------ */
            ck(okc.log.toString().equals("OWCSRc"),
               "the request body is opened, written and CLOSED, and only "
               + "then is the status asked, the response opened and closed "
               + "-- got '" + okc.log + "'");
            ck(okc.log.indexOf("C") < okc.log.indexOf("S"),
               "the request body is finished BEFORE the response is read: "
               + "a fixed-length body is not sent until close() flushes it, "
               + "so asking for the status first is asking a server to "
               + "answer a question we have not finished putting");
            ck(okc.reqClosed, "...and it really was closed");
            ck(okc.reply.closed,
               "the fully consumed response is closed, which is what lets "
               + "the platform put the connection back in the pool");

            /* A REQUEST WITH NO BODY OPENS NO BODY. setDoOutput(true) turns
             * a GET into a POST on HttpURLConnection, and every fetch in a
             * sync or a restore is a GET. */
            ck(!errc.reqOpened,
               "a request with no body never opens an output stream");
            ck(errc.log.toString().equals("SRc"),
               "...and its response is still opened and closed -- got '"
               + errc.log + "'");

            /* AND AN EMPTY BODY IS NO BODY, which is not the same statement
             * and is the one that is actually reachable. Native never hands
             * up a null: jni_http builds a jbyteArray from sync.c's `""` with
             * blen 0, so every GET in a sync arrives here as a byte[0], and
             * so does the empty PUT that closes a bucket. A guard written as
             * `body != null` opens an output stream for all of them --
             * setDoOutput(true) with nothing to send, which is how a GET
             * becomes a POST and every signed fetch stops matching the verb
             * it was signed with. */
            Conn emptyc = new Conn();
            BoundaryLogic.Exchange emx = BoundaryLogic.runExchange(
                emptyc, new byte[0], eclk, emono[0], BUDGET, ELIM);
            ck(!emptyc.reqOpened,
               "a ZERO-LENGTH body opens no output stream either -- it is "
               + "what every GET actually arrives as");
            ck(emptyc.log.toString().equals("SRc"),
               "...so the exchange is status, response, close -- got '"
               + emptyc.log + "'");
            ck(emx.body != null && emx.code == 200,
               "...and it is an ordinary successful request");

            /* A CLOSE THAT THROWS IS A FAILED REQUEST, NOT A QUIET ONE.
             * This is the whole reason the write is a try-with-resources and
             * not a write followed by a swallowed close: with a fixed-length
             * body, close() is the flush, so it is where "the upload did not
             * arrive" is reported. Swallowing it means reading the server's
             * answer to an upload that never landed and reporting it as the
             * answer to one that did -- and for a bucket push, that answer is
             * what makes the phone delete its copy of the rows. */
            Conn badc = new Conn();
            badc.closeReqThrows = true;
            boolean threw = false;
            try {
                BoundaryLogic.runExchange(badc, new byte[] {'p', 'u', 't'},
                                          eclk, emono[0], BUDGET, ELIM);
            } catch (java.io.IOException e) {
                threw = true;
            }
            ck(threw,
               "a request body whose close() fails throws out of the "
               + "exchange -- Ble.syncHttp's catch turns that into status "
               + "-1 and a named NET_* kind, which is 'the request did not "
               + "happen', which is true");
            ck(badc.statusCalls == 0,
               "...and the server's answer is never even asked for, let "
               + "alone reported as the outcome of an upload that failed "
               + "to flush");
            ck(badc.log.toString().equals("OWC"),
               "...the exchange stops at the failed close -- got '"
               + badc.log + "'");

            /* THE ABORTED PATHS ARE CLOSED TOO, and they are the ones that
             * matter: "read until EOF and then close" closes exactly the
             * responses that ended by themselves, which is the opposite of
             * the set that needs it. Item 122's over-limit abort stops with
             * megabytes still queued behind it. */
            Conn bigc = new Conn();
            bigc.replyLen = 10L * 1024 * 1024;
            BoundaryLogic.Exchange gx = BoundaryLogic.runExchange(
                bigc, null, eclk, emono[0], BUDGET, ELIM);
            ck(gx.body == null, "an over-limit body is still refused");
            ck(gx.code == -1,
               "...and the refusal carries status -1, not the 200 the "
               + "server sent: a null body beside a live 200 reads in "
               + "syncjni.c as a bucket that IS empty, and an empty bucket "
               + "is what drives the loop that deletes local rows");
            ck(gx.fail == BoundaryLogic.NET_OTHER,
               "...and is a transport failure, not a timeout");
            ck(bigc.reply.closed,
               "...and the ABANDONED response is closed -- the path that "
               + "leaves the most unread is the path that never reached a "
               + "close written after the loop");
            ck(bigc.reply.served <= ELIM + 1,
               "...having read no more than one byte past the limit");

            /* THE DECLARED-LENGTH REFUSAL, which aborts before a single byte
             * is read and so opens a response it immediately abandons. */
            Conn liec = new Conn();
            liec.lenHeader = "999999";
            BoundaryLogic.Exchange lx = BoundaryLogic.runExchange(
                liec, null, eclk, emono[0], BUDGET, ELIM);
            ck(lx.body == null && lx.code == -1,
               "a reply that DECLARES more than the limit is refused");
            ck(liec.reply != null && liec.reply.closed,
               "...and the response opened to make that decision is closed "
               + "again, unread");
            ck(liec.reply.served == 0, "...without a byte being read");

            /* THE DEADLINE REFUSAL closes it as well. Same shape, different
             * reason, and a close written on only one of the abort paths is
             * exactly the bug. */
            Conn slowc = new Conn();
            BoundaryLogic.Exchange tx = BoundaryLogic.runExchange(
                slowc, null, eclk, emono[0] - 60000000000L, 45000, ELIM);
            ck(tx.body == null && tx.code == -1
               && tx.fail == BoundaryLogic.NET_TIMEOUT,
               "an exchange already past its deadline is refused as a "
               + "timeout");
            ck(slowc.reply != null && slowc.reply.closed,
               "...and its response is closed too");

            /* AND THE NEXT REQUEST STILL WORKS. This is what all the closing
             * is FOR. The pool hands out the same connection to the next
             * bucket in the same sync, and one left holding an unread
             * response poisons it for everything after -- a single oversized
             * reply in the middle of a restore would otherwise take the rest
             * of the restore with it. */
            Pool pool = new Pool();
            Conn first = pool.next();
            first.replyLen = 10L * 1024 * 1024;
            BoundaryLogic.Exchange px = BoundaryLogic.runExchange(
                first, null, eclk, emono[0], BUDGET, ELIM);
            ck(px.body == null, "the flood is refused");
            Conn second = pool.next();   /* THROWS if first was left open */
            BoundaryLogic.Exchange py = BoundaryLogic.runExchange(
                second, null, eclk, emono[0], BUDGET, ELIM);
            ck(py.body != null && py.code == 200,
               "the request AFTER an aborted one still succeeds on the "
               + "pooled connection");
        } catch (java.io.IOException e) {
            throw new AssertionError("no fake connection in this section "
                                     + "does I/O that can fail except the "
                                     + "one that is asserted to", e);
        }

        ck(checks > 0, "this suite ran at least one check");
        System.out.println("boundaryjavatest: " + checks + " checks passed");
    }
}
