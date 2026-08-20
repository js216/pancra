// SPDX-License-Identifier: GPL-3.0
package com.jk.pancra;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

/* WHAT THIS SUITE IS FOR.
 *
 * Two failures of EXPORT DATA that a phone shows nobody: a snapshot published
 * with only part of its bytes in it, and a snapshot containing a row that no
 * file ever held. Both are invisible to the sender -- the share sheet looks
 * normal either way -- and both are discovered by whoever the file was sent
 * to, if at all. So the assertions here are about what is on disk and what
 * bytes came out, never about which methods were called.
 *
 * A NOTE ON THE FAKES. The Sink and the sections are interfaces precisely so
 * that a close() that fails and a read() that dies half way through are
 * ordinary test inputs. Both happen on real phones (a full filesystem, a file
 * the OS reclaimed) and neither can be arranged on one on demand. */
public final class ExportSnapshotTest {
    private static int checks;

    private static void ck(boolean yes, String what) {
        checks++;
        if (!yes) throw new AssertionError(what);
    }

    /* ---- fakes --------------------------------------------------------- */

    /* Every effect the export has on the world outside itself, in order. The
     * ORDER is the property: publish must follow the close, because close is
     * where a buffered stream writes its last block. */
    private static final class Log {
        final StringBuilder s = new StringBuilder();
        void add(String e) { s.append(e).append(' '); }
        String text() { return s.toString(); }
        boolean has(String e) { return at(e) >= 0; }
        int at(String e) { return s.indexOf(e); }
    }

    private static final class FakeStream extends OutputStream {
        final Log log;
        final ByteArrayOutputStream got = new ByteArrayOutputStream();
        boolean failClose;
        int failAfter = -1; /* throw once this many bytes have been written */
        private int written;

        FakeStream(Log log) { this.log = log; }

        @Override public void write(int b) throws IOException {
            byte[] one = { (byte) b };
            write(one, 0, 1);
        }

        @Override public void write(byte[] b, int off, int len)
                throws IOException {
            if (failAfter >= 0 && written + len > failAfter)
                throw new IOException("disk full");
            written += len;
            got.write(b, off, len);
        }

        @Override public void close() throws IOException {
            log.add("close");
            if (failClose) throw new IOException("close failed");
        }
    }

    private static final class FakeSink implements ExportSnapshot.Sink {
        final Log log;
        FakeStream stream;
        boolean failClose;
        int failAfter = -1;

        FakeSink(Log log) { this.log = log; }

        @Override public OutputStream open() {
            log.add("open");
            stream = new FakeStream(log);
            stream.failClose = failClose;
            stream.failAfter = failAfter;
            return stream;
        }

        @Override public void publish() { log.add("publish"); }
        @Override public void discard() { log.add("discard"); }
    }

    /* A source that dies part way through, and says whether it was closed. */
    private static final class DyingSource
            implements ExportSnapshot.Section {
        final Log log;
        boolean closed;
        boolean openThrows;

        DyingSource(Log log) { this.log = log; }

        @Override public InputStream open() throws IOException {
            if (openThrows) throw new IOException("cannot open source");
            return new InputStream() {
                int left = 3;
                @Override public int read() throws IOException {
                    if (left-- <= 0) throw new IOException("read failed");
                    return 'x';
                }
                @Override public int read(byte[] b, int off, int len)
                        throws IOException {
                    int v = read();
                    b[off] = (byte) v;
                    return 1;
                }
                @Override public void close() { closed = true; log.add("src"); }
            };
        }

        @Override public long cutoff() { return 0; }
    }

    private static final class Bytes implements ExportSnapshot.Section {
        final byte[] b;
        final long cutoff;
        Bytes(String s, long cutoff) { this.b = s.getBytes(); this.cutoff = cutoff; }
        @Override public InputStream open() {
            return b == null ? null : new ByteArrayInputStream(b);
        }
        @Override public long cutoff() { return cutoff; }
    }

    private static final class Absent implements ExportSnapshot.Section {
        @Override public InputStream open() { return null; }
        @Override public long cutoff() { return 0; }
    }

    /* ---- helpers ------------------------------------------------------- */

    private static String rows(String in, long cutoff) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        ExportSnapshot.copyRows(new ByteArrayInputStream(in.getBytes()), out,
                                cutoff, ExportSnapshot.MAX_ROW);
        return new String(out.toByteArray());
    }

    private static void put(File f, byte[] b) throws IOException {
        try (FileOutputStream os = new FileOutputStream(f)) { os.write(b); }
    }

    private static byte[] slurp(File f) throws IOException {
        byte[] b = new byte[(int) f.length()];
        try (InputStream in = new java.io.FileInputStream(f)) {
            int p = 0, n;
            while (p < b.length && (n = in.read(b, p, b.length - p)) > 0) p += n;
        }
        return b;
    }

    /* ONLY inside the directory this test made. */
    private static void wipe(File dir) {
        File[] all = dir.listFiles();
        if (all != null)
            for (File f : all) { if (f.isDirectory()) wipe(f); f.delete(); }
        dir.delete();
    }

    public static void main(String[] args) throws Exception {
        /* ================================================================
         * ITEM 130: A ROW IS COPIED ONLY ONCE ITS OWN NEWLINE HAS BEEN READ
         *
         * The C side appends to readings.csv, insulin.csv, weight.csv and
         * sensors.csv from threads Java knows nothing about; a CGM reading
         * lands every five minutes and does not wait for a share sheet. The
         * old copy used BufferedReader.readLine() and wrote each line back
         * followed by '\n', and readLine() cannot tell a final line with no
         * trailing newline from a row that is half written -- so an export
         * taken mid-append TERMINATED a fragment and shipped it as a row.
         * ============================================================== */

        /* THE TORN TAIL IS DROPPED, and everything before it is byte for byte
         * what the file held. Both halves matter: dropping the fragment is
         * the fix, and leaving the rest untouched is what makes the export
         * still be the user's data. */
        ck(rows("1755000000,101\n1755000300,104\n17550006", 0)
               .equals("1755000000,101\n1755000300,104\n"),
           "a row the native side is still appending is not exported");

        /* AND THE LAST COMPLETE ROW SURVIVES. This is the way the fix breaks
         * -- stop one row short and nobody sees it, because the row that goes
         * missing is the NEWEST reading, which is the one being looked at. */
        ck(rows("1755000000,101\n1755000300,104\n", 0)
               .equals("1755000000,101\n1755000300,104\n"),
           "a file ending exactly at a newline exports in full");

        /* NOTHING PROVEN, NOTHING EXPORTED. Not one manufactured row: a file
         * with no terminator anywhere is a file whose only row is still being
         * written. */
        ck(rows("1755000000,101", 0).equals(""),
           "a file with no newline at all exports as empty");
        ck(rows("", 0).equals(""), "an empty source exports as empty");
        ck(rows("\n", 0).equals("\n"), "a lone terminator is a complete row");

        /* THE BYTES ARE COPIED, NOT RE-RENDERED. The old path went through a
         * String and back with the platform charset and re-terminated every
         * line, so a CR became a lost byte and anything not decodable became
         * '?'. Here a row is the bytes between terminators. */
        ck(rows("a,1\r\nb,2\r\n", 0).equals("a,1\r\nb,2\r\n"),
           "CRLF rows survive unchanged");
        byte[] raw = { 'a', ',', (byte) 0x80, '\n', 'b', (byte) 0xff };
        ByteArrayOutputStream keep = new ByteArrayOutputStream();
        ExportSnapshot.copyRows(new ByteArrayInputStream(raw), keep, 0,
                                ExportSnapshot.MAX_ROW);
        byte[] kept = keep.toByteArray();
        ck(kept.length == 4 && kept[2] == (byte) 0x80 && kept[3] == '\n',
           "bytes that are not text are copied unchanged, and the "
           + "unterminated tail is still dropped");

        /* A ROW LONGER THAN THE READ BUFFER. The boundary search runs per
         * 4 kB read, so a row that spans reads is the case where an
         * off-by-one lives. */
        StringBuilder big = new StringBuilder();
        for (int i = 0; i < 10000; i++) big.append('x');
        String spanned = rows(big + "\n" + big, 0);
        ck(spanned.equals(big + "\n"),
           "a row spanning several reads is exported whole, and the "
           + "unterminated one after it is not exported at all");

        /* A run with no terminator cannot grow without bound: it is held in
         * memory until it is proven, so something has to say how much of it
         * we will hold. Failing beats truncating silently. */
        boolean threw = false;
        try {
            ExportSnapshot.copyRows(
                new ByteArrayInputStream(new byte[600]),
                new ByteArrayOutputStream(), 0, 512);
        } catch (IOException e) { threw = true; }
        ck(threw, "a source with no row boundary at all is refused, not "
                  + "silently cut");

        /* ---- the cutoff filter, on complete rows only ------------------ */
        ck(rows("# time,mgdl\n1000,90\n2000,95\n3000,99\n", 2000)
               .equals("# time,mgdl\n2000,95\n3000,99\n"),
           "the cutoff drops old rows and keeps the header");
        ck(rows("1000,90\n2000,95\n30", 2000).equals("2000,95\n"),
           "a filtered export drops the torn tail as well");
        ck(rows("1000,90\n2000,95\n", 0).equals("1000,90\n2000,95\n"),
           "cutoff 0 keeps everything");
        ck(ExportSnapshot.rowKept("1755000000,x".getBytes(), 12, 1755000001L)
               == false,
           "a row older than the cutoff is dropped");
        ck(ExportSnapshot.rowKept("1755000000,x".getBytes(), 12, 1755000000L),
           "a row exactly at the cutoff is kept");
        ck(ExportSnapshot.rowKept("# header".getBytes(), 8, 1755000000L),
           "a header row is kept whatever the cutoff");

        /* ================================================================
         * ITEM 129: NOTHING IS PUBLISHED UNTIL THE OUTPUT HAS CLOSED
         * ============================================================== */

        Log log = new Log();
        FakeSink sink = new FakeSink(log);
        ck(ExportSnapshot.write(sink, new ExportSnapshot.Section[] {
               new Bytes("a\n", 0) }),
           "an export with rows in it publishes");
        ck(log.at("close") >= 0 && log.at("publish") > log.at("close"),
           "the snapshot is published AFTER the stream closed, never "
           + "before: close is where the last buffered block is written");
        ck(!log.has("discard"), "a published export leaves its bytes alone");
        ck(new String(sink.stream.got.toByteArray()).equals("a\n"),
           "and the bytes written are the rows");

        /* A CLOSE THAT FAILS IS A FAILED EXPORT. This is the mutant that
         * looks harmless -- the writes all succeeded, after all -- and it is
         * exactly how a snapshot missing its final block gets shared. */
        log = new Log();
        sink = new FakeSink(log);
        sink.failClose = true;
        threw = false;
        try {
            ExportSnapshot.write(sink, new ExportSnapshot.Section[] {
                new Bytes("a\n", 0) });
        } catch (IOException e) { threw = true; }
        ck(threw, "a close that fails fails the export");
        ck(!log.has("publish"),
           "a snapshot whose close failed is NEVER published");
        ck(log.has("discard"), "...and what was written is discarded");

        /* A WRITE THAT DIES PART WAY. The old code caught it broadly, logged
         * a line, and left the half-written file under the shareable name.
         *
         * BIG ENOUGH TO HAVE LEFT THE BUFFER, deliberately: a few bytes never
         * reach the stream until the close, so a small payload here would be
         * testing the close case again under another name. */
        StringBuilder many = new StringBuilder();
        for (int i = 0; i < 3000; i++) many.append("1755000000,101\n");
        log = new Log();
        sink = new FakeSink(log);
        sink.failAfter = 100;
        threw = false;
        try {
            ExportSnapshot.write(sink, new ExportSnapshot.Section[] {
                new Bytes(many.toString(), 0) });
        } catch (IOException e) { threw = true; }
        ck(threw, "a write that fails fails the export");
        ck(!log.has("publish"), "a half-written export is not published");
        ck(log.has("discard"), "...and is removed");

        /* THE INPUT SIDE IS CLOSED TOO, ON THE PATH THAT THROWS. That path
         * was the leak: the sources were closed only where nothing went
         * wrong, so the descriptor survived exactly the export that failed --
         * and an export is four sources every time. */
        log = new Log();
        sink = new FakeSink(log);
        DyingSource dying = new DyingSource(log);
        threw = false;
        try {
            ExportSnapshot.write(sink,
                new ExportSnapshot.Section[] { dying });
        } catch (IOException e) { threw = true; }
        ck(threw, "a source that dies mid-read fails the export");
        ck(dying.closed, "the source is closed even when its read threw");
        ck(!log.has("publish"), "and nothing is published");
        ck(log.has("discard"), "and nothing is left");

        log = new Log();
        sink = new FakeSink(log);
        DyingSource unopenable = new DyingSource(log);
        unopenable.openThrows = true;
        threw = false;
        try {
            ExportSnapshot.write(sink,
                new ExportSnapshot.Section[] { unopenable });
        } catch (IOException e) { threw = true; }
        ck(threw && !log.has("publish") && log.has("discard"),
           "a source that cannot be opened at all fails the same way");

        /* NOTHING TO SHARE IS NOT A SHARE. Every section absent, every row
         * filtered out, or one torn row and nothing else: each of them used
         * to produce a file of separator newlines with a perfectly good name
         * on it. */
        log = new Log();
        sink = new FakeSink(log);
        ck(!ExportSnapshot.write(sink, new ExportSnapshot.Section[] {
               new Absent(), new Absent() }),
           "an export of nothing publishes nothing");
        ck(!log.has("publish") && log.has("discard"),
           "...and leaves nothing behind either");

        log = new Log();
        sink = new FakeSink(log);
        ck(!ExportSnapshot.write(sink, new ExportSnapshot.Section[] {
               new Bytes("17550", 0) }),
           "a source that is nothing but a half-written row exports "
           + "nothing at all");
        ck(!log.has("publish"), "...and is not published as an empty file");

        log = new Log();
        sink = new FakeSink(log);
        ck(!ExportSnapshot.write(sink, new ExportSnapshot.Section[] {
               new Bytes("1000,90\n", 2000) }),
           "an export whose every row is older than the cutoff is empty");

        /* THE BLANK LINE GOES BETWEEN SECTIONS THAT EXIST. It is armed and
         * paid for on the next real write, so an absent section in the middle
         * cannot leave a stray blank line -- and, more importantly, an export
         * of nothing has length zero rather than length three. */
        log = new Log();
        sink = new FakeSink(log);
        ExportSnapshot.write(sink, new ExportSnapshot.Section[] {
            new Bytes("a\n", 0), new Bytes("b\n", 0) });
        ck(new String(sink.stream.got.toByteArray()).equals("a\n\nb\n"),
           "sections are separated by one blank line");

        log = new Log();
        sink = new FakeSink(log);
        ExportSnapshot.write(sink, new ExportSnapshot.Section[] {
            new Bytes("a\n", 0), new Absent(), new Bytes("b\n", 0) });
        ck(new String(sink.stream.got.toByteArray()).equals("a\n\nb\n"),
           "a section with nothing in it contributes no separator");

        log = new Log();
        sink = new FakeSink(log);
        ExportSnapshot.write(sink, new ExportSnapshot.Section[] {
            new Bytes("a\n", 0) });
        ck(new String(sink.stream.got.toByteArray()).equals("a\n"),
           "and the last section is not followed by one");

        /* THE SECTION THAT IS THERE AND SAYS NOTHING is the case that decides
         * whether the separator may be written eagerly: insulin.csv exists,
         * every row in it is older than the cutoff, and the export is glucose
         * and weight either side of it. Written on entry to a section, that
         * is two blank lines in the middle of the file -- which a reader
         * splitting sections on blank lines counts as an extra, empty
         * section. */
        log = new Log();
        sink = new FakeSink(log);
        ExportSnapshot.write(sink, new ExportSnapshot.Section[] {
            new Bytes("2000,x\n", 0), new Bytes("1000,old\n", 5000),
            new Bytes("3000,y\n", 0) });
        ck(new String(sink.stream.got.toByteArray()).equals("2000,x\n\n3000,y\n"),
           "a section that opens and contributes no rows contributes no "
           + "separator either");

        log = new Log();
        sink = new FakeSink(log);
        ck(!ExportSnapshot.write(sink, new ExportSnapshot.Section[] {
               new Bytes("17550", 0), new Bytes("1755", 0) }),
           "several sources that are all half-written rows still export "
           + "nothing");
        ck(sink.stream.got.size() == 0,
           "...and not a file made of separators");

        /* ================================================================
         * THE SAME RULES AGAINST A REAL FILESYSTEM
         *
         * The fakes prove the policy; these prove the FileSink that carries
         * it out -- the temporary's name, the rename, the read-only bit, and
         * what survives a failure. Everything happens inside one directory
         * this test made and removes.
         * ============================================================== */
        File dir = java.nio.file.Files.createTempDirectory("pancra-export")
                       .toFile();
        try {
            String snapName = BoundaryLogic.exportName(1755000000L, 0x0badf00dL);
            ck(BoundaryLogic.exportNameValid(snapName), "the test's own "
               + "snapshot name is a valid one");

            /* THE TEMPORARY CANNOT BE SERVED AND CANNOT BE RETIRED BY
             * MISTAKE: PancraFiles resolves only names the export grammar
             * accepts, and cleanupExports only deletes those. */
            ck(!BoundaryLogic.exportNameValid(
                   ExportSnapshot.partialName(snapName)),
               "the half-written file's name is NOT a name the provider "
               + "will resolve");
            ck(ExportSnapshot.isPartialName(
                   ExportSnapshot.partialName(snapName)),
               "but this side recognises it, so it can be swept");
            ck(!ExportSnapshot.isPartialName(snapName),
               "a finished snapshot is not mistaken for a temporary");
            ck(!ExportSnapshot.isPartialName("notes.txt.part"),
               "nor is any other file that happens to end in .part");

            File src = new File(dir, "readings.csv");
            put(src, "# t,mgdl\n1000,90\n2000,95\n30000".getBytes());

            /* A SUCCESSFUL EXPORT: the snapshot holds the complete rows, the
             * temporary is gone, and the file cannot be written again. */
            File dest = new File(dir, snapName);
            ck(dest.createNewFile(), "the name is reserved first, as "
               + "Ble.newSnapshot does");
            ExportSnapshot.FileSink fs = new ExportSnapshot.FileSink(dest);
            ck(ExportSnapshot.write(fs, new ExportSnapshot.Section[] {
                   new ExportSnapshot.FileSection(src, 0) }),
               "a real export publishes");
            ck(new String(slurp(dest)).equals("# t,mgdl\n1000,90\n2000,95\n"),
               "the published snapshot holds every complete row and the "
               + "half-written one it found is not in it");
            ck(!fs.temporary().exists(),
               "the temporary is gone once it has been published");
            ck(!dest.canWrite(),
               "a published snapshot is read-only: nothing may rewrite the "
               + "bytes a recipient was promised");

            /* A FAILING EXPORT LEAVES NO PUBLISHED FILE. The failure is a
             * source that exists and is not a regular file, which is a real
             * open error and not a simulated one -- and it happens AFTER a
             * section that already wrote bytes, so there is a partial
             * temporary at the moment it strikes. */
            final File dest2 =
                new File(dir, BoundaryLogic.exportName(1755000001L, 1));
            ck(dest2.createNewFile(), "second name reserved");
            final ExportSnapshot.FileSink fs2 = new ExportSnapshot.FileSink(dest2);
            File notAFile = new File(dir, "sub");
            ck(notAFile.mkdir(), "a source that is a directory");
            /* Big enough that the first section's bytes are on the disk and
             * not still in a buffer when the second section fails -- which is
             * the only arrangement in which "a partial file was published"
             * can be observed at all. */
            File bigsrc = new File(dir, "insulin.csv");
            put(bigsrc, many.toString().getBytes());
            /* WHERE THE BYTES ARE at the moment of the failure, measured
             * rather than argued: something must be in the temporary, and
             * NOTHING may be under the name the share sheet would hand out. */
            final long[] seen = { -1, -1 };
            ExportSnapshot.Section probe = new ExportSnapshot.Section() {
                @Override public InputStream open() {
                    seen[0] = fs2.temporary().length();
                    seen[1] = dest2.length();
                    return null;
                }
                @Override public long cutoff() { return 0; }
            };
            threw = false;
            try {
                ExportSnapshot.write(fs2, new ExportSnapshot.Section[] {
                    new ExportSnapshot.FileSection(bigsrc, 0),
                    probe,
                    new ExportSnapshot.FileSection(notAFile, 0) });
            } catch (IOException e) { threw = true; }
            ck(threw, "the export fails");
            ck(seen[0] > 0, "the bytes written so far were in the temporary");
            ck(seen[1] == 0, "and the shareable name held nothing at any "
               + "point during the write");
            ck(!dest2.exists(),
               "NOTHING EXISTS UNDER THE SHAREABLE NAME after a failed "
               + "export -- not a partial file, and not the empty one the "
               + "name was reserved with");
            ck(!fs2.temporary().exists(),
               "and the half-written temporary is removed, not left for a "
               + "later reader or a later mistake to find");

            /* AN EXPORT THAT CANNOT PUBLISH is a failed export: the rename
             * cannot replace a non-empty directory, which stands in here for
             * every reason a rename fails on a phone. */
            File dest3 = new File(dir, BoundaryLogic.exportName(1755000002L, 2));
            ck(dest3.mkdir() && new File(dest3, "x").createNewFile(),
               "a destination the rename cannot take");
            ExportSnapshot.FileSink fs3 = new ExportSnapshot.FileSink(dest3);
            threw = false;
            try {
                ExportSnapshot.write(fs3, new ExportSnapshot.Section[] {
                    new ExportSnapshot.FileSection(src, 0) });
            } catch (IOException e) { threw = true; }
            ck(threw, "a rename that fails fails the export");
            ck(!fs3.temporary().exists(),
               "and its temporary is removed too -- a snapshot that could "
               + "not be published is not left half-published");

            /* THE PROCESS THAT DIED BETWEEN THE WRITE AND THE RENAME. Its
             * temporary is still there; the next export sweeps it. Nothing
             * else in the directory is touched. */
            File orphan = new File(dir, ExportSnapshot.partialName(
                BoundaryLogic.exportName(1755000003L, 3)));
            put(orphan, "half a snapshot".getBytes());
            File stranger = new File(dir, "stelo.key");
            put(stranger, "not ours".getBytes());
            File dest4 = new File(dir, BoundaryLogic.exportName(1755000004L, 4));
            ck(dest4.createNewFile(), "fourth name reserved");
            ck(ExportSnapshot.write(new ExportSnapshot.FileSink(dest4),
                   new ExportSnapshot.Section[] {
                       new ExportSnapshot.FileSection(src, 0) }),
               "the next export runs normally");
            ck(!orphan.exists(),
               "and sweeps away the temporary a killed process left behind");
            ck(stranger.exists() && new String(slurp(stranger)).equals("not ours"),
               "while everything that is not an export temporary is left "
               + "strictly alone");
            ck(dest.exists(), "including the snapshots themselves");
        } finally {
            wipe(dir);
        }

        ck(checks > 0, "this suite ran at least one check");
        System.out.println("exportjavatest: " + checks + " checks passed");
    }
}
