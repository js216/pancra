// SPDX-License-Identifier: GPL-3.0
// PancraPlatform.java --- the platform policies a CGM has to argue with
// Copyright 2026 Jakob Kastelic

/* THE PLATFORM-POLICY ADAPTER: orientation, permissions, the battery and
 * background-execution rules, the foreground service, and the ongoing
 * notification.
 *
 * These are the calls that decide whether this app is ALLOWED TO KEEP
 * RUNNING, which for something reading a sensor on somebody's arm is not a
 * settings-screen nicety -- a phone that has quietly moved the app into a
 * restricted standby bucket stops delivering readings, and the only symptom
 * is a screen that has not changed for an hour. They were in Ble.java among
 * the GATT callbacks; they have nothing to do with GATT, and a reader looking
 * for "why did it stop in the background" should not have to know the BLE
 * file to find them.
 *
 * Every method answers rather than throws: a platform that refuses one of
 * these (or does not have it at all) must degrade the feature, never take
 * down the process that is watching a glucose level. */
package com.jk.pancra;

import android.app.Activity;
import android.content.Context;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import android.util.Log;

public final class PancraPlatform {
    private static final String TAG = "pancra";

    private PancraPlatform() { }

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

    /* ---- STEP COUNTING ----
     *
     * TYPE_STEP_COUNTER is a HARDWARE counter maintained on the sensor hub:
     * it keeps counting while the phone sleeps and the application processor
     * is never woken to look at an accelerometer. All this class does is hold
     * the newest value it has been handed; native differences two of them a
     * window apart (see steps.h).
     *
     * BATCHED BY FIVE MINUTES. The third argument to registerListener is a
     * max-report LATENCY: the hub accumulates events and delivers them in one
     * batch at most that often, so a walk costs a handful of wakeups an hour
     * instead of one per step. The delay argument is only a hint about
     * sampling rate and is the slowest on offer.
     *
     * The listener is registered ONCE while the feature is on and torn down
     * when it is off -- there is nothing to poll, and polling a counter that
     * is already free would cost more than reading it does. */
    private static android.hardware.SensorManager stepMgr;
    private static android.hardware.SensorEventListener stepLis;
    private static volatile long stepCum = -1;

    public static void stepsListen(Context ctx, boolean on) {
        try {
            if (!on) {
                if (stepMgr != null && stepLis != null)
                    stepMgr.unregisterListener(stepLis);
                stepLis = null;
                /* NOT reset to -1: the count is still valid, and clearing it
                 * would make the next enable throw away one window for no
                 * reason. The counter is the hardware's; we only cached it. */
                return;
            }
            if (stepLis != null) return;   /* already listening */
            stepMgr = ctx.getSystemService(android.hardware.SensorManager.class);
            if (stepMgr == null) return;
            android.hardware.Sensor s = stepMgr.getDefaultSensor(
                android.hardware.Sensor.TYPE_STEP_COUNTER);
            if (s == null) return;         /* no such hardware: stays at -1 */
            android.hardware.SensorEventListener l =
                new android.hardware.SensorEventListener() {
                    public void onSensorChanged(android.hardware.SensorEvent e) {
                        if (e.values != null && e.values.length > 0)
                            stepCum = (long) e.values[0];
                    }
                    public void onAccuracyChanged(android.hardware.Sensor s, int a) { }
                };
            /* REMEMBERED ONLY IF IT TOOK. registerListener REFUSES by
             * returning false -- it does not throw -- and the commonest
             * refusal is the one that happens on the very first call: the
             * feature is switched on, this runs, and the permission dialog it
             * triggered has not been answered yet. Storing the listener
             * regardless made the "already listening" guard above permanent,
             * so granting the permission changed nothing until the app was
             * restarted. */
            if (!stepMgr.registerListener(l, s,
                                          android.hardware.SensorManager.SENSOR_DELAY_NORMAL,
                                          5 * 60 * 1000000))
                return;
            stepLis = l;
        } catch (Throwable t) { Log.i(TAG, "steps: " + t); stepLis = null; }
    }

    /* The counter's newest total, or -1 when nothing has arrived: no sensor,
     * no permission, or simply not moved since the listener came up. */
    public static long stepsCount() { return stepCum; }

    /* Push the live glucose + a 3H plot bitmap into the ongoing notification
     * (shown on the lock screen / shade). Called from native each reading. */
    public static void showGlucose(Context ctx, String title, String text,
                                   String value,
                                   int[] px, int w, int h, int lockscr) {
        PancraService.showGlucose(ctx, title, text, value, px, w, h, lockscr);
    }
}
