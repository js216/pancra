// SPDX-License-Identifier: GPL-3.0
// ExportSnapshot.java --- writing one export snapshot: rows, streams, publish
// Copyright 2026 Jakob Kastelic

/* THE HALF OF "EXPORT DATA" THAT CAN BE WRONG WITHOUT ANYONE NOTICING.
 *
 * Ble.exportData picks the sections and hands the finished URI to the share
 * sheet. Everything between those two -- opening the sources, deciding which
 * bytes are safe to copy, closing the output, and only then making the file
 * shareable -- is here, because every one of those steps fails in a way whose
 * only symptom is a file that LOOKS fine to the sender and is wrong in
 * somebody else's app, hours later.
 *
 * THE TWO WAYS AN EXPORT GOES WRONG QUIETLY:
 *
 *   1. A PARTIAL FILE IS PUBLISHED. With the whole export in one broad
 *      try/catch, a read, a write or a close that throws part-way jumps to
 *      the catch -- which logs a line -- while the snapshot it has already
 *      half-written keeps the name the share sheet is about to hand out. With
 *      the input streams closed only on the normal path, the same throw
 *      also leaked a descriptor until the finaliser got round to it. What the
 *      doctor received was a truncated export with no marker of any kind:
 *      CSV has no length field and no terminator, so half a file parses.
 *
 *   2. THE LAST ROW WAS INVENTED. The filtered copy read the live CSV with
 *      BufferedReader.readLine() and wrote each line back followed by '\n'.
 *      readLine() cannot tell "the final line has no trailing newline" from
 *      "native is half way through appending this row", and these files are
 *      appended to by C code (reading.c, insulin.c, weight.c, sensors.c) with
 *      no coordination with Java whatsoever -- a CGM reading lands every five
 *      minutes and does not wait for a share sheet. So an export taken during
 *      an append copied a torn row and TERMINATED it with a newline, turning
 *      a fragment into a row that no file ever contained. Downstream it is a
 *      reading at a plausible time with a truncated value, or two fields of
 *      one row and none of the next.
 *
 * THE RULE FOR (2) IS THE NEWLINE, and it is deliberately self-contained: a
 * byte in one of these files is proven to be part of a complete row only once
 * a '\n' has been seen AFTER it. No lock, no handshake and no cooperation
 * from the native side is required to know that, which matters because the
 * native side appends from threads Java cannot see. The cost is that an
 * export taken mid-append omits the row being written -- a row that is at
 * most one sensor cycle old and will be in the next export -- and the
 * alternative is a row that never existed at all.
 *
 * (These files are append-only in normal operation. Where a rewrite does
 * happen -- a compaction, a settings migration -- the newline rule still
 * yields whole rows; it does not promise they came from one instant, which is
 * why a REWRITTEN source is copied under the same rule and nothing more is
 * claimed about it here.)
 *
 * THE RULE FOR (1) IS THE ORDER: write into a temporary file, close it and
 * let the close fail, and publish under the shareable name only afterwards.
 * close() on a buffered stream is where the last block is flushed, so a
 * design that grants the URI before the close is the same bug wearing a
 * different hat -- it publishes bytes that are still in a buffer.
 *
 * WHY IT IS ITS OWN CLASS. All of it is java.io and none of it is Android,
 * so `make exportjavatest` runs it on the host: a source that throws part-way
 * through a read, an output whose CLOSE is the thing that fails, a file whose
 * tail is a torn row. None of those can be produced on a phone on demand,
 * which is exactly why they were never checked. */
package com.jk.pancra;

import java.io.BufferedOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

final class ExportSnapshot {
    /* THE LONGEST RUN OF BYTES WE WILL HOLD WAITING FOR A NEWLINE.
     *
     * A row is buffered until its terminator arrives, so a file containing no
     * '\n' at all would otherwise be held entirely in memory before being
     * dropped. Every row this app writes is a few dozen bytes; a megabyte
     * without a terminator is not a long row, it is a file that is not one of
     * ours, and refusing the whole export says so where a silent truncation
     * would not. */
    static final int MAX_ROW = 1 << 20;

    /* ---- ONE SECTION OF THE EXPORT ------------------------------------ */

    /* An OPENER rather than a File, so the test can hand in a stream that
     * fails half way through a read -- the case that leaks the descriptor and
     * publishes the prefix. */
    interface Section {
        /* The bytes to copy, or NULL when this section has nothing on disk.
         * Null is not an error: a user who has logged no insulin still gets
         * the sections that do exist. */
        InputStream open() throws IOException;

        /* Rows whose leading epoch field is older are dropped; 0 keeps all. */
        long cutoff();
    }

    static final class FileSection implements Section {
        private final File src;
        private final long cutoff;

        FileSection(File src, long cutoff) {
            this.src = src;
            this.cutoff = cutoff;
        }

        /* exists(), NOT isFile(). A missing source is an ordinary state and is
         * skipped; a source that exists and is not a regular file is not a
         * state this app produces, and letting the open throw fails the export
         * loudly rather than shipping one silently missing section. */
        @Override public InputStream open() throws IOException {
            if (src == null || !src.exists()) return null;
            return new FileInputStream(src);
        }

        @Override public long cutoff() { return cutoff; }
    }

    /* ---- WHERE THE BYTES GO, AND WHEN THEY BECOME SHAREABLE ------------
     *
     * Three calls rather than a File, for the same reason ServiceOps in
     * ExportPolicy is an interface: the ORDER is the safety property. With
     * the rename written inline next to the stream, no test could observe
     * that publish happens after the close and not before it, and "after"
     * is the entire fix. */
    interface Sink {
        /* The TEMPORARY file. Nothing may serve this name. */
        OutputStream open() throws IOException;

        /* Make what was written the snapshot the share sheet names. Called
         * once, only after the output stream has closed without throwing. */
        void publish() throws IOException;

        /* Leave nothing behind: no temporary, and no file under the
         * shareable name either.
         *
         * ANSWERS, rather than trying and hoping. Returns null
         * when the directory is clean and a human-readable description of
         * what is still there when it is not -- because "a failed export
         * leaves nothing" is a contract, and a contract nothing checks is a
         * comment. A leftover under the SHAREABLE name is the dangerous one:
         * it is a zero-byte or half-written file with a name the provider
         * will serve, so an implementation that cannot delete it must make
         * it unservable before it reports. */
        String discard();
    }

    /* WHAT A HALF-WRITTEN SNAPSHOT IS CALLED, and why the suffix matters more
     * than it looks. `pancra-DDDDDDDDDD-HHHHHHHH.csv.part` is 34 characters,
     * so ExportPolicy.exportNameValid refuses it on the length check alone:
     * the provider cannot serve it, and Ble.cleanupExports -- which only
     * considers files whose names are valid snapshot names -- will not delete
     * it either. That is the right pair of answers while an export is in
     * flight, and it means a temporary orphaned by a process death (the app
     * killed for memory between the write and the rename) would live for
     * ever, so sweeping them is this file's job too. */
    static final String PART_SUFFIX = ".part";

    static String partialName(String snapshot) {
        return snapshot + PART_SUFFIX;
    }

    static boolean isPartialName(String name) {
        if (name == null || !name.endsWith(PART_SUFFIX)) return false;
        return ExportPolicy.exportNameValid(
            name.substring(0, name.length() - PART_SUFFIX.length()));
    }

    /* Remove temporaries left by an export that never finished. Narrow on
     * purpose: only names that are a snapshot name plus the suffix, so
     * anything else that ever appears in the directory is left alone.
     *
     * Safe to run at the start of an export because exports are serialised by
     * the thing that starts them -- the EXPORT DATA menu item, on the UI
     * thread -- so a `.part` found here belongs to a run that is over. */
    static String sweepPartials(File dir) {
        if (dir == null) return null;
        File[] all = dir.listFiles();
        if (all == null) return null;
        String left = null;
        for (File f : all) {
            if (!f.isFile() || !isPartialName(f.getName())) continue;
            /* ANSWERED, NOT ATTEMPTED. A temporary that survives
             * the sweep is not dangerous on its own -- nothing serves a
             * .part -- but it is evidence about the directory this export is
             * about to write into, and the one case that matters is when the
             * survivor is the temporary this run needs. See open(). */
            if (!f.delete())
                left = (left == null) ? f.getName() : left + ", " + f.getName();
        }
        return left;
    }

    /* The real one: write beside the snapshot, publish with a rename.
     *
     * RENAME, because it is the only step that is atomic with respect to a
     * reader. The name either denotes the finished export or does not exist;
     * there is no instant at which it denotes a prefix. (Same directory, so
     * the rename cannot cross a filesystem and degrade to a copy.) */
    static final class FileSink implements Sink {
        private final File dest;
        private final File tmp;

        FileSink(File dest) {
            File d = dest.getAbsoluteFile();
            this.dest = d;
            this.tmp = new File(d.getParentFile(),
                                partialName(d.getName()));
        }

        File temporary() { return tmp; }

        @Override public OutputStream open() throws IOException {
            String left = sweepPartials(tmp.getParentFile());
            /* THE ONE SURVIVOR THAT MATTERS IS OUR OWN. Another run's
             * leftover .part is unservable and will be swept again next
             * time; refusing this export over it would turn one stuck file
             * into no exports at all. But a temporary under the name this
             * run is about to write is a file that could not be deleted, so
             * opening it would either fail obscurely or -- worse, if it is
             * writable after all -- append this export onto the wreckage of
             * an older one. */
            if (left != null && left.contains(tmp.getName()))
                throw new IOException("cannot clear " + tmp.getName());
            return new FileOutputStream(tmp);
        }

        /* READ-ONLY BEFORE THE RENAME, not after. Set afterwards there is a
         * window in which the shareable name is writable, and the whole point
         * of the read-only bit is that a future change which opens a snapshot
         * for writing fails loudly rather than truncating a file somebody is
         * reading. It does not impede the retention delete: unlinking needs
         * write permission on the DIRECTORY, not on the file. */
        @Override public void publish() throws IOException {
            /* THE READ-ONLY BIT IS REQUIRED, NOT ATTEMPTED.
             * setReadOnly()'s answer was dropped, so a filesystem that does
             * not carry the bit -- or a directory the app cannot chmod in --
             * published a WRITABLE snapshot while this code claimed an
             * immutable one. Asked twice, deliberately: setReadOnly() can
             * report success on a filesystem that then reports the file
             * writable anyway (FAT-backed shared storage does exactly this),
             * and canWrite() is the property the comment above actually
             * promises. Refusing here is right because the alternative is
             * publishing under the promise and breaking it silently; the
             * caller discards, and the user is told the export failed. */
            if (!tmp.setReadOnly() || tmp.canWrite())
                throw new IOException("cannot seal " + tmp.getName()
                                      + " read-only");
            if (!tmp.renameTo(dest))
                throw new IOException("cannot publish " + dest.getName());
        }

        /* BOTH, always. The temporary is the partial data; `dest` is the
         * empty file the caller created to reserve the name, and leaving it
         * would publish a zero-byte export under a name that passes every
         * check the provider makes.
         *
         * THE TWO ARE NOT THE SAME KIND OF LEFTOVER. A surviving `.part` is
         * already harmless: its name fails ExportPolicy.exportNameValid, so
         * the provider will not serve it and the retention sweep will not
         * touch it, and sweepPartials removes it at the start of the next
         * export. A surviving `dest` is the opposite -- a servable name over
         * a file that is empty or half written -- so if it cannot be deleted
         * it is RENAMED into the .part space, which makes it unservable and
         * puts it in front of the next sweep. Both are still reported: a
         * quarantine is a leftover that has been made safe, not a success. */
        @Override public String discard() {
            String left = null;
            if (tmp.exists() && !tmp.delete())
                left = tmp.getName();
            String d = removeOrQuarantine(dest);
            if (d != null)
                left = (left == null) ? d : left + ", " + d;
            return left;
        }
    }

    /* Delete `f`, or -- failing that -- move it somewhere nothing will serve
     * it from. Returns null when the file is gone, and what is still on disk
     * when it is not.
     *
     * The quarantine name is the .part name for `f`, which is exactly the
     * name a temporary for this snapshot would have. That is safe HERE and
     * only here: this runs from discard(), after the temporary has already
     * been dealt with, so the name is free unless the temporary is the thing
     * that could not be deleted -- in which case the rename fails, and the
     * leftover is reported rather than hidden. */
    static String removeOrQuarantine(File f) {
        if (f == null || !f.exists()) return null;
        if (f.delete()) return null;
        /* ONLY A FILE IS QUARANTINED. Anything else under a snapshot's name
         * is not something this code wrote -- a directory is the shape that
         * makes the rename in publish() fail in the first place -- and
         * moving it would be this module tidying up somebody else's object
         * into a name it reserves for its own. The provider serves files, so
         * a non-file under the name is already unservable; it is reported
         * and left exactly where it is. */
        if (!f.isFile()) return f.getName();
        File q = new File(f.getParentFile(), partialName(f.getName()));
        if (!q.exists() && f.renameTo(q))
            return f.getName() + " (quarantined as " + q.getName() + ")";
        return f.getName();
    }

    /* ---- THE EXPORT ITSELF --------------------------------------------- */

    /* Write every section, then publish. Returns true if a snapshot now
     * exists under the shareable name -- and ONLY then may the caller build a
     * URI for it.
     *
     * FALSE IS NOT A FAILURE: it means every selected section was empty, so
     * there is nothing to share and nothing was left on disk. A throw is a
     * failure, and by the time it leaves this method the temporary and the
     * destination are both gone.
     *
     * try-with-resources ON EVERY STREAM, input side included. Closing the
     * sources only on the normal path leaves the throw that matters -- the
     * one that abandons the export -- as exactly the one that leaks the
     * descriptor. */
    static boolean write(Sink sink, Section[] secs) throws IOException {
        boolean published = false;
        /* THE PRIMARY FAILURE OUTRANKS THE CLEANUP'S. A cleanup
         * that cannot finish has to be surfaced -- silence is how "a failed
         * export leaves nothing" stopped being true -- but thrown from the
         * finally it would REPLACE the exception that says why the export
         * failed at all, and that one is the one worth reading. So the
         * primary is caught, the cleanup's failure is attached to it, and it
         * is rethrown; with no primary, the cleanup failure is itself the
         * failure. Throwable, not IOException: an Error propagating out of a
         * write must not be masked either. */
        Throwable primary = null;
        try {
            long total = 0;
            /* BUFFERED, so the close below is a real flush and a real place
             * to fail. That is the point of putting publish() after it. */
            try (OutputStream raw = new BufferedOutputStream(sink.open())) {
                SectionOut out = new SectionOut(raw);
                for (int i = 0; secs != null && i < secs.length; i++) {
                    Section s = secs[i];
                    if (s == null) continue;
                    try (InputStream in = s.open()) {
                        if (in == null) continue; /* nothing logged yet */
                        out.startSection();
                        copyRows(in, out, s.cutoff(), MAX_ROW);
                    }
                }
                total = out.total();
            }
            /* THE STREAM IS CLOSED AND THE CLOSE DID NOT THROW. Everything
             * written is on the filesystem; nothing before this line is
             * allowed to hand the name out. */
            if (total == 0)
                return false; /* the finally discards it */
            sink.publish();
            published = true;
            return true;
        } catch (Throwable t) {
            primary = t;
            throw t;
        } finally {
            /* Every path that is not a successful publish leaves NOTHING: a
             * failed write, a failed close, a failed rename, an empty export.
             * A partial file that nobody shared is still a partial file the
             * next mistake can share. */
            if (!published) {
                String left = sink.discard();
                if (left != null) {
                    IOException ce = new IOException(
                        "export: could not remove " + left);
                    if (primary != null) primary.addSuppressed(ce);
                    else throw ce;
                }
            }
        }
    }

    /* THE BLANK LINE BETWEEN SECTIONS, EMITTED ONLY IF A SECTION FOLLOWS IT.
     *
     * The separator has to be written before a section's first byte, and
     * whether a section produces any bytes is not known until it has been
     * read -- an absent file, a cutoff that excludes every row and a file
     * that is one torn row all produce none. Writing it eagerly gave an
     * export of nothing but separators a non-zero length, which is how a
     * "successful" share of a file containing three newlines happened.
     *
     * So it is armed here and paid for on the next actual write, which also
     * makes the byte count exact: total() is zero if and only if no row was
     * exported, and that is what decides whether anything is published. */
    private static final class SectionOut extends OutputStream {
        private final OutputStream out;
        private long total;
        private boolean pending;

        SectionOut(OutputStream out) { this.out = out; }

        void startSection() { if (total > 0) pending = true; }

        long total() { return total; }

        @Override public void write(int b) throws IOException {
            byte[] one = { (byte) b };
            write(one, 0, 1);
        }

        @Override public void write(byte[] b, int off, int len)
                throws IOException {
            if (len <= 0) return;
            if (pending) {
                out.write('\n');
                total++;
                pending = false;
            }
            out.write(b, off, len);
            total += len;
        }

        @Override public void flush() throws IOException { out.flush(); }
        /* NOT closed here: the resource this wraps is the one write() owns,
         * and closing it twice from two places is how a close failure gets
         * swallowed by the second attempt. */
    }

    /* A ROW IN HAND, none of whose bytes are proven yet. */
    private static final class Row {
        byte[] b = new byte[256];
        int len;

        void add(byte[] src, int off, int n, int maxRow) throws IOException {
            if (len + n > maxRow)
                throw new IOException("export: no row boundary in "
                                      + maxRow + " bytes");
            if (len + n > b.length) {
                int cap = b.length;
                while (cap < len + n) cap <<= 1;
                if (cap > maxRow) cap = maxRow; /* len+n <= maxRow, so it fits */
                byte[] nb = new byte[cap];
                System.arraycopy(b, 0, nb, 0, len);
                b = nb;
            }
            System.arraycopy(src, off, b, len, n);
            len += n;
        }
    }

    /* COPY COMPLETE ROWS AND NOTHING ELSE. Returns the bytes written.
     *
     * A row is emitted when -- and only when -- its terminating '\n' has been
     * read, and it is emitted WITH that byte, exactly as it was found. So:
     *
     *   - a file whose tail is a row native is still appending exports
     *     without that tail, and every byte before it byte for byte;
     *   - a file that ends exactly at a newline exports in full. This is the
     *     obvious way to break the rule -- stopping one row short is
     *     invisible unless something checks it, and the last row is the
     *     newest reading, the one the doctor is looking at;
     *   - a file with no newline anywhere exports as empty, because nothing
     *     in it is a proven row. NOT as one row: manufacturing a terminator
     *     is what turns a fragment into a row.
     *
     * No newline is ever added: the terminator is copied from the source, or
     * the bytes are not copied at all.
     *
     * The cutoff filter is applied per complete row and cannot reintroduce
     * the problem: a dropped row is dropped whole. */
    static long copyRows(InputStream in, OutputStream out, long cutoff,
                         int maxRow) throws IOException {
        byte[] buf = new byte[4096];
        Row row = new Row();
        long written = 0;
        int n;
        while ((n = in.read(buf)) > 0) {
            int p = 0;
            while (p < n) {
                int nl = -1;
                for (int i = p; i < n; i++)
                    if (buf[i] == '\n') { nl = i; break; }
                int end = (nl < 0) ? n : nl + 1;
                row.add(buf, p, end - p, maxRow);
                p = end;
                if (nl < 0) break; /* the rest of this row has not arrived */
                if (rowKept(row.b, row.len, cutoff)) {
                    out.write(row.b, 0, row.len);
                    written += row.len;
                }
                row.len = 0;
            }
        }
        /* END OF FILE WITH BYTES IN HAND: a row without a terminator. It is
         * dropped, and it is the only thing that is. */
        return written;
    }

    /* Keep a row unless its LEADING integer -- epoch seconds, the first field
     * of readings.csv, insulin.csv and weight.csv alike -- is older than the
     * cutoff. A row that does not start with a digit is a header and is
     * always kept, so a filtered export stays self-describing.
     *
     * Twelve digits at most, which is the bound the original filter had: it
     * is past any epoch this app will see, and it means a row beginning with
     * a long run of digits cannot spin arithmetic into an overflow. */
    static boolean rowKept(byte[] row, int len, long cutoff) {
        if (cutoff <= 0) return true;
        long t = -1;
        for (int i = 0; i < len && i < 12; i++) {
            byte c = row[i];
            if (c < '0' || c > '9') break;
            t = (t < 0 ? 0 : t) * 10 + (c - '0');
        }
        return t < 0 || t >= cutoff;
    }

    private ExportSnapshot() {}
}
