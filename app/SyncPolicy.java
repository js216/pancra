// SPDX-License-Identifier: GPL-3.0
package com.jk.pancra;

/* ANDROID-FREE POLICY FOR THE SIGNED HTTP EXCHANGE: the body budget, the
 * deadline, what a redirect means, and the exchange itself. */
final class SyncPolicy {

    /* ==== BOUNDING AND TIMING THE SYNC EXCHANGE ========================
     *
     * PancraNet.syncHttp is the only caller; these rules are here because
     * they are the only parts of a socket exchange that can be RUN on a host
     * JVM, and a rule nobody can execute is a rule nobody has checked.
     *
     * ---- WHAT THE PERSON HOLDING THE PHONE WOULD SEE WITHOUT THEM ------
     *
     * Reading the response `while ((n = in.read(buf)) > 0)` into a
     * ByteArrayOutputStream with NO ceiling, and only afterwards handing the
     * finished array to native -- which measures it (syncjni.c, jni_http) and
     * returns -1 if it does not fit -- is a refusal that is real and three
     * copies too late: the growing BAOS, the `toByteArray()` duplicate, and
     * the byte[] handed across JNI. A server
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
    static final long SYNC_BODY_CAP = 256L * 1024;
    static final long SYNC_BODY_MAX = SYNC_BODY_CAP - 1;

    /* HOW LONG A WHOLE EXCHANGE MAY TAKE, end to end: connect, request body,
     * status line, response body, and every gap between them.
     *
     * 45 s, against a connect timeout of 8 s and a read idle timeout of 20 s.
     * It has to be comfortably larger than either or an ordinary slow mobile
     * link would be cut off mid-sync, and small enough that a wedged worker
     * frees itself while the phone is still the same phone. A restore issues
     * one request per missing bucket and each gets its own budget, so this is
     * not a ceiling on a restore -- only on any single request within it. */
    static final long SYNC_EXCHANGE_MS = 45000L;

    /* HAS THE BODY OUTGROWN WHAT WE WILL ACCEPT?
     *
     * `read` is the running total INCLUDING the bytes just read and NOT yet
     * appended to the buffer, so a true answer means "stop now, before this
     * chunk is kept" -- the excess byte is never stored. Strictly greater
     * than: `limit` bytes is the largest body native accepts, so a body of
     * exactly the limit is legal and must not be refused here. */
    static boolean bodyBudgetExceeded(long read, long limit) {
        return read > limit;
    }

    /* SHOULD WE REFUSE BEFORE READING A BYTE, on the strength of the declared
     * Content-Length?
     *
     * This is an OPTIMISATION AND NOT THE BOUND. A lying or absent
     * Content-Length must not be able to defeat anything, which is why
     * bodyBudgetExceeded above still counts every byte; this only saves the
     * app from opening a stream it already knows it will abandon.
     *
     * The header arrives as a raw string because that is what it is on the
     * wire, and because every interesting case here is a string that is not a
     * number:
     *
     *   - ABSENT (null) or empty: chunked encoding, or a server that did not
     *     say. Not refused -- the byte counter is the real bound.
     *   - NOT A PLAIN COUNT ("12, 12", "+5", "0x10", " 5 " with inner junk):
     *     not refused HERE. Deciding what a malformed length "really means"
     *     is how request smuggling gets in; we decline to guess and let the
     *     counter do its job.
     *   - LONGER THAN A long WILL HOLD ("9" * 25): REFUSED. Long.parseLong
     *     THROWS on that, and a bounds check that throws is a bounds check
     *     that did not happen -- the throw would have unwound into syncHttp's
     *     catch and been reported as a transport failure, which is the right
     *     answer for the wrong reason and stops being right the moment the
     *     parse moves. Digits that cannot fit in a long describe a body no
     *     phone will ever hold, so they are refused as a length, deliberately
     *     and by name. (A pathological "000...0100" is refused with them.
     *     Twenty-plus leading zeros is not a thing a real server sends, and
     *     refusing a sync is recoverable; the alternative is arithmetic on
     *     attacker-chosen digit strings.) */
    static boolean contentLengthRefused(String header, long limit) {
        if (header == null)
            return false;
        String s = header.trim();
        if (s.isEmpty())
            return false;
        for (int i = 0; i < s.length(); i++) {
            char ch = s.charAt(i);
            if (ch < '0' || ch > '9')
                return false; /* not a plain count: the counter decides */
        }
        if (s.length() > 19)
            return true; /* too many digits for a long, let alone a phone */
        try {
            return Long.parseLong(s) > limit;
        } catch (NumberFormatException e) {
            /* Nineteen digits still overflows above 9223372036854775807, so
             * the catch is needed even with the length guard above.
             *
             * THE TWO ARE REDUNDANT, DELIBERATELY, AND THE MUTATION DRILL
             * SAYS SO RATHER THAN PRETENDING OTHERWISE: removing the length
             * guard alone fails no assertion, because the parse then throws
             * and lands here with the same answer. Removing BOTH is caught
             * only by the NumberFormatException escaping the suite -- a
             * crash, not a named failure. The guard stays because the intent
             * ("digits that cannot describe a body are refused as a length")
             * has to survive the next edit of the parse, and because a bounds
             * check whose only correct path is an exception handler is one
             * refactor away from being a bounds check that throws. */
            return true;
        }
    }

    /* HOW MUCH OF THE BUDGET IS LEFT, in milliseconds, never negative.
     *
     * BOTH ARGUMENTS ARE System.nanoTime() READINGS AND NOTHING ELSE. This is
     * the lesson a wall-clock freshness test teaches: written against
     * currentTimeMillis went NEGATIVE when the wall clock stepped backwards
     * -- an NTP correction, a timezone database update, the user setting the
     * date -- and `now - stamp <= limit` was then true for a value that meant
     * "this happened in the future", so stale state read as fresh. Here the
     * same shape would be worse: a negative elapsed makes the REMAINING
     * budget larger than the budget, so a clock that moved would hand a
     * wedged request MORE time, which is precisely the hang this exists to
     * end. nanoTime has no epoch and no user-settable value, so it cannot
     * step; but the guard below stays anyway, because the next person to
     * touch this will not have read this paragraph.
     *
     * The elapsed value is computed as a SUBTRACTION and then compared to
     * zero, rather than comparing the two readings directly. nanoTime is
     * documented to be meaningful only as a difference -- it may start
     * anywhere, including near overflow -- and the difference stays correct
     * across the wrap where `now < start` does not. A non-positive elapsed
     * reading (backwards, or the same instant twice) yields ZERO REMAINING:
     * expired, refused, retried later. Refusing a sync that could have
     * succeeded costs one retry interval. Granting time to a request that
     * cannot finish costs every sync, pairing and restore from then on.
     *
     * AND IT IS CLAMPED AT ZERO RATHER THAN GOING NEGATIVE, which matters
     * for what this value is for next. A remaining budget is the natural
     * argument to setReadTimeout/setSoTimeout, and every Java socket API
     * reads a NEGATIVE timeout as an error and a ZERO one as INFINITE -- so
     * an overrun that reported "-3000 ms left" and was passed onward would
     * arm no timeout at all -- the very hang the budget exists to end,
     * reintroduced by the code that enforces it. deadlineExpired's `<= 0`
     * would mask a negative
     * return today, so the clamp is pinned by its own assertion in
     * boundaryjavatest and not left to be inferred from its only caller. */
    static long deadlineRemainingMs(long startMono, long nowMono,
                                    long budgetMs) {
        long elapsedNs = nowMono - startMono;
        if (elapsedNs < 0)
            return 0; /* time did not move forwards: no budget remains */
        long rem = budgetMs - elapsedNs / 1000000L;
        return rem > 0 ? rem : 0;
    }

    /* IS THE WHOLE-REQUEST DEADLINE UP? Non-positive remaining is expired,
     * so a budget of zero refuses immediately rather than meaning "for
     * ever" -- the reading of 0 that a missing configuration would produce. */
    static boolean deadlineExpired(long startMono, long nowMono,
                                   long budgetMs) {
        return deadlineRemainingMs(startMono, nowMono, budgetMs) <= 0;
    }

    /* HOW BIG A READ TO ASK FOR, so the refusal happens on the FIRST byte
     * past the limit rather than up to a bufferful later.
     *
     * Without this the loop reads 4096 at a time and notices the overrun only
     * once the whole chunk has landed, so "abort on the first excess byte"
     * would have been "abort within four kilobytes of it". Asking for exactly
     * one byte more than the budget allows makes the overrun detectable and
     * the buffer's peak exact: the accumulated body never exceeds the limit,
     * because the read that would have pushed it over is the read that ends
     * the exchange.
     *
     * Never returns less than 1 -- a read of 0 bytes returns 0, which the
     * loop's `> 0` condition reads as end of stream, and a truncated body
     * reported as a complete one is the exact failure mode syncjni.c's
     * oversize check was written to stop. */
    static int bodyReadSize(long read, long limit, int bufLen) {
        long want = limit - read + 1;
        if (want > bufLen)
            want = bufLen;
        if (want < 1)
            want = 1;
        return (int) want;
    }

    /* ---- THE READ LOOP ITSELF, WHERE A HOST JVM CAN RUN IT -------------
     *
     * This is deliberately NOT left in PancraNet.syncHttp. Everything that decides
     * whether a response is refused -- the byte counter, the declared length,
     * the deadline, the point at which accumulation stops -- operates on a
     * java.io.InputStream and a clock reading, and neither of those is an
     * Android type. Leaving the loop next to HttpURLConnection would have
     * made every one of the cases below testable only on a phone, which in
     * practice means untested; here `make boundaryjavatest` drives a stream
     * that dribbles one byte at a time, a server that lies about its
     * Content-Length, and a clock that runs backwards, on the host, with
     * assertions on where the reading actually stopped.
     *
     * What stays in PancraNet.syncHttp is the part that genuinely needs Android:
     * opening the connection, the connect/read idle timeouts, and the
     * watchdog that closes the socket when this deadline expires. */

    /* THE CLOCK, AS AN ARGUMENT. System.nanoTime() in production, a fake in
     * the test. Named `nanos` and not `now` because the unit is the whole
     * point: milliseconds here would be the wall clock, and the wall clock
     * answers a different question. */
    interface MonoClock {
        long nanos();
    }

    /* WHAT THE READ CAME TO. Four fields rather than a byte[] and a null,
     * because "it was refused" and "it was refused BEFORE anything was
     * allocated" are different claims and only the second one is worth
     * anything here.
     *
     *   body        the bytes, or null if the exchange was refused
     *   fail        NetPolicy.NET_OK, or the NET_* kind the screen should show
     *   consumed    bytes actually pulled off the stream
     *   accumulated bytes ever held in the growing buffer -- THE ABORT POINT
     *
     * `accumulated` exists for the assertion and for nothing else. Returning
     * null proves the caller was told no; it does not prove the heap was
     * spared, and the heap is the item. A test that reads this field is
     * asserting the thing the person holding the phone actually cares
     * about: that a hostile gigabyte never became a gigabyte in memory. */
    static final class BodyRead {
        final byte[] body;
        final int fail;
        final long consumed;
        final long accumulated;

        BodyRead(byte[] body, int fail, long consumed, long accumulated) {
            this.body = body;
            this.fail = fail;
            this.consumed = consumed;
            this.accumulated = accumulated;
        }
    }

    /* READ A RESPONSE BODY, BOUNDED IN SIZE AND IN TIME.
     *
     * The order of the three refusals is not arbitrary:
     *
     *   1. THE DEADLINE, FIRST AND BEFORE ANY I/O. If the budget is already
     *      gone by the time the body starts -- a connect that took nearly all
     *      of it, a request body that crawled -- there is no point opening the
     *      stream at all, and checking afterwards would let a request that was
     *      already over time spend another full read timeout getting there.
     *   2. THE DECLARED LENGTH, second, before the first read. Refusing here
     *      is what makes a well-behaved oversized reply cost nothing.
     *   3. THE BYTE COUNTER, on every chunk, which is THE ACTUAL BOUND. A
     *      server that declares 12 and sends ten megabytes is refused by this
     *      and only by this. Trusting the header alone is the obvious wrong
     *      fix and it is worse than no fix, because it looks like one.
     *
     * The deadline is re-checked EVERY iteration, not once. That is the
     * whole of the budget: setReadTimeout is per-read and restarts on every
     * byte, so a server sending one byte every nineteen seconds satisfies it
     * for ever. Here the elapsed time is measured against a fixed start, so
     * the dribble ends at the budget no matter how many bytes arrived.
     *
     * A REFUSAL RETURNS NO BYTES AT ALL, never a prefix. sync.c parses what
     * it gets; half a bucket that parses is worse than no bucket, and the
     * asymmetry rule in lib/wirevec.h says an implementation that cannot hold
     * a legal body must DECLINE rather than truncate. The partial buffer is
     * dropped on the floor. */
    static BodyRead readBoundedBody(java.io.InputStream in, String lenHeader,
                                    long limit, MonoClock clock,
                                    long startMono, long budgetMs)
        throws java.io.IOException {
        if (deadlineExpired(startMono, clock.nanos(), budgetMs))
            return new BodyRead(null, NetPolicy.NET_TIMEOUT, 0, 0);
        if (contentLengthRefused(lenHeader, limit))
            return new BodyRead(null, NetPolicy.NET_OTHER, 0, 0);
        if (in == null)
            return new BodyRead(new byte[0], NetPolicy.NET_OK, 0, 0);

        java.io.ByteArrayOutputStream out =
            new java.io.ByteArrayOutputStream();
        byte[] buf = new byte[4096];
        long consumed = 0;
        for (;;) {
            /* Ask for exactly one byte more than the budget allows, so the
             * overrun is detected on the first excess byte rather than up to
             * a bufferful past it. */
            int want = bodyReadSize(consumed, limit, buf.length);
            int n = in.read(buf, 0, want);
            if (n <= 0)
                break; /* end of stream: the body is whatever arrived */
            consumed += n;
            /* COUNT BEFORE APPENDING. The chunk that crosses the line is
             * never written, so `out` never holds more than `limit` bytes and
             * the peak allocation is the limit plus one 4 kB buffer -- not
             * whatever the server felt like sending. */
            if (bodyBudgetExceeded(consumed, limit))
                return new BodyRead(null, NetPolicy.NET_OTHER, consumed, out.size());
            /* AND CHECK THE CLOCK EVERY TIME AROUND. A body that stays under
             * the size limit for ever is still a body that never ends. */
            if (deadlineExpired(startMono, clock.nanos(), budgetMs))
                return new BodyRead(null, NetPolicy.NET_TIMEOUT, consumed, out.size());
            out.write(buf, 0, n);
        }
        return new BodyRead(out.toByteArray(), NetPolicy.NET_OK, consumed, out.size());
    }

    /* ---- 124: A SIGNED REQUEST GOES WHERE IT WAS SIGNED TO GO, OR NOWHERE
     *
     * Native signs ONE method and ONE target. sync.c builds the string that
     * the HMAC covers out of the verb and the path it is about to ask for,
     * and the server checks the signature against the verb and path it
     * actually received; that is the whole authentication. A redirect breaks
     * the assumption underneath it in two different ways, and both of them
     * end with this app reporting success for an operation that never
     * happened:
     *
     *   - THE TARGET MOVES. 301/302/307/308 name a new location, and
     *     HttpURLConnection follows it by default, with the same headers,
     *     to a host this app never signed anything for. The signature we
     *     computed for /v1/bucket/7 travels to wherever the redirect points.
     *     Either it is rejected there -- and the rejection is reported for
     *     the ORIGINAL request -- or, if the redirect came from something
     *     that speaks for the server, it is honoured somewhere we did not
     *     mean.
     *   - THE METHOD MOVES. A 303 turns ANY method into a GET by
     *     specification, and 301/302 have rewritten a body-carrying request
     *     into a GET in practice since Netscape. The uploads here are PUTs
     *     (sync.c: signed_req("PUT", ...)), so the request that carried a
     *     bucket of readings arrives at the new location as a fetch with no
     *     body, the fetch answers 200, and the follower reports that 200 as
     *     the outcome of the upload. sync.c's `!= 200` passes, the phone
     *     believes those rows are on the server, and DELETES ITS COPY. That
     *     is the failure mode: not a failed sync, a silently discarded one.
     *     307/308 exist precisely to preserve the method, which is what makes
     *     them the ones a rule written against 302 alone lets straight
     *     through -- and a preserved PUT sent to a destination we did not
     *     sign for is worse than a mangled one, not better.
     *
     * So: setInstanceFollowRedirects(false) in PancraNet.syncHttp -- `make
     * javacheck` fails the build without it -- and then EVERY 3xx is a
     * protocol failure here. Not the status, which native would publish as
     * the server's answer; a transport failure, meaning "the request did not
     * happen", which is exactly true.
     *
     * THE WHOLE RANGE, including 304. This app sends no conditional requests,
     * so a Not Modified is as much a server we do not understand as a 302 is,
     * and it carries no body to return. The rule is the range, because the
     * next status number somebody invents in it will not be in a list written
     * today. If a future policy wants to follow one, it has to hand the
     * validated target back to native and have the request RE-SIGNED for it;
     * following without re-signing is the bug. */
    static boolean redirectRefused(int status) {
        return status >= 300 && status < 400;
    }

    /* ---- 125: THE CONNECTION, AS LITTLE OF IT AS IS DECIDABLE ------------
     *
     * The shadow of the three HttpURLConnection calls that the exchange
     * actually depends on, so the ORDER of them -- and the closing of what
     * they hand back -- is a thing the host JVM can run and assert on.
     * Everything genuinely Android stays behind this interface in
     * PancraNet.syncHttp: https, the two idle timeouts, the redirect switch, the
     * header lines, and the watchdog that closes the socket.
     *
     * openResponse takes the status because the platform splits the body in
     * two: getInputStream() for a 2xx and getErrorStream() for everything
     * else, with the second returning null when there is no error body at
     * all. Both are the caller's problem, not this file's -- but both must be
     * CLOSED, which is this file's problem. */
    interface SyncConn {
        /* The request body sink, sized. Null means "no body to send". */
        java.io.OutputStream openRequest(int len) throws java.io.IOException;
        /* The status line. This is where the request is actually sent. */
        int status() throws java.io.IOException;
        /* A response header, or null. */
        String header(String name);
        /* The response body, or null when there is none. */
        java.io.InputStream openResponse(int status)
            throws java.io.IOException;
    }

    /* WHAT THE EXCHANGE CAME TO.
     *
     *   body  the response bytes, or null when the exchange was refused
     *   code  what PancraNet.sSyncCode must become -- the server's status, or -1
     *   fail  NetPolicy.NET_OK, or the NET_* kind the screen should show
     *
     * THE INVARIANT IS `code == -1 WHENEVER body == null`, and it is the
     * reason the status travels in here rather than being read off the
     * connection by the caller. Native reads syncCode() SEPARATELY from the
     * returned array (syncjni.c, jni_http), so a null body next to a live 200
     * is read there as a request that succeeded and returned nothing: an
     * upload we never completed treated as accepted, and -- for a bucket
     * fetch -- a bucket treated as EMPTY, which is the one answer that drives
     * the loop that deletes local rows. Refusing and publishing the status
     * are therefore not two decisions; they are one, and this type is where
     * they are made together. */
    static final class Exchange {
        final byte[] body;
        final int code;
        final int fail;

        Exchange(byte[] body, int code, int fail) {
            this.body = body;
            this.code = code;
            this.fail = fail;
        }
    }

    /* RUN ONE SIGNED REQUEST: send the body, get the status, read the reply.
     *
     * ---- WHY EVERY STREAM IS CLOSED --------------------------------------
     *
     * EVERY ONE OF THESE STREAMS IS CLOSED. It is tempting not to: write the
     * request body with `getOutputStream().write(body)` and leave it open,
     * read the response to EOF and leave it open, and close nothing in the
     * finally block, because disconnect() would throw away the pooled socket
     * and a sync is dozens of requests through one TLS handshake. Leaving the
     * socket in the pool is right. Leaving the STREAMS open is not the same
     * thing, and this is what it costs:
     *
     *   - A REQUEST BODY IS NOT SENT UNTIL IT IS CLOSED. close() is where the
     *     final flush happens, so it is also where "the server refused this
     *     upload" and "the link died mid-write" are reported. A close that
     *     is never called cannot report them; a close that is called and
     *     whose exception is swallowed reports them as success, which is
     *     worse. Hence try-with-resources, and hence no catch around it.
     *   - AND IT MUST BE CLOSED BEFORE THE RESPONSE IS TOUCHED. Reading the
     *     status with a request body still open is asking a server for its
     *     answer to a question we have not finished asking.
     *   - AN UNCLOSED RESPONSE POISONS THE POOL. The connection the next
     *     request in the same sync reuses is the one this request left with
     *     bytes still unread on it. Fully consumed and closed is reusable;
     *     abandoned and closed is discarded by the platform and a fresh one
     *     is dialled. Abandoned and NOT closed is neither -- it is a socket
     *     and a file descriptor held until the finalizer gets to it, on a
     *     path that runs once per bucket per sync.
     *
     * So every stream here is a try-with-resources, INCLUDING ON THE REFUSED
     * PATHS -- the 3xx above, and the over-limit abort, which is the
     * one that by construction leaves the most unread. Those two are the
     * paths that were never going to be closed by "read until EOF", because
     * they are the paths that stop early on purpose.
     *
     * IT THROWS RATHER THAN CLASSIFYING. An IOException out of any of this is
     * "the request did not happen", and PancraNet.syncHttp's catch already turns
     * that into a status of -1 and a named NET_* kind. Catching it here would
     * mean two places deciding what a failure means. */
    static Exchange runExchange(SyncConn conn, byte[] body, MonoClock clock,
                                long startMono, long budgetMs, long limit)
        throws java.io.IOException {
        /* `length > 0` AND NOT MERELY `!= null`. Native never sends a null:
         * jni_http builds a jbyteArray out of sync.c's `""` for every GET and
         * for the empty PUT that closes a bucket, so those arrive here as a
         * byte[0]. Opening a request body for them means setDoOutput(true)
         * with nothing to send, and setDoOutput(true) makes
         * HttpURLConnection send a POST -- so every signed GET would go out
         * as a verb it was not signed with -- the redirect failure above,
         * arriving by the other door. */
        if (body != null && body.length > 0) {
            /* CLOSED BY THE BRACE, and the close is inside the try, so an
             * exception from it is thrown and not swallowed. */
            try (java.io.OutputStream out = conn.openRequest(body.length)) {
                out.write(body);
            }
        }
        int status = conn.status();
        if (redirectRefused(status)) {
            /* NOT FOLLOWED, NOT REPORTED, BUT STILL CLOSED. A redirect
             * usually carries a short courtesy body, and unread-and-unclosed
             * is what the next request in this sync would inherit. Nothing
             * is read from it -- there is nothing in a redirect this app
             * would believe -- so this is a plain close rather than a
             * try-with-resources: there is no body between the open and the
             * close for a resource block to guard. The null test is the
             * platform's: getErrorStream() returns null when the response
             * carried nothing. */
            java.io.InputStream in = conn.openResponse(status);
            if (in != null)
                in.close();
            return new Exchange(null, -1, NetPolicy.NET_OTHER);
        }
        try (java.io.InputStream in = conn.openResponse(status)) {
            BodyRead r = readBoundedBody(in, conn.header("Content-Length"),
                                         limit, clock, startMono, budgetMs);
            /* THE STATUS TRAVELS ONLY WITH THE BYTES. See Exchange. */
            if (r.body == null)
                return new Exchange(null, -1, r.fail);
            return new Exchange(r.body, status, NetPolicy.NET_OK);
        }
    }
}
