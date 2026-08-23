// SPDX-License-Identifier: GPL-3.0
// PancraService.java --- Foreground service keeping BLE alive
// Copyright 2026 Jakob Kastelic

/* Foreground service: keeps the process alive (and BLE-exempt from Doze) so the
 * sensor connection and reading collection continue when the app is not in the
 * foreground -- or has been swiped away entirely, like the official app.
 *
 * It runs no logic itself. All BLE state is static in Ble and lives in this same
 * process; the GATT callbacks arrive on binder threads and keep driving the
 * reconnect/parse path regardless of the Activity's lifecycle. This service
 * exists only to hold foreground priority (with an ongoing notification) so the
 * OS does not kill the process. Started by the activity at launch. */
package com.jk.pancra;

import android.app.AlarmManager;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.graphics.Bitmap;
import android.net.Uri;
import android.os.Build;
import android.os.IBinder;
import android.os.PowerManager;
import android.os.SystemClock;
import android.provider.Settings;
import android.util.Log;

public final class PancraService extends Service {
    /* Channel id v2: importance is immutable once a channel exists, and the
     * original "pancra" channel was IMPORTANCE_LOW -- which Android files
     * under "silent notifications", a class most lock screens HIDE. DEFAULT
     * importance (with sound and vibration explicitly off, so it stays just
     * as quiet) is what makes the value show on the lock screen. */
    private static final String CH = "pancra_glucose";
    private static final String ACTION_WAKE = "com.jk.pancra.WAKE";
    private static final long WAKE_INTERVAL_MS = 5 * 60 * 1000L;   /* ~one sensor cycle */
    private static PowerManager.WakeLock wakelock;

    /* LAST glucose notification content, cached so EVERY (re)post of the
     * foreground notification -- including the wake-alarm restart every cycle --
     * re-shows the value + plot rather than a placeholder. Without this, each
     * onStartCommand stamped the plain "Reading glucose" over the live reading,
     * so the shade showed neither number nor plot until the next reading. */
    /* ONE OBJECT, PUBLISHED THROUGH ONE REFERENCE.
     *
     * As seven separate volatile fields, written one after another by
     * showGlucose and read one after another by buildNotif, each field is
     * individually safe and the SET is not: a build running while a reading
     * lands sees any mix of the two, and two of those mixes are not merely
     * stale but wrong.
     *
     *   - `px` with the OLD `w`/`h`. The guard `px.length >= w * h` is what
     *     stands between this and Bitmap.createBitmap reading past the end of
     *     the array; it passed on a new large plot paired with old small
     *     dimensions -- drawing a corner of the new plot, stretched -- and a
     *     new SMALL plot paired with old LARGE dimensions fails the guard, so
     *     the notification silently loses its plot instead.
     *   - `value` from one reading with `title`/`text` from another: a shade
     *     showing one number in the status bar and a different one in the row.
     *
     * A single volatile reference to an immutable object has neither: the
     * write publishes every field at once, and a reader that captures the
     * reference once cannot see a mixture. VOLATILE IS STILL LOAD-BEARING --
     * it is what makes the constructor's writes visible to the reading thread
     * before the reference is.
     *
     * THE PIXELS ARE COPIED, not referenced. They arrive from native as a
     * jintArray this class does not own; keeping the caller's array would let
     * the plot change under a build that had already checked its length. */
    /* THE CLASS ITSELF IS IN ServicePolicy, where the host JVM can execute it.
     * A copy here would be a second definition of the one rule that stands
     * between a mismatched plot and Bitmap.createBitmap reading past the end of
     * an array -- and the copy in this file is the one no test can reach,
     * because this class needs android.jar. */

    /* Never null, so no reader needs a null check: the initial value is the
     * "before the first reading" state, rather than a null test per field
     * inside buildNotif. */
    private static volatile NotifPolicy.Notif sNotif =
        NotifPolicy.nextNotif(null, null, null, null, true, null, 0, 0);

    /* Hold a partial wakelock so the CPU keeps processing BLE while the screen is
     * off. The foreground service keeps the process alive, but without this the
     * CPU can suspend between the sensor's 5-min cycles and the reconnect stalls.
     * This is what keeps a locked-screen CGM connection alive (small battery cost). */
    private void holdWakelock() {
        try {
            if (wakelock == null) {
                PowerManager pm = getSystemService(PowerManager.class);
                wakelock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "pancra:ble");
                wakelock.setReferenceCounted(false);
            }
            if (!wakelock.isHeld()) wakelock.acquire();
        } catch (Throwable t) { Log.i("pancra", "wakelock: " + t); }
    }

    /* GIVE THE CPU BACK. Its own method because onDestroy is not the only
     * caller: a start that cannot promote the service to the foreground
     * has to release the lock a PREVIOUS start took (the field is static and
     * outlives one onStartCommand), and it has to do it now rather than
     * whenever the looper gets round to destroying the service. A partial
     * wakelock held by a process that is monitoring nothing is a battery
     * drain with no upside, and it is the one cost of monitoring the user can
     * actually see. */
    private void releaseWakelock() {
        try {
            if (wakelock != null && wakelock.isHeld()) wakelock.release();
        } catch (Throwable t) { Log.i("pancra", "wakelock release: " + t); }
    }

    /* called by native (via Ble.startService) at activity create */
    public static void start(Context ctx) {
        try {
            Context app = ctx.getApplicationContext();
            app.startForegroundService(new Intent(app, PancraService.class));
        } catch (Throwable t) { Log.i("pancra", "startService: " + t); }
    }

    /* Ask the user to exempt us from battery optimisation. Without this, Doze can
     * still throttle/kill even a foreground-service process over a long idle
     * night. Shows the system dialog once; a no-op if already exempt. Called from
     * the activity (an Activity context) at launch. */
    public static void requestNoBatteryOpt(Context ctx) {
        try {
            PowerManager pm = ctx.getSystemService(PowerManager.class);
            String pkg = ctx.getPackageName();
            if (pm != null && pm.isIgnoringBatteryOptimizations(pkg)) return;
            Intent i = new Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS,
                                  Uri.parse("package:" + pkg));
            i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            ctx.startActivity(i);
        } catch (Throwable t) { Log.i("pancra", "batteryOpt: " + t); }
    }

    /* Resolve the app's pixel-droplet notification icon at runtime (this build
     * emits no R.java), falling back to a framework icon if it isn't found. */
    private static int notifIcon(Context ctx) {
        int id = ctx.getResources().getIdentifier(
            "ic_notification", "drawable", ctx.getPackageName());
        return id != 0 ? id : android.R.drawable.ic_dialog_info;
    }

    /* Build the ongoing notification. If a glucose reading has been seen, this
     * reconstructs the FULL notification (value + trend in the title, plot as
     * both the collapsed large-icon and the expanded big-picture) from the
     * cache -- so a restart or wake-alarm re-post never downgrades it to a bare
     * placeholder. Before the first reading it shows "Reading glucose". */
    private static Notification buildNotif(Context app) {
        NotificationManager nm = app.getSystemService(NotificationManager.class);
        NotificationChannel ch =
            new NotificationChannel(CH, "Pancra",
                                    NotificationManager.IMPORTANCE_DEFAULT);
        ch.setSound(null, null);   /* DEFAULT importance, but never a peep: */
        ch.enableVibration(false); /* the alarm has its own loud path */
        ch.setShowBadge(false);
        nm.createNotificationChannel(ch);
        nm.deleteNotificationChannel("pancra"); /* retire the LOW-imp. v1 */
        Intent open = new Intent(app, android.app.NativeActivity.class);
        open.setAction(Intent.ACTION_MAIN);
        open.addCategory(Intent.CATEGORY_LAUNCHER);
        open.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        PendingIntent pi = PendingIntent.getActivity(app, 0, open,
            PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
        /* CAPTURED ONCE. Every field below comes from this one object, so a
         * reading landing mid-build cannot contribute half of itself. */
        NotifPolicy.Notif n = sNotif;
        String title = (n.title != null) ? n.title : "Pancra";
        String text  = (n.text  != null) ? n.text  : "Reading glucose";
        Notification.Builder b = new Notification.Builder(app, CH)
            .setContentTitle(title)
            .setContentText(text)
            .setContentIntent(pi)
            .setOngoing(true)
            .setOnlyAlertOnce(true);
        /* Status bar shows the VALUE when fresh; the app glyph otherwise.
         * The pulldown keeps its identity via the large icon / plot. */
        String v = n.value;
        android.graphics.drawable.Icon vi =
            (v != null && !v.isEmpty()) ? valueIcon(v) : null;
        if (vi != null) b.setSmallIcon(vi);
        else b.setSmallIcon(notifIcon(app));
        /* LOCK SCREEN setting: SHOW = full content on the lock screen,
         * HIDE = the notification does not appear there at all. */
        b.setVisibility(n.lock ? Notification.VISIBILITY_PUBLIC
                               : Notification.VISIBILITY_SECRET);
        /* The COLLAPSED row's large icon is the APP'S OWN badge -- the one
         * small-icon slot serves both the status bar and the shade header,
         * so with the value as the small icon this is where the drop
         * identity lives. The plot moved to the EXPANDED view only. */
        int li = app.getResources().getIdentifier(
            "ic_launcher", "mipmap", app.getPackageName());
        if (li != 0)
            b.setLargeIcon(android.graphics.drawable.Icon
                .createWithResource(app, li));
        /* No length test: NotifPolicy.Notif refused any pixels that did not
         * match their dimensions, so hasPlot() IS the guarantee. */
        if (n.hasPlot()) {
            Bitmap bmp = Bitmap.createBitmap(n.px, n.w, n.h,
                                             Bitmap.Config.ARGB_8888);
            b.setStyle(new Notification.BigPictureStyle()
                .bigPicture(bmp).bigLargeIcon((Bitmap) null));
        }
        return b.build();
    }

    /* THE BAREST NOTIFICATION THAT CAN EXIST, for when the rich one cannot.
     *
     * buildNotif does real work that can throw: it renders digits into an icon
     * (valueIcon), looks up resources, and hands a pixel array to
     * Bitmap.createBitmap. startForeground REQUIRES a notification, and the
     * system kills a foreground service that does not present one within a few
     * seconds of being started -- so "the plot could not be drawn" must not
     * become "the service was killed and BLE stopped".
     *
     * The channel is created here too, because a notification on a channel
     * that does not exist is not shown at all, and this path can be the very
     * first one taken.
     *
     * A PLATFORM icon, not notifIcon(app). notifIcon does a resource lookup by
     * name, which is one of the things that can have thrown on the way here --
     * a fallback that repeats the failing step is not a fallback. The framework
     * drawable needs no lookup and is already the right shape for the slot: a
     * status-bar glyph, white on transparency, which is all Android renders a
     * small icon as. It is the wrong PICTURE (a sync arrow, not the droplet),
     * and that is the trade: this notification exists so the service survives,
     * not so it looks right. */
    private static Notification plainNotif(Context app) {
        NotificationManager nm = app.getSystemService(NotificationManager.class);
        NotificationChannel ch =
            new NotificationChannel(CH, "Pancra",
                                    NotificationManager.IMPORTANCE_DEFAULT);
        ch.setSound(null, null);
        ch.enableVibration(false);
        ch.setShowBadge(false);
        nm.createNotificationChannel(ch);
        return new Notification.Builder(app, CH)
            .setContentTitle("Pancra")
            .setContentText("Reading glucose")
            .setSmallIcon(android.R.drawable.stat_notify_sync)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .build();
    }

    /* Whatever it takes to have SOMETHING to hand startForeground -- and null,
     * never a throw, when even the plain one cannot be built.
     *
     * The fallback is not exempt from failing. plainNotif calls
     * getSystemService, createNotificationChannel and Notification.Builder, and
     * the most likely reason buildNotif threw at all -- no NotificationManager
     * in this context, so the first use of it is an NPE -- is a reason plainNotif
     * will throw in exactly the same place. So it gets its OWN try, and the
     * hopeless case answers null.
     *
     * That matters because of where this is called from. build() is deliberately
     * OUTSIDE onStartCommand's try (see there), so a throw escaping here escapes
     * onStartCommand -- and an exception out of a Service lifecycle method is an
     * uncaught exception on the main thread, i.e. the process dies immediately.
     * That process holds the CGM connection and the alarm. Trading "the plot
     * could not be drawn, so the service was killed" for "the plot could not be
     * drawn, so the app crashed" is not a trade worth making.
     *
     * Returning null instead lets the REST of onStartCommand run, which is the
     * part that still has something useful to do: the recreation check, the
     * MONITORING STOPPED warning, and the stopSelf that discharges the
     * startForegroundService obligation without a system-thrown kill. */
    private Notification build() {
        try {
            return buildNotif(this);
        } catch (Throwable t) {
            Log.i("pancra", "buildNotif: " + t + " -- posting the plain one");
        }
        try {
            return plainNotif(this);
        } catch (Throwable t) {
            Log.i("pancra", "plainNotif: " + t + " -- nothing to post");
        }
        return null;
    }

    /* Update the ongoing (foreground-service) notification with the live glucose
     * value + trend and a small 3H plot bitmap (px = ARGB int[w*h]). Called from
     * native on each new reading. Caches the content so subsequent restarts /
     * wakes re-post the SAME rich notification (see buildNotif); uses id 1 so it
     * refreshes the FGS notification, setOnlyAlertOnce so refreshes never buzz. */
    public static void showGlucose(Context ctx, String title, String text,
                                   String value, int[] px, int w, int h,
                                   int lockscr) {
        try {
            Context app = ctx.getApplicationContext();
            /* ONE PUBLISH. The previous plot is carried forward when this
             * call brings none -- the same rule the separate fields had, now
             * stated where the object is built rather than by declining to
             * assign three of seven fields. */
            /* Read-modify-write, and deliberately not made atomic -- because
             * THERE IS ONLY EVER ONE WRITER IN HERE.
             *
             * Every call arrives from notify_update() in app/notify.c, which is
             * entered under a single-flight guard (flight_enter). Two threads do
             * reach it -- the activity's 1 Hz timer and the service's tick
             * thread -- and the second one to arrive returns immediately rather
             * than waiting, so two showGlucose calls never overlap and this
             * read-modify-write is serialised by construction. Nothing else in
             * this process writes sNotif.
             *
             * The guard is in native code this class cannot see, so here is
             * what it costs if it ever goes away: two concurrent calls could
             * lose one call's carry-forward of the previous plot -- the newer
             * snapshot wins and one plot is momentarily absent from the shade
             * until the next reading. What still CANNOT happen, which is the
             * point, is a reader seeing a MIXTURE: every snapshot is whole
             * before it is published. That is the property worth having, and a
             * compare-and-set would only buy back one cycle of one plot at the
             * cost of a loop in the reading path. */
            sNotif = NotifPolicy.nextNotif(sNotif, title, text, value,
                                             lockscr != 0, px, w, h);
            NotificationManager nm = app.getSystemService(NotificationManager.class);
            nm.notify(NotifPolicy.NOTIF_SERVICE, buildNotif(app));
        } catch (Throwable t) { Log.i("pancra", "showGlucose: " + t); }
    }

    /* The STATUS BAR icon is the value itself: Android renders small icons as
     * alpha-only silhouettes, so bold white digits on transparency show up as
     * a legible number in the top row (the xDrip/Juggluco trick). Returns null
     * on any failure so the caller falls back to the app glyph -- which is
     * also what a STALE reading gets (value == ""), matching the app's rule
     * that stale data blanks rather than lies. */
    private static android.graphics.drawable.Icon valueIcon(String v) {
        try {
            final int S = 96;
            Bitmap b = Bitmap.createBitmap(S, S, Bitmap.Config.ARGB_8888);
            android.graphics.Canvas c = new android.graphics.Canvas(b);
            android.graphics.Paint p =
                new android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG);
            p.setColor(0xFFFFFFFF);
            /* CONDENSED bold: designed-narrow digits look like the clock
             * where artificially squeezed regular ones look distorted.
             * The icon SLOT is a fixed ~25px square, so the clock's full
             * 3-digit width (~39px) is unreachable; the tuning target is
             * the clock's exact glyph HEIGHT (measured: cap 18px of the
             * 25px slot = 72% of the canvas). */
            p.setTypeface(android.graphics.Typeface.create(
                "sans-serif-condensed",
                android.graphics.Typeface.BOLD));
            p.setTextAlign(android.graphics.Paint.Align.CENTER);
            float ts = 100f; /* cap height ~0.71em -> ~71px = clock's 72% */
            p.setTextSize(ts);
            float tw = p.measureText(v);
            if (tw > S) {
                float sx = S / tw;
                if (sx < 0.6f) sx = 0.6f;
                p.setTextScaleX(sx);
                tw = p.measureText(v);
                if (tw > S) p.setTextSize(ts * S / tw);
            }
            android.graphics.Paint.FontMetrics fm = p.getFontMetrics();
            c.drawText(v, S / 2f, (S - fm.ascent - fm.descent) / 2f, p);
            return android.graphics.drawable.Icon.createWithBitmap(b);
        } catch (Throwable t) { return null; }
    }

    /* Re-arm a periodic wake. Stelo disconnects after every reading, so between
     * cycles reconnection depends on our process getting CPU; in deep Doze the
     * process is frozen and queued BLE callbacks (the auto-reconnect) don't get
     * delivered. This alarm briefly unfreezes us each cycle so those callbacks
     * flush and the EXISTING reconnect path runs -- it does not touch BLE itself.
     * setAndAllowWhileIdle needs no special permission and fires during Doze. */
    private void scheduleWake() {
        try {
            AlarmManager am = getSystemService(AlarmManager.class);
            if (am == null) return;
            Intent i = new Intent(this, PancraService.class).setAction(ACTION_WAKE);
            PendingIntent pi = PendingIntent.getForegroundService(this, 1, i,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
            am.setAndAllowWhileIdle(AlarmManager.ELAPSED_REALTIME_WAKEUP,
                SystemClock.elapsedRealtime() + WAKE_INTERVAL_MS, pi);
        } catch (Throwable t) { Log.i("pancra", "scheduleWake: " + t); }
    }

    /* Drop the periodic wake. Only for the giving-up path below: scheduleWake()
     * re-arms on EVERY start, and the alarm outlives the process, so a service
     * that stops because it has no native code would otherwise be restarted by
     * its own alarm five minutes later -- foreground notification, wakelock,
     * one failed tick, stop, repeat forever. Opening the activity re-arms it,
     * which is exactly when it becomes useful again. */
    private void cancelWake() {
        try {
            AlarmManager am = getSystemService(AlarmManager.class);
            if (am == null) return;
            Intent i = new Intent(this, PancraService.class).setAction(ACTION_WAKE);
            PendingIntent pi = PendingIntent.getForegroundService(this, 1, i,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
            am.cancel(pi);
        } catch (Throwable t) { Log.i("pancra", "cancelWake: " + t); }
    }

    @Override public int onStartCommand(Intent i, int flags, int startId) {
        /* startForeground FIRST, and it must happen.
         *
         * The notification is built OUTSIDE the try that guards
         * startForeground, through build(), which already falls back to the
         * plain notification rather than propagating. With the whole thing
         * in one try, a throw from buildNotif -- rendering the value icon, or
         * a pixel array the plot guard let through -- is caught and logged,
         * and startForeground is NEVER REACHED. Android then kills a
         * foreground service that has not presented its notification within
         * seconds, so a failure to draw a plot took BLE down with it, and the
         * log line said "buildNotif" while the visible symptom was that the
         * app stopped reading.
         *
         * build() answers null rather than throwing when NOTHING could be built
         * -- not even the plain notification -- because a throw here would leave
         * onStartCommand through a Service lifecycle method and kill the process
         * outright. There is no notification to post in that case and so no way
         * to be a foreground service; what is left is to say so, which is now
         * exactly what happens: `first == null` is one of the two ways this
         * start fails to become a foreground service, and both go down the
         * same path below. */
        Notification first = build();
        /* WHAT THE PROMOTION DID, KEPT rather than logged and dropped.
         *
         * Ending the story in this catch -- write the throwable to the log
         * and carry straight on to take a wakelock, start the heartbeat,
         * re-arm the wake alarm and return START_STICKY -- performs the full
         * set of gestures that mean "monitoring is protected" on a service
         * the OS is about to stop for never having presented a notification.
         * Nothing on the phone says so. The user gets a warm battery and no
         * readings.
         *
         * The throwable is the evidence for WHICH failure this is (see
         * ServicePolicy.promotionOutcome), so it is kept. */
        Throwable promoErr = null;
        try {
            if (first == null)
                Log.i("pancra", "startForeground: nothing could be built");
            else if (Build.VERSION.SDK_INT >= 29)
                startForeground(NotifPolicy.NOTIF_SERVICE, first,
                                ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE);
            else
                startForeground(NotifPolicy.NOTIF_SERVICE, first);
        } catch (Throwable t) {
            promoErr = t;
            Log.i("pancra", "startForeground: " + t);
        }
        int promo = ServicePolicy.promotionOutcome(first != null, promoErr);
        if (promo != ServicePolicy.PROMO_OK)
            Log.i("pancra", "startForeground: REFUSED (kind " + promo
                            + ") -- releasing and stopping");
        /* i == null means the system restarted us on its own (START_STICKY)
         * with no activity -- BLE and the reading log live in the activity's
         * native lib, so this process cannot monitor anything. It must not
         * pretend to (a notification and a wakelock, reading nothing).
         *
         * But it must not vanish either. The foreground notification goes
         * with the service, and a phone with no pancra notification is exactly
         * what a healthy phone looks like when the app is not in the shade --
         * so a user whose device reclaimed the app would have their glucose
         * stop being watched with nothing anywhere to say so. Alarms
         * included.
         *
         * The decision itself is in ServicePolicy, where it is tested on the
         * host without a phone. */
        int act = ServicePolicy.serviceAction(i != null, sNativeReady, false);
        /* WHICH SENTENCE THE NOTICE CARRIES. The two ways monitoring stops
         * are one fact to the user -- the app is not watching any more -- and
         * one notification (NOTIF_STOPPED), but they are not the same
         * sentence: "Android restarted pancra without the app" is a lie about
         * a phone that refused the foreground notification, and a user who is
         * told the wrong thing tries the wrong fix. */
        final boolean promoFailed = promo != ServicePolicy.PROMO_OK;
        /* EVERY EFFECT onStartCommand HAS, handed to the policy as calls it
         * can make. The sequence -- and above all the fact that the failing
         * paths release the wakelock and post the notice before stopping --
         * is asserted on the host by boundaryjavatest, which is the only
         * place a REFUSED promotion can be arranged at all. */
        ServicePolicy.ServiceOps ops = new ServicePolicy.ServiceOps() {
            @Override public void holdWakelock() { PancraService.this.holdWakelock(); }
            @Override public void releaseWakelock() { PancraService.this.releaseWakelock(); }
            @Override public void startTicking() { PancraService.this.startTicking(); }
            @Override public void scheduleWake() { PancraService.this.scheduleWake(); }
            @Override public void cancelWake() { PancraService.this.cancelWake(); }
            @Override public void warnStopped() {
                PancraService.this.warnStopped(
                    promoFailed ? STOP_PROMO_SHORT : STOP_RECREATED_SHORT,
                    promoFailed ? STOP_PROMO_LONG : STOP_RECREATED_LONG);
            }
            @Override public void stopSelf() { PancraService.this.stopSelf(); }
        };
        /* START_NOT_STICKY on every failing path: sticky means "restart me
         * the same way", and the same way is what just failed. Where a retry
         * can help, the wake alarm is the retry -- serviceStart keeps it armed
         * for a recoverable refusal and cancels it for a terminal one, so a
         * manifest fault cannot become a five-minute loop for the life of the
         * install. */
        return ServicePolicy.serviceStart(act, promo, ops)
            ? START_STICKY : START_NOT_STICKY;
    }

    /* MONITORING HAS STOPPED, AND THE USER HAS TO KNOW.
     *
     * Its own notification id, because the foreground one (id 1) disappears
     * with the service. Ongoing and DEFAULT importance so it does not sit
     * silently at the bottom of the shade: this says the app has stopped
     * watching, which is exactly the thing a person relies on it for. Tapping
     * it opens the app, which is also what fixes it. */
    /* Its OWN id, from NotifPolicy's list. Sharing 2 with Alarm.java's
     * sounding glucose alarm would let each notice replace the other, and
     * either cancel take down the wrong one. */
    private static final int WARN_ID = NotifPolicy.NOTIF_STOPPED;

    /* THE TWO WAYS MONITORING STOPS, in the words that lead to the right fix.
     * One notification and one id, because to the person holding the phone it
     * is one fact; two texts, because "Android restarted pancra without the
     * app" describes only the first of them and would send someone chasing a
     * memory kill that never happened. */
    private static final String STOP_RECREATED_SHORT =
        "Android restarted pancra without the app. Open it to resume.";
    private static final String STOP_RECREATED_LONG =
        "Android restarted pancra in the background, where it cannot reach "
        + "the sensor. No readings are being taken and no alarms will "
        + "sound. Open the app to resume monitoring.";
    private static final String STOP_PROMO_SHORT =
        "Android refused pancra's ongoing notification. Open it to resume.";
    private static final String STOP_PROMO_LONG =
        "Android would not let pancra show the ongoing notification a "
        + "background app needs, so it has stopped rather than hold your "
        + "phone awake while watching nothing. No readings are being taken "
        + "and no alarms will sound. Open the app to resume monitoring.";

    private void warnStopped(String shortText, String longText) {
        try {
            Context app = getApplicationContext();
            NotificationManager nm = app.getSystemService(NotificationManager.class);
            NotificationChannel ch =
                new NotificationChannel(CH, "Pancra",
                                        NotificationManager.IMPORTANCE_DEFAULT);
            ch.setShowBadge(true);
            nm.createNotificationChannel(ch);
            Intent open = new Intent(app, android.app.NativeActivity.class);
            open.setAction(Intent.ACTION_MAIN);
            open.addCategory(Intent.CATEGORY_LAUNCHER);
            open.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP);
            PendingIntent pi = PendingIntent.getActivity(app, 0, open,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
            Notification n = new Notification.Builder(app, CH)
                .setSmallIcon(notifIcon(app))
                .setContentTitle("MONITORING STOPPED")
                .setContentText(shortText)
                .setStyle(new Notification.BigTextStyle().bigText(longText))
                .setContentIntent(pi)
                .setOngoing(true)
                .setAutoCancel(false)
                .build();
            nm.notify(WARN_ID, n);
        } catch (Throwable t) { Log.i("pancra", "warnStopped: " + t); }
    }

    /* Set once the activity's native side is up (Ble.nativeReady), so a
     * service recreated on its own can tell the difference between "the app
     * is running and asked for me" and "I am alone in a fresh process". */
    private static volatile boolean sNativeReady;

    public static void nativeReady() { sNativeReady = true; }

    /* The app is open again, so the warning is stale. Cleared explicitly
     * rather than left to the user: a MONITORING STOPPED notice that outlives
     * the condition trains people to ignore the one that does not. */
    public static void clearStoppedWarning(Context ctx) {
        try {
            ctx.getApplicationContext()
               .getSystemService(NotificationManager.class)
               .cancel(WARN_ID);
        } catch (Throwable t) { Log.i("pancra", "clearStoppedWarning: " + t); }
    }

    /* When the user swipes the task away, keep running: re-arm the service so the
     * process (and thus the live BLE connection) is not torn down. */
    @Override public void onTaskRemoved(Intent rootIntent) {
        try {
            Intent restart = new Intent(getApplicationContext(), PancraService.class);
            getApplicationContext().startForegroundService(restart);
        } catch (Throwable t) { Log.i("pancra", "onTaskRemoved: " + t); }
    }

    /* Service-owned heartbeat.
     *
     * Evaluated only on the activity's 1 Hz looper timer, which onDestroy
     * tears down, the alarm stops after a back-press or a task swipe -- with
     * this service still holding the BLE connection alive for days, a hypo is
     * decoded and logged but never sounded, and an alarm already ringing can
     * never be silenced. The heartbeat has to belong to whatever outlives the
     * activity, which is this service.
     *
     * 20 s is well inside the shortest DISCONNECT threshold (15 min) and costs
     * nothing: it touches no radio, only re-evaluates state already in memory. */
    private static final int TICK_MS = 20000;
    /* volatile: written on the main thread (onDestroy), read on the tick
     * thread. Without it the tick thread may never observe the null. */
    private volatile android.os.Handler tick;
    private final Runnable ticker = new Runnable() {
        @Override public void run() {
            /* EVERYTHING inside the try. A reschedule outside it, reading
             * `tick` twice, lets onDestroy null the field between the check
             * and the postDelayed, and that NPE escapes run() inside a
             * Looper dispatch -> uncaught handler -> the PROCESS IS KILLED.
             * That process holds the CGM connection and the alarm, so a service
             * teardown would take the alarm down with it. */
            /* The reschedule is in a FINALLY, not merely inside the try.
             *
             * Inside the try it is skipped whenever Ble.onTick() throws,
             * and one throw is permanent: startTicking() returns early while
             * `tick` is non-null, so nothing re-arms the heartbeat. The
             * service then keeps its notification and wakelock while
             * evaluating no alarm at all -- with the activity gone, that is
             * the exact hypo-decoded-but-never-sounded case this heartbeat
             * exists to prevent. UnsatisfiedLinkError from a native-less
             * process (see onStartCommand) is the realistic first throw.
             *
             * `tick` is still read ONCE, which is what keeps onDestroy's null
             * from escaping run() as an NPE and killing the process. */
            boolean noNative = false;
            try {
                Ble.onTick();
            } catch (UnsatisfiedLinkError e) {
                /* libpancra is loaded by the NativeActivity's android.app.lib_name
                 * and by nothing else, so in a process started WITHOUT the
                 * activity there is no native code to tick. Such a service can
                 * never raise or silence an alarm; the honest move is the same
                 * one the i == null branch makes -- stop, rather than sit there
                 * showing a "Reading glucose" notification and holding a
                 * wakelock while monitoring nothing. */
                noNative = true;
                Log.i("pancra", "tick: no native in this process, stopping");
            } catch (Throwable t) {
                Log.i("pancra", "tick: " + t);
            } finally {
                if (noNative) {
                    cancelWake();   /* or its own alarm restarts this in 5 min */
                    stopSelf();
                } else {
                    android.os.Handler h = tick;   /* read once */
                    if (h != null) h.postDelayed(this, TICK_MS);
                }
            }
        }
    };

    private android.os.HandlerThread tickThread;

    private void startTicking() {
        if (tick != null) return;
        /* A DEDICATED thread, not the main looper.
         *
         * The tick calls into native, which can raise an alarm -- and that path
         * holds a no-timeout spin lock across MediaPlayer setDataSource/prepare/
         * start, hundreds of milliseconds of media-server IPC. On the main looper
         * that stalls every other main-thread callback, and if a BLE thread is
         * already inside that critical section the main looper SPINS waiting for
         * it. An ANR here would kill the process holding the CGM connection,
         * i.e. the alarm itself. */
        tickThread = new android.os.HandlerThread("pancra-tick");
        tickThread.start();
        tick = new android.os.Handler(tickThread.getLooper());
        tick.postDelayed(ticker, TICK_MS);
    }

    @Override public void onDestroy() {
        if (tick != null) { tick.removeCallbacks(ticker); tick = null; }
        if (tickThread != null) { tickThread.quitSafely(); tickThread = null; }
        /* Release the partial wakelock. It is acquired in holdWakelock() and
         * nothing released it, so after a stopSelf() or a system-initiated
         * destroy the process held the CPU awake indefinitely while doing no
         * BLE work at all -- a battery drain with no upside. */
        releaseWakelock();
        super.onDestroy();
    }

    @Override public IBinder onBind(Intent i) { return null; }
}
