// SPDX-License-Identifier: GPL-3.0
package com.jk.pancra;

/* ANDROID-FREE NOTIFICATION POLICY: what the ongoing notification should
 * say, and which id each notification owns. */
final class NotifPolicy {

    /* ---- THE FOREGROUND NOTIFICATION'S CONTENT, AS ONE VALUE ------------
     *
     * Title, text, value, lock state, pixels, width and height in seven
     * separate volatile fields, written one after another as each reading
     * lands and read one after another while the notification is built, are
     * individually safe and are not safe as a SET: two of the mixtures a
     * build can see are worse than stale:
     *
     *   - new pixels against the previous width and height. `px.length >= w *
     *     h` is the only thing standing between this and Bitmap.createBitmap
     *     reading past the end of the array, and a large new plot paired with
     *     small earlier dimensions PASSES it -- drawing a stretched corner of
     *     the new plot;
     *   - the value from one reading beside the title and text of another, so
     *     the status bar shows one number and the row beneath it another.
     *
     * The rule is here, and not only in PancraService, because it is pure: an
     * object either carries a plot whose dimensions match its pixels or it
     * carries none, and a call bringing no plot keeps the previous one. That is
     * three sentences of arithmetic guarding a read past the end of an array,
     * and on the host it can be executed rather than argued. */
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
}
