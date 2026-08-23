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

    /* Push the live glucose + a 3H plot bitmap into the ongoing notification
     * (shown on the lock screen / shade). Called from native each reading. */
    public static void showGlucose(Context ctx, String title, String text,
                                   String value,
                                   int[] px, int w, int h, int lockscr) {
        PancraService.showGlucose(ctx, title, text, value, px, w, h, lockscr);
    }
}
