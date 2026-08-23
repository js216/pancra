// SPDX-License-Identifier: GPL-3.0
package com.jk.pancra;

/* ANDROID-FREE POLICY FOR THE FOREGROUND SERVICE: whether it may run, what
 * to do when it may not, and how a promotion to foreground can fail (item
 * 263; see ScanPolicy for why these are separate classes).
 *
 * `ScanPolicy.Attempt` lives in ScanPolicy and is shared: it is "a thing that may throw",
 * which both capabilities need and neither owns. */
final class ServicePolicy {

    /* THE MEDIA PLAYER, as the alarm's silence path uses it.
     *
     * An interface rather than the real MediaPlayer so the policy below is
     * executable on a host JVM. The policy is the whole point: it is what
     * stands between a user and a LOOPING alarm-usage player that nothing can
     * silence. Guaranteed only by javacheck grepping Alarm.java for the word
     * "release", it is satisfied by a dead call, a call in the wrong order,
     * or a call skipped by an earlier throw. */
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

    static boolean[] runIndependent(ScanPolicy.Attempt... stages) {
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
     * Stopping quietly is the wrong answer: the foreground notification
     * disappears with the service, so a user whose phone reclaimed the app
     * sees exactly what they would see if everything
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
     * PancraNet.syncHttp catches a Throwable, and one answer -- -1, "it did
     * not work" -- makes every screen say SYNC FAILED for a
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
}
