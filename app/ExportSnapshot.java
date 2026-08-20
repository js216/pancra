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
 * WHAT USED TO HAPPEN, twice over:
 *
 *   1. A PARTIAL FILE WAS PUBLISHED. The whole export sat in one broad
 *      try/catch. A read, a write or a close that threw part-way jumped to
 *      the catch -- which logged a line -- while the snapshot it had already
 *      half-written kept the name the share sheet was about to hand out. The
 *      input streams were closed only on the normal path, so the same throw
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
     * fails half way through a read -- the case that used to leak the
     * descriptor and publish the prefix. */
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
         * loudly instead of shipping one silently missing section. */
        @Override public InputStream open() throws IOException {
            if (src == null || !src.exists()) return null;
            return new FileInputStream(src);
        }

        @Override public long cutoff() { return cutoff; }
    }

    /* ---- WHERE THE BYTES GO, AND WHEN THEY BECOME SHAREABLE ------------
     *
     * Three calls rather than a File, for the same reason ServiceOps in
     * BoundaryLogic is an interface: the ORDER is the safety property. With
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
         * shareable name either. */
        void discard();
    }

    /* WHAT A HALF-WRITTEN SNAPSHOT IS CALLED, and why the suffix matters more
     * than it looks. `pancra-DDDDDDDDDD-HHHHHHHH.csv.part` is 34 characters,
     * so BoundaryLogic.exportNameValid refuses it on the length check alone:
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
        return BoundaryLogic.exportNameValid(
            name.substring(0, name.length() - PART_SUFFIX.length()));
    }

    /* Remove temporaries left by an export that never finished. Narrow on
     * purpose: only names that are a snapshot name plus the suffix, so
     * anything else that ever appears in the directory is left alone.
     *
     * Safe to run at the start of an export because exports are serialised by
     * the thing that starts them -- the EXPORT DATA menu item, on the UI
     * thread -- so a `.part` found here belongs to a run that is over. */
    static void sweepPartials(File dir) {
        if (dir == null) return;
        File[] all = dir.listFiles();
        if (all == null) return;
        for (File f : all)
            if (f.isFile() && isPartialName(f.getName()))
                f.delete();
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
            sweepPartials(tmp.getParentFile());
            return new FileOutputStream(tmp);
        }

        /* READ-ONLY BEFORE THE RENAME, not after. Set afterwards there is a
         * window in which the shareable name is writable, and the whole point
         * of the read-only bit is that a future change which opens a snapshot
         * for writing fails loudly instead of truncating a file somebody is
         * reading. It does not impede the retention delete: unlinking needs
         * write permission on the DIRECTORY, not on the file. */
        @Override public void publish() throws IOException {
            tmp.setReadOnly();
            if (!tmp.renameTo(dest))
                throw new IOException("cannot publish " + dest.getName());
        }

        /* BOTH, always. The temporary is the partial data; `dest` is the
         * empty file the caller created to reserve the name, and leaving it
         * would publish a zero-byte export under a name that passes every
         * check the provider makes. */
        @Override public void discard() {
            tmp.delete();
            dest.delete();
        }
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
     * try-with-resources ON EVERY STREAM, input side included. The old code
     * closed the sources only on the normal path, so the throw that mattered
     * -- the one that abandoned the export -- was exactly the one that leaked
     * the descriptor. */
    static boolean write(Sink sink, Section[] secs) throws IOException {
        boolean published = false;
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
        } finally {
            /* Every path that is not a successful publish leaves NOTHING: a
             * failed write, a failed close, a failed rename, an empty export.
             * A partial file that nobody shared is still a partial file the
             * next mistake can share. */
            if (!published) sink.discard();
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
     *     obvious way to break the fix -- stopping one row short is invisible
     *     unless something checks it, and the last row is the newest reading,
     *     the one the doctor is looking at;
     *   - a file with no newline anywhere exports as empty, because nothing
     *     in it is a proven row. NOT as one row: manufacturing a terminator
     *     is the bug this replaces.
     *
     * No newline is ever added. The old path wrote `line + '\n'`, which is
     * what turned a fragment into a row; here the terminator is copied from
     * the source or the bytes are not copied at all.
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
