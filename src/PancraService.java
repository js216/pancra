// SPDX-License-Identifier: GPL-3.0
// PancraService.java --- Foreground service keeping BLE alive
// Copyright 2026 Jakob Kastelic

/* Foreground service: keeps the process alive (and BLE-exempt from Doze) so the
 * sensor connection and reading collection continue when the app is not in the
 * foreground — or has been swiped away entirely, like the official app.
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
     * re-shows the value + plot instead of a placeholder. Without this, each
     * onStartCommand stamped the plain "Reading glucose" over the live reading,
     * so the shade showed neither number nor plot until the next reading. */
    private static volatile String sTitle, sText;
    private static volatile String sVal; /* fresh display value, "" = stale */
    private static volatile boolean sLock = true; /* visible on lock screen */
    private static volatile int[] sPx;
    private static volatile int sW, sH;

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
        String title = (sTitle != null) ? sTitle : "Pancra";
        String text  = (sText  != null) ? sText  : "Reading glucose";
        Notification.Builder b = new Notification.Builder(app, CH)
            .setContentTitle(title)
            .setContentText(text)
            .setContentIntent(pi)
            .setOngoing(true)
            .setOnlyAlertOnce(true);
        /* Status bar shows the VALUE when fresh; the app glyph otherwise.
         * The pulldown keeps its identity via the large icon / plot. */
        String v = sVal;
        android.graphics.drawable.Icon vi =
            (v != null && !v.isEmpty()) ? valueIcon(v) : null;
        if (vi != null) b.setSmallIcon(vi);
        else b.setSmallIcon(notifIcon(app));
        /* LOCK SCREEN setting: SHOW = full content on the lock screen,
         * HIDE = the notification does not appear there at all. */
        b.setVisibility(sLock ? Notification.VISIBILITY_PUBLIC
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
        int[] px = sPx; int w = sW; int h = sH;
        if (px != null && w > 0 && h > 0 && px.length >= w * h) {
            Bitmap bmp = Bitmap.createBitmap(px, w, h, Bitmap.Config.ARGB_8888);
            b.setStyle(new Notification.BigPictureStyle()
                .bigPicture(bmp).bigLargeIcon((Bitmap) null));
        }
        return b.build();
    }

    private Notification build() {
        return buildNotif(this);
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
            sTitle = title; sText = text; sVal = value; sLock = lockscr != 0;
            if (px != null && w > 0 && h > 0 && px.length >= w * h) {
                sPx = px; sW = w; sH = h;
            }
            NotificationManager nm = app.getSystemService(NotificationManager.class);
            nm.notify(1, buildNotif(app));
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
     * flush and the EXISTING reconnect path runs — it does not touch BLE itself.
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
        try {
            if (Build.VERSION.SDK_INT >= 29)
                startForeground(1, build(), ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE);
            else
                startForeground(1, build());
        } catch (Throwable t) { Log.i("pancra", "startForeground: " + t); }
        /* i == null means the system restarted us on its own (START_STICKY) with no
         * activity — BLE lives in the activity's native lib, so this would be a
         * zombie (notification + wakelock, reading nothing). Don't auto-restart:
         * stop cleanly so the app is simply gone until reopened, never a zombie. */
        if (i == null) { stopSelf(); return START_NOT_STICKY; }
        holdWakelock();          /* keep the CPU processing BLE while the screen is off */
        startTicking();          /* alarms must not depend on a live activity */
        scheduleWake();          /* re-arm each time, including on each wake tick */
        return START_STICKY;     /* restart if the system kills us while running */
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
     * The alarm used to be evaluated only on the activity's 1 Hz looper timer,
     * which onDestroy tears down -- so after a back-press or a task swipe, with
     * this service still holding the BLE connection alive for days, a hypo was
     * decoded and logged but never sounded, and an alarm already ringing could
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
            /* EVERYTHING inside the try. The reschedule used to sit outside it
             * and read `tick` twice: onDestroy can null the field between the
             * check and the postDelayed, and that NPE escapes run() inside a
             * Looper dispatch -> uncaught handler -> the PROCESS IS KILLED.
             * That process holds the CGM connection and the alarm, so a service
             * teardown could take the alarm down with it. */
            /* The reschedule is in a FINALLY, not merely inside the try.
             *
             * Inside the try it was skipped whenever Ble.onTick() threw, and
             * one throw was permanent: startTicking() returns early while
             * `tick` is non-null, so nothing ever re-armed the heartbeat. The
             * service then kept its notification and wakelock while no longer
             * evaluating the alarm at all -- with the activity gone, that is
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
        try {
            if (wakelock != null && wakelock.isHeld()) wakelock.release();
        } catch (Throwable t) { Log.i("pancra", "wakelock release: " + t); }
        super.onDestroy();
    }

    @Override public IBinder onBind(Intent i) { return null; }
}
