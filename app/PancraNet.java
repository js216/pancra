// SPDX-License-Identifier: GPL-3.0
// PancraNet.java --- the sync transport: bytes on and off the wire
// Copyright 2026 Jakob Kastelic

/* THE SYNC-TRANSPORT ADAPTER. Native decides every byte on the wire (app/
 * sync.c); this moves them, because the platform's TLS is free here and a C
 * TLS stack would cost about a megabyte of library in a 143 kB app.
 *
 * It owns the worker the blocking protocol calls run on, the watchdog that
 * can cut a wedged exchange off, and the one HTTPS exchange itself. It was in
 * Ble.java, sharing a file with the GATT link table -- so a sync stuck inside
 * read() and a sensor that had stopped notifying were two states in one
 * class, with one set of static fields between them and no boundary a reader
 * could point at.
 *
 * WHAT IS NOT HERE: every decision that is arithmetic -- the deadline, the
 * body ceiling, the status mapping -- lives in SyncPolicy, where the host
 * JVM runs it against a dribbling stream and a lying Content-Length
 * (`make boundaryjavatest`). What is here is what genuinely needs Android.
 *
 * THE THREE NATIVE ENTRY POINTS IT DRIVES stay on Ble: RegisterNatives binds
 * them to that class (app/dexble.c), so that is where they must be declared.
 * Calling them from here is an ordinary static call. */
package com.jk.pancra;

import android.util.Log;

public final class PancraNet {
    private static final String TAG = "pancra";

    private PancraNet() { }


    /* One background thread with a small BOUNDED queue. Pushes must never block
     * the BLE binder thread that decoded the reading (it holds the driver lock),
     * and an unreachable server must not accumulate work without bound.
     *
     * THE QUEUE NEVER HOLDS MORE THAN ONE TASK, and that is a load-bearing
     * invariant, not an accident: all three submitters (remoteRange,
     * remoteCursor, remoteBatch) are `static synchronized` AND gated on
     * `sBusy != 0`, so at most one is ever in flight or queued. The capacity
     * and the discard policy below are therefore unreachable today.
     *
     * ANY NEW SUBMITTER MUST TAKE THE SAME sBusy GATE. Each of the three
     * clears sBusy in a `finally` INSIDE the submitted Runnable, and
     * DiscardOldestPolicy drops a task SILENTLY -- no exception, so the
     * submitting method's catch never runs. A fourth, ungated submitter that
     * filled the queue could therefore discard the Runnable carrying the
     * sBusy clear, wedging every future push for the life of the process
     * with nothing logged.
     */
    private static final java.util.concurrent.ThreadPoolExecutor pushExec =
        new java.util.concurrent.ThreadPoolExecutor(0, 1, 30,
            java.util.concurrent.TimeUnit.SECONDS,
            new java.util.concurrent.ArrayBlockingQueue<Runnable>(64),
            new java.util.concurrent.ThreadPoolExecutor.DiscardOldestPolicy());

    /* ---- HOW A READING REACHES THE SERVER ----
     *
     * Native asks the server what it already has (the CURSOR, GET
     * /api/last), then sends everything newer in chronological batches; a
     * batch that fails for ANY reason is retried later from the cursor, and
     * the server skips what it already stored, so a reply lost in flight
     * cannot double-write. A reading taken while the server is unreachable
     * therefore waits rather than being lost. See srv/logs.c.
     *
     * All of it runs on pushExec (never a BLE binder thread, which holds
     * the driver lock). Native drives one step per tick and reads the
     * state back through the three getters below -- no callbacks into C
     * from arbitrary threads beyond the existing onRemoteOk. */
    /* The tag of the last batch the server CONFIRMED, per set. Native passes
     * its outbox position as the tag and advances only when it comes back
     * here -- so a batch that failed, timed out, or whose reply was lost
     * leaves the position untouched and is simply resent. */
    /* What the LAST attempt actually got back. Visible only in logcat, a
     * server quietly refusing every batch looks exactly like a working link
     * that has nothing to send. */


    /* ---- there is no PULL direction ----
     * The server holds exactly what this phone gave it, so there is nothing
     * there to import. */

    /* ---- cloud sync: this class only moves bytes ----
     *
     * The protocol lives in native (src/sync.c); Java is here because the
     * platform's TLS is free and a C TLS stack would cost about a megabyte of
     * library in a 143 kB app. So: native decides every byte on the wire and
     * calls syncHttp to post it.
     *
     * syncRun/syncPair BLOCK -- they make several round trips -- so they are
     * only ever called on pushExec, never on a binder or UI thread. syncHttp
     * is then called back FROM that same worker thread, which is why nothing
     * here touches the main looper.
     */
    private static volatile int sSyncCode = -1;
    /* WHY the last one failed, when it failed before the server answered: a
     * NetPolicy.NET_* kind, read by native alongside the status. */
    private static volatile int sSyncFail = NetPolicy.NET_OK;
    private static volatile boolean sSyncBusy;

    /* THE THREE BLOCKING PROTOCOL CALLS are native and are declared on Ble,
     * because RegisterNatives binds them to that class (app/dexble.c). They
     * make several round trips each -- syncRestore one per missing bucket --
     * so they are only ever called on the worker below, never on a binder or
     * UI thread. */

    /* The status of the LAST syncHttp call, read by native straight after. */
    static int syncCode() { return sSyncCode; }

    /* ...and, when there was no status, WHY. Native turns this into the
     * outcome the screen shows and the scheduler retries (or does not). */
    static int syncFail() { return sSyncFail; }

    /* THE WATCHDOG THAT ACTUALLY CUTS A WEDGED EXCHANGE OFF.
     *
     * A DEADLINE IS ARITHMETIC, and arithmetic cannot interrupt a blocked
     * socket read. The loop in SyncPolicy re-checks the clock
     * every time around, which ends a server that DRIBBLES -- one byte,
     * another byte -- but a server that accepts the connection and then says
     * nothing at all leaves the worker parked inside a single blocking call,
     * where no amount of checking between calls is ever reached. (The read
     * idle timeout does cover that one case; the write does not have one at
     * all, and the gaps between connect, request and response are covered by
     * nothing.) So there is a timer, and when it fires it CLOSES THE SOCKET,
     * which is the only thing that makes a blocked read or write return.
     *
     * One shared thread, not one per request: a sync is dozens of requests
     * and a restore is one per bucket. Core size 1 with core-thread timeout
     * on, so the thread evaporates when nothing has synced for half a minute
     * and a phone that is not syncing pays nothing for it. Daemon, so it can
     * never be the reason a process lingers. removeOnCancel, because the
     * normal path cancels every single one of these and a queue that keeps
     * cancelled tasks until their deadline would hold a reference to every
     * connection of the whole sync.
     *
     * This is NOT the sync worker. It must not be: the whole point is that it
     * runs while pushExec's only thread is stuck. */
    private static final java.util.concurrent.ScheduledThreadPoolExecutor
        syncWatch = new java.util.concurrent.ScheduledThreadPoolExecutor(1,
            new java.util.concurrent.ThreadFactory() {
                public Thread newThread(Runnable r) {
                    Thread t = new Thread(r, "pancra-syncwatch");
                    t.setDaemon(true);
                    return t;
                }
            });
    static {
        syncWatch.setKeepAliveTime(30, java.util.concurrent.TimeUnit.SECONDS);
        syncWatch.allowCoreThreadTimeOut(true);
        syncWatch.setRemoveOnCancelPolicy(true);
    }

    /* ARM THE WATCHDOG: after `ms`, close this connection's socket.
     *
     * A SEPARATE METHOD SO IT CAN BE RUN, which is the whole reason it is not
     * still inline in syncHttp. Everything about the deadline that is
     * arithmetic between reads is host-tested in SyncPolicy; this is the
     * part that is not arithmetic at all -- a thread already parked inside
     * read() cannot be interrupted by any amount of checking, and the only
     * lever that reaches it is closing the socket underneath it. Whether that
     * lever actually works is a fact about the platform, not about this code,
     * so it is demonstrated rather than assumed:
     * test/app/SyncWatchTest.java drives it against a real server that
     * accepts a connection and then says nothing, by calling THIS method with
     * a short delay -- the same code the sync path arms, differing only in
     * the number.
     *
     * `cut` is set BEFORE the disconnect so the worker, whichever call it
     * unblocks from, can tell a socket we closed from one the peer or the
     * network dropped: the first is a timeout, the second is not. */
    static java.util.concurrent.ScheduledFuture<?> armSyncWatch(
            final java.net.HttpURLConnection conn,
            final java.util.concurrent.atomic.AtomicBoolean cut, long ms) {
        return syncWatch.schedule(new Runnable() {
            public void run() {
                cut.set(true);
                /* disconnect() closes the underlying socket, so whichever
                 * blocking call the worker is parked in returns with an
                 * IOException. It is the only lever that reaches a thread
                 * already inside read(). */
                try { conn.disconnect(); } catch (Throwable t) { }
            }
        }, ms, java.util.concurrent.TimeUnit.MILLISECONDS);
    }

    /* Called FROM native on the sync worker. Returns the response body, or
     * null; the status goes in sSyncCode, because a JNI call that returns two
     * things needs two calls and this is the cheaper pair.
     *
     * ---- WHY THE CEILING IS HERE AND NOT IN NATIVE --
     *
     * Reading the reply `while ((n = in.read(buf)) > 0)` into a
     * ByteArrayOutputStream with no ceiling and handing the finished array to
     * native -- which measures it and refuses it if it does not fit -- is a
     * refusal that is correct and three copies too late. A server that
     * answered a bucket fetch with a gigabyte, whether hostile,
     * misconfigured, or a captive portal serving an endless error page, got
     * this app to allocate until the heap gave out. An OutOfMemoryError does
     * not land where it was caused: it lands on the next allocation anywhere
     * in the process, which here is as likely to be the BLE callback decoding
     * a glucose reading. The screen stops updating, the low alarm stops
     * sounding, and nothing says anything is wrong -- because nothing knows.
     *
     * And the clock was per-read. setReadTimeout restarts on every byte, so a
     * server sending one byte every nineteen seconds never tripped it and
     * never finished. This method runs on pushExec, which has a MAXIMUM of
     * one thread and also carries pairing and restore, so one such server
     * stopped every sync, every pairing attempt and every restore for the
     * life of the process, silently, with the app believing a request was
     * merely still in progress.
     *
     * Both bounds now live in SyncPolicy, where the host JVM runs them
     * against a dribbling stream and a lying Content-Length. What is left
     * here is what needs Android: the connection, the two idle timeouts, and
     * the watchdog above. */
    static byte[] syncHttp(String server, int port, String method, String path,
                           String hdr, byte[] body) {
        sSyncCode = -1;
        sSyncFail = NetPolicy.NET_OK;
        /* MONOTONIC: this is an elapsed-time question, and
         * currentTimeMillis answers a different one. An NTP
         * correction or the user setting the date mid-sync would move a wall
         * clock, and a deadline computed from a clock that moved BACKWARDS
         * grows -- it would hand exactly the wedged request this exists to
         * kill even more time. nanoTime has no epoch and cannot be set. */
        final long startMono = System.nanoTime();
        /* Written by the watchdog thread, read by this one: a close that WE
         * caused must be reported as a timeout, not as whatever SocketException
         * the platform raises on a socket somebody yanked. */
        final java.util.concurrent.atomic.AtomicBoolean cut =
            new java.util.concurrent.atomic.AtomicBoolean(false);
        java.util.concurrent.ScheduledFuture<?> alarm = null;
        java.net.HttpURLConnection c = null;
        try {
            /* https, always: the session this signs is not the only thing on
             * the wire -- the record itself is. The platform supplies the TLS,
             * so this costs nothing but the scheme. */
            final java.net.HttpURLConnection conn =
                (java.net.HttpURLConnection) new java.net.URL(
                    "https", server, port, path).openConnection();
            c = conn;
            /* ARMED HERE, before a single byte moves. openConnection() does
             * not connect -- HttpURLConnection is lazy -- so starting the
             * timer at this line is what makes the budget cover the connect,
             * the TLS handshake, the request body, the status line, the
             * response body AND the gaps between them. Arming it after
             * getResponseCode(), which is the obvious-looking place, would
             * have left the connect and the write outside the deadline: the
             * connect has its own 8 s, but the write has NO timeout of any
             * kind, so a server that accepts a connection and never drains
             * the request would have hung exactly as before. */
            alarm = armSyncWatch(conn, cut, SyncPolicy.SYNC_EXCHANGE_MS);
            c.setConnectTimeout(8000);
            c.setReadTimeout(20000);
            c.setRequestMethod(method);
            c.setUseCaches(false);
            /* NO AUTOMATIC REDIRECTS, and one line is the whole of it.
             * HttpURLConnection follows 3xx by default, and a followed
             * redirect changes the target -- or the METHOD -- of a request
             * whose signature covers exactly the method and path native
             * chose. The 2xx that comes back from wherever it went is then
             * reported as the answer to the request we signed. See
             * SyncPolicy.redirectRefused for what that costs; the refusal
             * of every 3xx lives there, and `make javacheck` fails the build
             * if this line goes away. */
            c.setInstanceFollowRedirects(false);
            if (hdr != null) {
                /* "Name: value\r\n" lines, exactly as native built them. */
                for (String line : hdr.split("\r\n")) {
                    int colon = line.indexOf(':');
                    if (colon > 0)
                        c.setRequestProperty(line.substring(0, colon),
                                             line.substring(colon + 1).trim());
                }
            }
            /* THE LIMIT IS NATIVE'S LIMIT. SyncPolicy.SYNC_BODY_MAX is
             * SYNC_BUF_MAX - 1 from app/sync.h, which is the `outcap` every
             * sync.c call site passes to jni_http and the exact length that
             * jni_http still accepts. `make javacheck` fails the build if the
             * two ever disagree, so this is one number with a copy the build
             * will not let rot -- not a second, independent guess.
             *
             * The exchange itself -- write the body and CLOSE it, then the
             * status, then refuse a 3xx, then the bounded read, with every
             * stream closed on every path -- is SyncPolicy.runExchange,
             * for the same reason the read loop is: none of those decisions
             * needs Android, and on this side of the boundary they would be
             * testable only on a phone. What is left below is the wiring the
             * platform actually owns, including WHICH stream carries the body
             * (getInputStream for a 2xx, getErrorStream for the rest, and
             * getErrorStream is null when there is no error body). */
            SyncPolicy.Exchange x = SyncPolicy.runExchange(
                new SyncPolicy.SyncConn() {
                    public java.io.OutputStream openRequest(int len)
                            throws java.io.IOException {
                        conn.setDoOutput(true);
                        conn.setFixedLengthStreamingMode(len);
                        return conn.getOutputStream();
                    }
                    public int status() throws java.io.IOException {
                        return conn.getResponseCode();
                    }
                    public String header(String name) {
                        return conn.getHeaderField(name);
                    }
                    public java.io.InputStream openResponse(int status)
                            throws java.io.IOException {
                        return (status / 100 == 2) ? conn.getInputStream()
                                                   : conn.getErrorStream();
                    }
                },
                body, new SyncPolicy.MonoClock() {
                    public long nanos() { return System.nanoTime(); }
                },
                startMono, SyncPolicy.SYNC_EXCHANGE_MS,
                SyncPolicy.SYNC_BODY_MAX);
            if (x.body == null || cut.get()) {
                /* SET THE STATUS BACK TO -1, and this line is load-bearing.
                 * Native reads syncCode() separately from the returned array
                 * (syncjni.c, jni_http): a null return with sSyncCode still
                 * holding the 200 we got a moment ago is read there as a
                 * successful request with an EMPTY body -- an upload we never
                 * completed treated as accepted, a bucket fetch that returned
                 * nothing treated as a bucket that IS empty, which is the one
                 * kind of answer that drives the loop that deletes. A refused
                 * body is "nothing happened", and nothing happened is -1.
                 *
                 * IT IS ALSO BELT AND BRACES NOW, DELIBERATELY. Since item
                 * 124 the status is not published until there are bytes to
                 * publish it with -- runExchange returns code -1 with every
                 * null body, and the assignment below is the only one -- so
                 * this line writes -1 over the -1 set on entry. It stays
                 * because the invariant it enforces is one line's edit away
                 * from being lost: the obvious "simplification" here is to
                 * read the status straight off the connection as soon as it
                 * is known, which reports the status of an exchange that did
                 * not finish. */
                sSyncCode = -1;
                sSyncFail = cut.get() ? NetPolicy.NET_TIMEOUT : x.fail;
                return null;
            }
            sSyncCode = x.code;
            return x.body;
        } catch (Throwable t) {
            /* Any failure is "the request did not happen": native retries the
             * whole sync later, and a partial answer must never look like a
             * complete one. But WHICH failure is not lost: a name that does
             * not resolve and a refused certificate are the user's to fix,
             * and retrying them for ever helps nobody. */
            sSyncCode = -1;
            /* A socket WE closed reports as a timeout. netFailure would see a
             * SocketException reading "Socket closed" and call it NET_OTHER,
             * which on the screen is an unexplained failure rather than "that
             * server is too slow" -- and NET_OTHER is retried on a schedule
             * that assumes a transient fault. */
            sSyncFail = cut.get() ? NetPolicy.NET_TIMEOUT
                                  : NetPolicy.netFailure(t);
            return null;
        } finally {
            if (alarm != null)
                alarm.cancel(false);
            /* NOT disconnect() ON THE NORMAL PATH: that tears the socket down,
             * and the socket is the expensive part. A TLS handshake to the
             * server costs 100-600 ms and the request itself costs 5, so a
             * sync that reconnects per bucket spends all its time shaking
             * hands. Leaving the connection in the pool lets the whole sync
             * ride one handshake.
             *
             * A connection the watchdog cut is the exception, and it is
             * already gone -- disconnect() is what the watchdog called. That
             * one must NOT go back in the pool: it is attached to a server
             * that just demonstrated it does not finish, and the next request
             * to reuse it would inherit the stall.
             *
             * AND THE STREAMS ARE CLOSED, which is the other half. Not
             * disconnecting is what keeps the socket; closing the request and
             * response streams is what makes keeping it correct, because a
             * connection with unread bytes still on it is not a connection
             * the next request may use. runExchange closes both on every
             * path, consumed or aborted, so what this block leaves in the
             * pool is a connection that is actually reusable rather than one
             * held open by a stream nobody ever finished with. There is
             * still nothing to do HERE: the closing belongs where the streams
             * are opened, inside try-with-resources, not in a finally that
             * would have to re-derive which of them exist. */
        }
    }

    /* Ask for a sync. Coalesced: a second request while one is running is
     * dropped rather than queued, because a sync always sends whatever is
     * current -- running it twice back to back would achieve nothing.
     *
     * RETURNS WHETHER THE WORKER TOOK IT, and false for that coalesced drop.
     * The native scheduler advances its "we have synced up to here" stamp
     * when it asks, so a dropped ask reported as success leaves it believing
     * the newest data is on its way when nothing was queued -- and with
     * nothing new arriving afterwards, the next attempt waits out the
     * six-hour safety interval. A drop is not a failure of the sync, but it
     * IS a failure of the request, and only the caller can tell the
     * difference. */
    static boolean syncSoon() {
        if (sSyncBusy)
            return false;
        sSyncBusy = true;
        pushExec.execute(new Runnable() {
            public void run() {
                try { Ble.syncRun(); } catch (Throwable t) { /* reported native */ }
                finally { sSyncBusy = false; }
            }
        });
        return true;
    }

    /* A restore is user-initiated from the REMOTE screen and reports the same
     * way a sync does, through the LAST STATUS row. Not gated on sSyncBusy: a
     * restore and a sync must not overlap, and the worker is single-threaded,
     * so the queue already serialises them. */
    static void syncRestoreSoon() {
        pushExec.execute(new Runnable() {
            public void run() {
                try { Ble.syncRestore(); } catch (Throwable t) { /* reported native */ }
            }
        });
    }

    /* Pairing is user-initiated and its result is shown on screen, so it runs
     * on the same worker but reports through onSyncPaired. */
    static void syncPairSoon(final String email, final String code) {
        pushExec.execute(new Runnable() {
            public void run() { Ble.syncPair(email, code); }
        });
    }
}
