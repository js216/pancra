// SPDX-License-Identifier: GPL-3.0
package com.jk.pancra;

/* ANDROID-FREE EXPORT POLICY: what an export file is called, which names
 * this app will accept back, and when a snapshot may be reclaimed (item
 * 263). */
final class ExportPolicy {

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
     * rejected by the provider, which is a failed export rather than an ugly
     * timestamp. */
    /* A WORD THAT FITS THE ROW, rather than a Java class name.
     *
     * Two adapters need it -- the sync transport reports why an exchange
     * failed, and the BLE side reports why a bond was refused -- and it is a
     * decision about a string, which is what this class is for. It was a
     * private helper in the file that happened to need it first; the second
     * caller is how a copy gets made. */
    static String shortErr(Throwable e) {
        String n = e.getClass().getSimpleName();
        if (n.contains("Timeout")) return "TIMEOUT";
        if (n.contains("UnknownHost")) return "NO HOST";
        if (n.contains("Connect")) return "REFUSED";
        if (n.contains("NoRoute")) return "NO ROUTE";
        return "ERROR";
    }

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

    /* ==== BOUNDING AND TIMING THE SYNC EXCHANGE ========================
     *
     * PancraNet.syncHttp is the only caller; these rules are here because
     * they are the only parts of a socket exchange that can be RUN on a host
     * JVM, and a rule nobody can execute is a rule nobody has checked.
     *
     * ---- WHAT THE PERSON HOLDING THE PHONE WOULD SEE WITHOUT THEM ------
     *
     * Reading the response `while ((n = in.read(buf)) > 0)` into a
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
     * (PancraNet.pushExec: core 0, MAXIMUM 1). One such server therefore stopped
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
}
