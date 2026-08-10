// SPDX-License-Identifier: GPL-3.0
// Alarm.java --- Audible + vibrating glucose alarm
// Copyright 2026 Jakob Kastelic

/* Audible + vibrating glucose alarm. Kept separate from the BLE pipe: native
 * code (main.c) decides WHEN to alarm (edge-triggered on the transition into an
 * out-of-range reading) and calls trigger()/silence() via dexble.c.
 *
 * The alert loops (sound + vibration) until silenced, so a single missed beep
 * can't be the difference -- silence() stops everything at once. We play the
 * sound ourselves (looping MediaPlayer, USAGE_ALARM) rather than via the
 * notification channel, because channel sounds play once and can't be stopped
 * on demand; the notification is kept silent and used only for the heads-up
 * banner when the app isn't in front. The tone is the device's Default alarm
 * sound (Settings > Sound > Default alarm sound). */
package com.jk.pancra;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.media.AudioAttributes;
import android.media.MediaPlayer;
import android.media.RingtoneManager;
import android.net.Uri;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.util.Log;

public final class Alarm {
    private static final String CH = "pancra-alarm";
    private static final int NID = 2;              /* distinct from the service's id 1 */
    private static MediaPlayer player;
    /* trigger() runs on a BLE binder thread, silence() on the main looper.
     * Unsynchronized, silence() could read `player` as null (the binder thread
     * had not assigned it yet), do nothing, and return -- and the binder thread
     * would then start a LOOPING alarm-usage MediaPlayer that nothing in the
     * app could ever stop, because the C side already considers the alarm
     * silenced and issues no further silence(). The mirror ordering released
     * the player between prepare() and start(), throwing IllegalStateException
     * into trigger()'s catch-all: a silently missing hypo alarm. Both are
     * unacceptable for an alarm, so trigger() and silence() are both
     * synchronized on the Alarm class monitor. They never call back into
     * native code, so this monitor is a leaf and cannot deadlock against the
     * C-side alarm_lock that surrounds these calls. */

    /* A single short beep for the NEW DATAPOINT alert -- distinct from the
     * looping glucose alarm. A ToneGenerator is kept alive and reused so each
     * datapoint just fires a brief tone; nothing to silence. Called on a BLE
     * binder thread, so it must not block. */
    private static android.media.ToneGenerator toneGen;
    public static synchronized void beep(Context ctx) {
        try {
            /* Media stream at full tone volume so it is actually audible; a
             * fresh generator each time avoids a stale one going silent. */
            if (toneGen != null) { try { toneGen.release(); } catch (Throwable t) {} }
            toneGen = new android.media.ToneGenerator(
                android.media.AudioManager.STREAM_MUSIC, 100);
            boolean ok = toneGen.startTone(
                android.media.ToneGenerator.TONE_PROP_BEEP, 200);
            Log.i("pancra", "beep startTone=" + ok);
        } catch (Throwable t) { Log.i("pancra", "beep: " + t); }
    }

    /* CHIRP: the beep's duration and starting pitch, bent by st10 TENTHS of a
     * semitone over the tone (up for a rise, down for a fall). ToneGenerator
     * cannot bend pitch, so the waveform is synthesised and handed to a
     * one-shot AudioTrack.
     *
     * The phase is integrated rather than computed per sample from a single
     * frequency: sin(2*pi*f(t)*t) with a moving f is NOT a glide, it sweeps at
     * twice the intended rate and lands on the wrong note. Accumulating
     * phase += 2*pi*f(t)/rate is the actual definition of an instantaneous
     * frequency, so the tone ends exactly CHIRP_MAX_ST semitones away at the
     * cap. A short raised-cosine fade on each end keeps the start and stop
     * from clicking. Called on a BLE binder thread, so nothing here blocks:
     * AudioTrack.write on a static buffer just copies. */
    private static final int CHIRP_MS = 200;      /* == the beep's duration */
    private static final double CHIRP_HZ = 1200;  /* == TONE_PROP_BEEP's pitch */
    /* Trailing digital silence, and the reason it exists: the marker fires
     * when the frame has been handed to the mixer, NOT when the speaker has
     * finished with it, so releasing the track there chopped the last few
     * milliseconds off mid-render -- an abrupt truncation, which is audible
     * as a click at the end of every chirp. The fade already takes the
     * waveform to near-zero, so the click was never the waveform; it was the
     * cut. Padding means whatever the release truncates is silence. */
    private static final int CHIRP_PAD_MS = 40;
    /* The chirp currently playing (or the last one). Strongly held so the GC
     * cannot finalize a track mid-note; released when the next chirp starts. */
    private static android.media.AudioTrack chirpTrack;

    /* Build a one-shot AudioTrack over `pcm`, start it, and return it so the
     * caller can park it in a static field.
     *
     * Shared by chirp() and nudge() because it carries two lessons that cost
     * real debugging and must not be re-learned per call site:
     *
     * (1) THE CALLER MUST HOLD THE RETURNED TRACK IN A STATIC FIELD. The
     *     framework keeps only a WeakReference, so a local is not enough: a GC
     *     during playback can finalize the track mid-note.
     * (2) NOTHING IS RELEASED HERE. Releasing at a write/marker boundary cuts
     *     the tail by the output latency -- a few ms on the speaker, but
     *     150-250 ms over A2DP, enough to swallow a whole short motif. Each
     *     caller releases its PREVIOUS track when the next one starts, so
     *     playback is never interrupted and at most one track is alive.
     *
     * USAGE_MEDIA, i.e. STREAM_MUSIC -- the SAME stream the beep's
     * ToneGenerator uses, and the reason the beep was audible when the first
     * chirp was not. USAGE_ASSISTANCE_SONIFICATION routes to STREAM_SYSTEM,
     * which is aliased to STREAM_RING and therefore muted outright whenever
     * the phone is on silent or vibrate. These alerts are ambient companions
     * to the reading, not ringer events. (The glucose alarm is different and
     * stays on USAGE_ALARM, which silent mode does not touch.) */
    private static android.media.AudioTrack playPcm(short[] pcm, int rate, String what) {
        android.media.AudioTrack tr = new android.media.AudioTrack.Builder()
            .setAudioAttributes(new AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_MEDIA)
                .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION).build())
            .setAudioFormat(new android.media.AudioFormat.Builder()
                .setEncoding(android.media.AudioFormat.ENCODING_PCM_16BIT)
                .setSampleRate(rate)
                .setChannelMask(android.media.AudioFormat.CHANNEL_OUT_MONO).build())
            .setBufferSizeInBytes(pcm.length * 2)
            .setTransferMode(android.media.AudioTrack.MODE_STATIC)
            .build();
        int wrote = tr.write(pcm, 0, pcm.length);
        if (wrote < pcm.length) {
            /* A short or failed load would play silence (or a fragment) while
             * still holding a mixer track. Say so and drop it. */
            Log.i("pancra", what + ": short write " + wrote + " of " + pcm.length);
            tr.release();
            return null;
        }
        tr.play();
        return tr;
    }

    public static synchronized void chirp(Context ctx, int st10) {
        try {
            final int rate = 22050;
            final int tone = (rate * CHIRP_MS) / 1000;      /* the audible part */
            final int n = tone + ((rate * CHIRP_PAD_MS) / 1000); /* + silence */
            double f1 = CHIRP_HZ * Math.pow(2.0, st10 / 120.0);
            short[] pcm = new short[n];   /* the tail stays zero: silence */
            double phase = 0;
            int fade = rate / 200;                /* 5 ms of ramp each end */
            for (int i = 0; i < tone; i++) {
                double u = (double) i / (double) tone;
                double f = CHIRP_HZ + ((f1 - CHIRP_HZ) * u);
                phase += (2.0 * Math.PI * f) / rate;
                double a = 1.0;
                if (i < fade) a = 0.5 - (0.5 * Math.cos((Math.PI * i) / fade));
                else if (i > tone - fade)
                    a = 0.5 - (0.5 * Math.cos((Math.PI * (tone - i)) / fade));
                pcm[i] = (short) (Math.sin(phase) * a * 26000);
            }
            android.media.AudioTrack tr = playPcm(pcm, rate, "chirp");
            if (tr == null) return;
            /* Release the PREVIOUS track, never this one -- see playPcm. At
             * most one track is alive at a time (datapoints are ~5 minutes
             * apart), so nothing accumulates. */
            if (chirpTrack != null) {
                try { chirpTrack.release(); } catch (Throwable x) {}
            }
            chirpTrack = tr;
        } catch (Throwable t) { Log.i("pancra", "chirp: " + t); }
    }

    /* NUDGE: the one-time heads-up on the wider threshold band (alarmlogic.h).
     *
     * It has to be unmistakably NOT the other two sounds, and unmistakably not
     * the alarm, because the whole point is that the user can hear it and
     * decide it does not matter:
     *
     *   vs BEEP/CHIRP -- those are ONE 200 ms note at 1200 Hz. This is TWO
     *     notes, an octave lower, with a gap between them and half again the
     *     duration. Different pitch, different rhythm, different length: no
     *     amount of distance or pocket muffling makes them the same event.
     *   vs the ALARM -- that is the system alarm ringtone, looping at full
     *     USAGE_ALARM volume until it is dismissed. This is one soft motif at
     *     roughly two thirds the chirp's amplitude and then silence. It asks
     *     for a glance, not for action.
     *
     * The two notes FALL for a low crossing and RISE for a high one, the same
     * direction convention CHIRP uses, so which threshold was crossed is
     * audible without looking.
     *
     * The vibration is a single short double-buzz, NOT the alarm's repeating
     * 600/400 waveform, and it is not cancelled afterwards because it never
     * repeats -- a one-shot cannot be left running. It deliberately does not
     * cancel any alarm vibration either: nudge_fire suppresses the nudge while
     * an alarm is active, so the two cannot overlap.
     *
     * `sound` and `vibrate` are the NUDGE's own settings, not the alarm's --
     * see settings.h. They are honoured independently and each in its own try
     * block, for the same reason trigger() is staged: a throw from one must
     * not be able to suppress the other.
     *
     * kind: 0 = crossed the nudge LOW, 1 = crossed the nudge HIGH. */
    private static final int NUDGE_NOTE_MS = 150;
    private static final int NUDGE_GAP_MS = 70;
    private static final int NUDGE_PAD_MS = 40;   /* see CHIRP_PAD_MS */
    private static final double NUDGE_HI_HZ = 740.0;
    private static final double NUDGE_LO_HZ = 554.0;
    private static final int NUDGE_AMP = 17000;   /* softer than the chirp */
    private static android.media.AudioTrack nudgeTrack;

    /* One enveloped sine note written into pcm[off..off+n). The raised-cosine
     * fade on each end is what keeps the note from clicking; the chirp learned
     * that the same way. */
    private static void note(short[] pcm, int off, int n, double hz, int rate) {
        int fade = rate / 125;                    /* 8 ms of ramp each end */
        double phase = 0;
        for (int i = 0; i < n; i++) {
            phase += (2.0 * Math.PI * hz) / rate;
            double a = 1.0;
            if (i < fade) a = 0.5 - (0.5 * Math.cos((Math.PI * i) / fade));
            else if (i > n - fade)
                a = 0.5 - (0.5 * Math.cos((Math.PI * (n - i)) / fade));
            pcm[off + i] = (short) (Math.sin(phase) * a * NUDGE_AMP);
        }
    }

    public static synchronized void nudge(Context ctx, int kind,
                                          boolean sound, boolean vibrate) {
        try {
            if (sound) {
                final int rate = 22050;
                int nn = (rate * NUDGE_NOTE_MS) / 1000;
                int ng = (rate * NUDGE_GAP_MS) / 1000;
                int pad = (rate * NUDGE_PAD_MS) / 1000;
                short[] pcm = new short[(2 * nn) + ng + pad]; /* gap+tail stay 0 */
                double first = (kind == 0) ? NUDGE_HI_HZ : NUDGE_LO_HZ;
                double second = (kind == 0) ? NUDGE_LO_HZ : NUDGE_HI_HZ;
                note(pcm, 0, nn, first, rate);
                note(pcm, nn + ng, nn, second, rate);
                android.media.AudioTrack tr = playPcm(pcm, rate, "nudge");
                if (tr != null) {
                    if (nudgeTrack != null) {
                        try { nudgeTrack.release(); } catch (Throwable x) {}
                    }
                    nudgeTrack = tr;
                }
            }
        } catch (Throwable t) { Log.i("pancra", "nudge (sound): " + t); }

        try {
            Context app = ctx.getApplicationContext();
            Vibrator v = app.getSystemService(Vibrator.class);
            /* -1 = play once. NOT 0, which repeats forever: this method has no
             * counterpart to cancel it, so a repeating waveform here would
             * buzz until the process died. */
            if (vibrate && v != null && v.hasVibrator())
                v.vibrate(VibrationEffect.createWaveform(
                    new long[]{0, 90, 110, 90}, -1));
        } catch (Throwable t) { Log.i("pancra", "nudge (vibrate): " + t); }
    }

    private static void ensureChannel(NotificationManager nm) {
        if (nm.getNotificationChannel(CH) != null) return;
        NotificationChannel c = new NotificationChannel(CH, "Glucose alarm",
            NotificationManager.IMPORTANCE_HIGH);
        c.setSound(null, null);        /* we loop the sound ourselves */
        c.enableVibration(false);      /* we loop the vibration ourselves */
        nm.createNotificationChannel(c);
    }

    /* kind: 0 = glucose low, 1 = glucose high, 2 = stale/disconnected */
    /* THE AUDIBLE PARTS GO FIRST, AND EACH STAGE HAS ITS OWN CATCH.
     *
     * This whole method used to be one try block with the notification built
     * first. Anything that threw before the MediaPlayer block -- and the
     * notification path is by far the most throw-prone thing here, touching
     * NotificationManager, PendingIntent and a channel the user can alter --
     * jumped straight to the catch, so the sound and the vibration never ran.
     * The C side has already committed g_alarm_want by then, so alarm_apply
     * considers the alarm RAISED and never retries: a hypo that produces one
     * log line and no sound. Three independent stages means a failure in one
     * cannot silence the others, and the ones that actually wake the user are
     * attempted before the one that merely informs them. */
    public static synchronized void trigger(Context ctx, int kind, boolean sound, boolean vibrate) {
        Context app;
        try { app = ctx.getApplicationContext(); }
        catch (Throwable t) { Log.i("pancra", "alarm trigger (context): " + t); return; }

        /* 1. Sound. */
        try {
            stopSound();
            if (sound) {
                Uri uri = RingtoneManager.getActualDefaultRingtoneUri(app, RingtoneManager.TYPE_ALARM);
                if (uri == null) uri = RingtoneManager.getDefaultUri(RingtoneManager.TYPE_ALARM);
                if (uri != null) {
                    player = new MediaPlayer();
                    player.setAudioAttributes(new AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_ALARM)
                        .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION).build());
                    player.setDataSource(app, uri);
                    player.setLooping(true);
                    player.prepare();
                    player.start();
                }
            }
        } catch (Throwable t) {
            /* Drop a half-built player: prepare() or start() throwing leaves it
             * allocated but not looping, and silence() would then release an
             * object that is not making noise while the real problem persists. */
            stopSound();
            Log.i("pancra", "alarm trigger (sound): " + t);
        }

        /* 2. Vibration -- the fallback when the phone is muted.
         *
         * cancel() FIRST and unconditionally, mirroring stage 1's stopSound().
         * The waveform repeats until cancelled, so without this a re-trigger
         * with vibrate=false left the PREVIOUS alarm's buzzing running: turn
         * VIBRATION off while a LOW is active, then let it cross to HIGH, and
         * the low's waveform kept going with nothing in trigger() to stop it.
         * Stage 1 already had this shape; stage 2 was the asymmetry the staged
         * restructure was supposed to remove. */
        try {
            Vibrator v = app.getSystemService(Vibrator.class);
            if (v != null) {
                v.cancel();
                if (vibrate && v.hasVibrator())   /* 600ms on / 400ms off, repeating */
                    v.vibrate(VibrationEffect.createWaveform(new long[]{0, 600, 400}, 0));
            }
        } catch (Throwable t) { Log.i("pancra", "alarm trigger (vibrate): " + t); }

        /* 3. Notification -- informational; must never suppress 1 or 2. */
        try {
            NotificationManager nm = app.getSystemService(NotificationManager.class);
            ensureChannel(nm);

            Intent open = new Intent(app, android.app.NativeActivity.class);
            open.setAction(Intent.ACTION_MAIN);
            open.addCategory(Intent.CATEGORY_LAUNCHER);
            open.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP);
            PendingIntent pi = PendingIntent.getActivity(app, 0, open,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
            Notification n = new Notification.Builder(app, CH)
                .setContentTitle(kind == 2 ? "Sensor disconnected"
                               : kind == 1 ? "Glucose HIGH" : "Glucose LOW")
                .setContentText(kind == 2 ? "No recent readings - tap to open"
                                          : "Open the app to silence")
                .setSmallIcon(android.R.drawable.stat_sys_warning)
                .setCategory(Notification.CATEGORY_ALARM)
                .setContentIntent(pi)
                .setOngoing(true)
                .build();
            nm.notify(NID, n);
        } catch (Throwable t) { Log.i("pancra", "alarm trigger (notify): " + t); }
    }

    /* Staged for the same reason as trigger(), and it matters MORE here.
     *
     * These three used to share one try block with the vibrator first, so a
     * throw from v.cancel() skipped stopSound() and left a LOOPING
     * USAGE_ALARM MediaPlayer running. The C side clears g_alarm_sounding
     * either way, so nothing would ever call silence() again: a tone that
     * plays until the process dies, which is precisely the un-silenceable
     * alarm the locking around these calls exists to prevent. Stop the sound
     * FIRST and unconditionally. */
    public static synchronized void silence(Context ctx) {
        try { stopSound(); }
        catch (Throwable t) { Log.i("pancra", "alarm silence (sound): " + t); }

        Context app;
        try { app = ctx.getApplicationContext(); }
        catch (Throwable t) { Log.i("pancra", "alarm silence (context): " + t); return; }

        try {
            Vibrator v = app.getSystemService(Vibrator.class);
            if (v != null) v.cancel();
        } catch (Throwable t) { Log.i("pancra", "alarm silence (vibrate): " + t); }

        try {
            NotificationManager nm = app.getSystemService(NotificationManager.class);
            if (nm != null) nm.cancel(NID);
        } catch (Throwable t) { Log.i("pancra", "alarm silence (notify): " + t); }
    }

    /* release() gets its OWN try, and the reference is dropped last.
     *
     * With one combined try, a throw from stop() skipped release() and then
     * nulled the only reference -- leaving a looping USAGE_ALARM player that
     * nothing in the process could ever reach again. That is precisely the
     * un-silenceable alarm the rest of this file is built to prevent, and it
     * was the one place here that was not stage-isolated. */
    private static void stopSound() {
        MediaPlayer p = player;
        if (p == null) return;
        try { p.stop(); } catch (Throwable t) { Log.i("pancra", "stop: " + t); }
        try { p.release(); } catch (Throwable t) { Log.i("pancra", "release: " + t); }
        player = null;
    }
}
